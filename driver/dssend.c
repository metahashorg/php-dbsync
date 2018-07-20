#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

#include "dsmisc.h"
#include "dspack.h"
#include "dscrypto.h"



#define CONNECTION_TIMEOUT_MS 3000


enum { DSSTATE_0 = 0, DSSTATE_CONN, DSSTATE_OUT, DSSTATE_IN, DSSTATE_ERR, DSSTATE_FIN };
static const char *state_strings[] = {"DSSTATE_0", "DSSTATE_CONN", "DSSTATE_OUT", "DSSTATE_IN", "DSSTATE_ERR", "DSSTATE_FIN"};

typedef struct _dsconn {
  char  *address;
  int   port;
  int   sockfd;

  char buf[64];
  unsigned char *respkt;
  int respkt_size;
  int expected_size;
  int send_offset;
  int read_offset;
  int iostate;
  int inpoll;

  // managed in head instance
  int h_epollfd;
  struct epoll_event *h_epevents;
  int h_conns_num;
  int h_active_num;

  struct _dsconn *head;
  struct _dsconn *next;
} DSCONN, *PDSCONN;


void reset_connection(PDSCONN ctx)
{
  if(ctx->respkt)
    free(ctx->respkt);
  ctx->respkt = NULL;
  ctx->respkt_size = 0;
  ctx->expected_size = -1;
  ctx->send_offset = 0;
  ctx->read_offset = 0;
}


// returns 1 for need of polling, -1 for error, 0 for success
int create_connection(PDSCONN ctx)
{
  struct sockaddr_in serv_addr;

  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(ctx->port);
  
  dstrace("Create connection to %s:%d", ctx->address, ctx->port);

  if(inet_pton(AF_INET, ctx->address, &serv_addr.sin_addr) <= 0)
  {
    dslogerr(errno, "Bad address '%s'", ctx->address);
    return -1;
  }

  ctx->sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); 
  if(ctx->sockfd < 0) 
  {
    dslogerr(errno, "Fail opening socket");
    return -1;
  }

  if(connect(ctx->sockfd,(struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
  {
    if(errno != EINPROGRESS)
    {
      dslogwerr(errno, "Cannot connect\n");
      return -1; // sockfd will be closed in ctx_release
    }
    else
    {
      dstrace("Connect on hold to poll");
      return 1;
    }
  }

  return 0;
}


int sendbuf(int sockfd, const void *buf, int *send_size)
{
  int rc, buf_size = *send_size;
  *send_size = 0;

  while(*send_size < buf_size)
  {
    rc = send(sockfd, buf + *send_size, buf_size - *send_size, 0);

    if(rc > 0)
    {
      dstrace("Sent %d", rc);
      *send_size += rc;
    }
    else if(rc < 0)
    {
      if(errno != EWOULDBLOCK && errno != EAGAIN)
      {
        dslogwerr(errno, "Data send error");
        return -1;
      }

      dstrace("Send on hold to poll");
      break;
    }
    else // if(rc == 0)
    {
      dstrace("Connection closed while sending");
      return -1;
    }
  }
  
  return 0;
}


int sendpack(PDSCONN ctx, const char *msg, int size)
{
  int send_size = size - ctx->send_offset;
  if(sendbuf(ctx->sockfd, msg + ctx->send_offset, &send_size))
    return -1;
  
  ctx->send_offset += send_size;
  
  if(ctx->send_offset < size)
    return 1;

  return 0;
}


int readbuf(int sockfd, void *buf, int *read_size)
{
  int rc, buf_size = *read_size;
  *read_size = 0;

  while(*read_size < buf_size)
  {
    rc = recv(sockfd, buf + *read_size, buf_size - *read_size, 0);
    if(rc > 0)
    {
      dstrace("Received answer chunk %d size", rc);
      *read_size += rc;
    }
    else if(rc < 0)
    {
      if(errno != EWOULDBLOCK && errno != EAGAIN)
      {
        dstracerr(errno, "Error to read the result from service");
        return -1;
      }

      dstrace("Read on hold to poll");
      break;
    }
    else // rc = 0
    {
      dstrace("Closed connection while reading the result from service");
      return -1;
    }
  }
  
  return 0;
}


int readpack(PDSCONN ctx)
{
  if(ctx->expected_size < 0)
  {
    int read_size = sizeof(ctx->buf) - ctx->read_offset;
    if(readbuf(ctx->sockfd, ctx->buf + ctx->read_offset, &read_size) < 0)
      return -1;

    ctx->read_offset += read_size;

    if(dspack_bufsize("ds", ctx->buf, ctx->read_offset, &ctx->expected_size) < 0)
      return -1;
  }

  if(ctx->expected_size >= 0)
  {
    dstrace("Expected data size %d", ctx->expected_size);

    char *data = (char *)malloc(ctx->expected_size);
    if(!data)
    {
      dslogerr(errno, "Cannot allocate result buffer");
      return -1;
    }

    memcpy(data, ctx->buf, ctx->read_offset);

    int read_size = ctx->expected_size - ctx->read_offset;
    int rc = readbuf(ctx->sockfd, data + ctx->read_offset, &read_size);

    ctx->read_offset += read_size;

    if(ctx->read_offset == ctx->expected_size)
    {
      dstrace("data read OK");
      ctx->respkt = data;
      ctx->respkt_size = ctx->expected_size;
      return 0;
    }
    else if(rc < 0)
    {
      dslogw("data read failed, read size %d", read_size);
      free(data);
      return -1;
    }
  }
  
  return 1;
}


const char* getstate_string(int iostate)
{
  if(iostate >= DSSTATE_0 && iostate <= DSSTATE_FIN)
    return state_strings[iostate];
  else
    return "<ABNORMAL STATE>";
}


int poll_add(int epoll_fd, int sockfd, void *data, int event)
{
  dstrace("epoll add the connection %d to %d", sockfd, epoll_fd);

  struct epoll_event ev = { 0 };
  ev.data.ptr = data;
  ev.events = event;

  if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sockfd, &ev))
  {
    dslogerr(errno, "Cannot epoll add the connection %d to %d", sockfd, epoll_fd);
    return -1;
  }
  
  return 0;
}


int poll_mod(int epoll_fd, int sockfd, void *data, int event)
{
  dstrace("epoll mod the connection %d in %d", sockfd, epoll_fd);

  struct epoll_event ev = { 0 };
  ev.data.ptr = data;
  ev.events = event;

  if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sockfd, &ev))
  {
    dslogerr(errno, "Cannot epoll mod the connection %d in %d", sockfd, epoll_fd);
    return -1;
  }
  
  return 0;
}


int poll_del(int epoll_fd, int sockfd)
{
  dstrace("epoll del the connection %d in %d", sockfd, epoll_fd);

  if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sockfd, NULL))
  {
    dslogerr(errno, "Cannot epoll del the connection %d in %d", sockfd, epoll_fd);
    return -1;
  }
  
  return 0;
}


void setstate_connection(PDSCONN ctx, int iostate)
{
  int rc = 0;

  dstrace("Set connection %d(%s:%d) state to %s", ctx->sockfd, ctx->address, ctx->port, getstate_string(iostate));

  switch(iostate)
  {
    case DSSTATE_0:
      if(ctx->inpoll)
      {
        poll_del(ctx->head->h_epollfd, ctx->sockfd);
        ctx->inpoll = 0;
        ctx->head->h_active_num--;
      }
      break;

    case DSSTATE_CONN:
      if(ctx->iostate != DSSTATE_0)
        dsdie("Abnormal %s connection state transition from %s", getstate_string(iostate), getstate_string(ctx->iostate));

      if(ctx->inpoll)
      {
        rc = poll_mod(ctx->head->h_epollfd, ctx->sockfd, ctx, EPOLLOUT);
      }
      else
      {
        rc = poll_add(ctx->head->h_epollfd, ctx->sockfd, ctx, EPOLLOUT);
        ctx->head->h_active_num++;
        ctx->inpoll = 1;
      }
      break;

    case DSSTATE_OUT:
      if(ctx->iostate != DSSTATE_CONN && ctx->iostate != DSSTATE_FIN)
        dsdie("Abnormal %s connection state transition from %s", getstate_string(iostate), getstate_string(ctx->iostate));

      if(ctx->iostate == DSSTATE_FIN)
      {
        if(!ctx->inpoll)
          rc = poll_add(ctx->head->h_epollfd, ctx->sockfd, ctx, EPOLLOUT);
        else
          dsdie("Abnormal %s connection state", getstate_string(iostate));
        ctx->head->h_active_num++;
        ctx->inpoll = 1;
      }
      break;

    case DSSTATE_IN:
      if(ctx->iostate != DSSTATE_OUT)
        dsdie("Abnormal %s connection state transition from %s", getstate_string(iostate), getstate_string(ctx->iostate));

      if(ctx->inpoll)
        rc = poll_mod(ctx->head->h_epollfd, ctx->sockfd, ctx, EPOLLIN);
      else
        dsdie("Abnormal %s connection state", getstate_string(iostate));
      break;

    case DSSTATE_ERR:
      if(ctx->inpoll)
      {
        poll_del(ctx->head->h_epollfd, ctx->sockfd);
        ctx->inpoll = 0;
        ctx->head->h_active_num--;
      }
      break;

    case DSSTATE_FIN:
      if(ctx->inpoll)
      {
        poll_del(ctx->head->h_epollfd, ctx->sockfd);
        ctx->inpoll = 0;
        ctx->head->h_active_num--;
      }
      break;
  }

  if(!rc)
  {
    ctx->iostate = iostate;
  }
  else
  {
    dstrace("Error detected in connection state manipulation");

    ctx->iostate = DSSTATE_ERR;
    poll_del(ctx->head->h_epollfd, ctx->sockfd);
    ctx->head->h_active_num--;
  }
}


void process_connection(PDSCONN ctx, const char *pkt, int pkt_size)
{
  int rc = 0;

  if(!pkt_size)
    dsdie("Sending nothing is prohibitted");
  
  dstrace("Processing connection %d(%s:%d)", ctx->sockfd, ctx->address, ctx->port);

  if(ctx->iostate == DSSTATE_0)
  {
    dstrace("Zero state");

    rc = create_connection(ctx);
    if(rc > 0)
      setstate_connection(ctx, DSSTATE_CONN);
    else if(rc == 0)
      setstate_connection(ctx, DSSTATE_OUT);
    else
      setstate_connection(ctx, DSSTATE_ERR);
  }

  if(ctx->iostate == DSSTATE_OUT)
  {
    dstrace("Outgoing state");

    rc = sendpack(ctx, pkt, pkt_size);
    if(!rc)
      setstate_connection(ctx, DSSTATE_IN);
    else if(rc < 0)
      setstate_connection(ctx, DSSTATE_ERR);
  }

  if(ctx->iostate == DSSTATE_IN)
  {
    dstrace("Incoming state");

    rc = readpack(ctx);
    if(!rc)
      setstate_connection(ctx, DSSTATE_FIN);
    else if(rc < 0)
      setstate_connection(ctx, DSSTATE_ERR);
  }
}


int poll_connections(PDSCONN head, void *pkt, int pkt_size)
{
  if(!head->h_active_num)
  {
    dstrace("Nothing to poll");
    return -1;
  }

  dstrace("Polling connections");
  int nfds = epoll_wait(head->h_epollfd, head->h_epevents, head->h_conns_num, CONNECTION_TIMEOUT_MS);
  if(nfds < 0)
  {
    dslogerr(errno, "epoll wait error");
    return -1;
  }
  if(nfds == 0)
  {
    dslogw("%d connections timeout", head->h_active_num);
    return -1;
  }

  for(int i = 0; i < nfds; i++)
  {
    PDSCONN ctx = (PDSCONN)head->h_epevents[i].data.ptr;

    if(head->h_epevents[i].events & (EPOLLERR|EPOLLHUP))
    {
      setstate_connection(ctx, DSSTATE_ERR);
    }
    else
    {
      if(ctx->iostate == DSSTATE_CONN)
        setstate_connection(ctx, DSSTATE_OUT);

      process_connection(ctx, pkt, pkt_size);
    }

    if(ctx->iostate == DSSTATE_ERR)
    {
      dstrace("Abort all connections processing because of single connection error");

      ctx = head;
      while(ctx)
      {
        ctx->iostate = DSSTATE_ERR;
        ctx = ctx->next;
      }

      return -1;
    }
  }

  return 0;
}


void dssend(void *dsctx, int pack_signed, int keepalive, const char *msg, char **res, int *res_size)
{
  PDSCONN ctx;
  int rc, pack_options = 0;

  if(keepalive)
    dstrace("Sending via keepalive connection");
  else
    dstrace("Sending via non-keepalive connection");

  if(pack_signed)
  {
    dstrace("Sending signed packets");
    pack_options |= DSPACK_SIGNED;
  }
  else
    dstrace("Sending non-signed packets");

  
  // prepare packet
  void *pkt = NULL;
  int pkt_size = 0;
  int len = strlen(msg);
  rc = dspack("ds", msg, len + 1, &pkt, &pkt_size, pack_options); // add trailing 0 symbol
  if(rc)
  {
    dslog("Error: Packing failed");
    return;
  }

  
  // init connections
  ctx = (PDSCONN)dsctx;
  while(ctx)
  {
    if(ctx->iostate == DSSTATE_0)
      process_connection(ctx, pkt, pkt_size);
    ctx = ctx->next;
  }

  
  // send+recv loop
  while(!poll_connections((PDSCONN)dsctx, pkt, pkt_size));


  ctx = (PDSCONN)dsctx;
  // Build result
  *res = NULL;
  *res_size = 0;
  if(ctx->respkt)
  {
    dsunpack("ds", ctx->respkt, ctx->respkt_size, (const void **)res, res_size, 0);
    *res = strdup(*res); // pointer inside existing buffer respkt which will be freed
  }

  // analyse results
  while(ctx)
  {
    if(!rc)
    {
      if(!ctx->respkt || ctx->respkt_size == 0)
      {
        dslogw("DB %s:%d returns no result", ctx->address, ctx->port);
        rc = -1;
      }
      else if(ctx->next &&
          (
            ctx->respkt_size != ctx->next->respkt_size || 
            memcmp(ctx->respkt, ctx->next->respkt, ctx->respkt_size)
          )
        )
      {
        dslogw("DBs (%s:%d vs %s:%d) returns different results", ctx->address, ctx->port, ctx->next->address, ctx->next->port);
        rc = -1;
      }
    } // if !rc

    if(keepalive)
    {
      if(ctx->iostate == DSSTATE_FIN)
      {
        reset_connection(ctx);
        setstate_connection(ctx, DSSTATE_OUT);
      }
      else if(ctx->iostate != DSSTATE_ERR)
        setstate_connection(ctx, DSSTATE_ERR);
    }
    else
    {
      setstate_connection(ctx, DSSTATE_0);
      if(ctx->sockfd > 0)
      {
        dstrace("Close connection %d because no keepalive", ctx->sockfd);
        close(ctx->sockfd);
        ctx->sockfd = -1;
      }
    }

    ctx = ctx->next;
  }
  
  if(rc && *res)
  {
    free(*res);
    *res = NULL;
    *res_size = 0;
  }

  if(pkt)
    free(pkt);
}


void* dssend_init_ctx(const char *targets)
{
  PDSCONN head = NULL, curr;
  int rc = 0;

  dstrace("Init context for %s", targets);

  // Loop through targets
  char *_targets = strdup(targets);
  const char *pos = _targets;
  while(pos && !rc)
  {
    char *s1 = strchr(pos, ',');
    char *s2 = strchr(pos, ':');
    if(!s2 || (s1 && (s2 > s1)))
    {
      dslog("Bad target format %s", pos);
      rc = -1;
    }
    else
    {
      s2[0] = 0;
      const char *address = pos;
      if(s1)
        s1[0] = 0;
      const char *port = s2+1;
      
      if(!head)
      {
        head = (PDSCONN)malloc(sizeof(DSCONN));
        if(!head)
        {
          dslogerr(errno, "Cannot allocate first send context");
          rc = -1;
          break;
        }
        
        head->h_conns_num = 0;
        head->h_active_num = 0;

        curr = head;
      }
      else
      {
        curr->next = (PDSCONN)malloc(sizeof(DSCONN));
        if(!curr->next)
        {
          dslogerr(errno, "Cannot allocate send context");
          rc = -1;
          break;
        }
        curr = curr->next;
      }

      dstrace("Init connection context %s:%s", address, port);

      curr->address = strdup(address);
      curr->port = atoi(port);
      curr->sockfd = -1;

      curr->inpoll = 0;
      curr->iostate = -1;

      curr->head = head;
      curr->next = NULL;
      head->h_conns_num++;

      reset_connection(curr);
      setstate_connection(curr, DSSTATE_0);
    }

    if(s1)
      pos = s1+1;
    else
      pos = NULL;
  };

  if(_targets)
    free(_targets);

  if(!rc && head)
  {
    head->h_epollfd = epoll_create(1);
    if(head->h_epollfd < 0)
    {
      dslogerr(errno, "Cannot create epoll instance");
      rc = -1;
    }
    else
    {
      head->h_epevents = (struct epoll_event *)calloc(head->h_conns_num, sizeof(struct epoll_event));
      if(!head->h_epevents)
      {
        dslogerr(errno, "Cannot allocate epoll events buf");
        rc = -1;
      }
    }
  }

  if(rc && head)
  {
    if(head->h_epollfd >= 0)
      close(head->h_epollfd);

    while(head)
    {
      curr = head;
      head = head->next;
      free(curr->address);
      free(curr);
    }
  }

  return head;
}


void dssend_release_ctx(void *dsctx)
{
  PDSCONN head, curr;

  dstrace("Release context");

  head = (PDSCONN)dsctx;

  if(head)
  {
    if(head->h_epollfd > 0)
      close(head->h_epollfd);
    if(head->h_epevents)
      free(head->h_epevents);
  }

  while(head)
  {
    curr = head;
    head = head->next;

    if(curr->sockfd >= 0)
    {
      dstrace("Close connection %d in dbsync context release", curr->sockfd);
      close(curr->sockfd);
    }

    free(curr->address);
    free(curr->respkt);
    free(curr);
  }
}
