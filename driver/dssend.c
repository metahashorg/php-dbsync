#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

#include "dsmisc.h"
#include "dspack.h"
#include "dscrypto.h"



#define CONNECTION_TIMEOUT_MS 3000


enum { DSSTATE_0 = 0, DSSTATE_CONN, DSSTATE_OUT, DSSTATE_IN, DSSTATE_ERR, DSSTATE_FIN };
static const char *state_strings[] = {"DSSTATE_0", "DSSTATE_CONN", "DSSTATE_OUT", "DSSTATE_IN", "DSSTATE_ERR", "DSSTATE_FIN"};

typedef struct _dstarget {
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
  int ioready;
  int ind;

  // managed in head instance
  int h_conns_num;
  struct pollfd *h_pollfds;

  struct _dstarget *next;
} DSTARGET, *PDSTARGET;


// returns 1 for need of polling, -1 for error, 0 for success
int create_connection(PDSTARGET ctx)
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


int sendpack(PDSTARGET ctx, const char *msg, int size)
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


int readpack(PDSTARGET ctx)
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


void setstate_connection(PDSTARGET ctx, int iostate)
{
  dstrace("Set connection %s:%d state to %s", ctx->address, ctx->port, getstate_string(iostate));
  ctx->iostate = iostate;
}


void process_connection(PDSTARGET ctx, const char *pkt, int pkt_size)
{
  int rc = 0;

  if(!pkt_size)
    die("Sending nothing is prohibitted");
  
  dstrace("Sending to \"%s:%d\"", ctx->address, ctx->port);

  if(ctx->iostate == DSSTATE_0)
  {
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
    rc = sendpack(ctx, pkt, pkt_size);
    if(!rc)
      setstate_connection(ctx, DSSTATE_IN);
    else if(rc < 0)
      setstate_connection(ctx, DSSTATE_ERR);
  }

  if(ctx->iostate == DSSTATE_IN)
  {
    // Read service answer
    dstrace("Expecting answer");
    rc = readpack(ctx);
    if(!rc)
      setstate_connection(ctx, DSSTATE_FIN);
    else if(rc < 0)
      setstate_connection(ctx, DSSTATE_ERR);
  }
}


int poll_connections(PDSTARGET ctx)
{
  int dopoll = 0, doconn = 0;
  PDSTARGET head = ctx;

  while(ctx)
  {
    head->h_pollfds[ctx->ind].fd = ctx->sockfd;
    head->h_pollfds[ctx->ind].events = 0;

    switch(ctx->iostate)
    {
      case DSSTATE_0:
        ctx->ioready = 1;
        doconn = 1;
        break;
      case DSSTATE_CONN:
      case DSSTATE_OUT:
        head->h_pollfds[ctx->ind].events = POLLOUT;
        dopoll = 1;
        break;
      case DSSTATE_IN:
        head->h_pollfds[ctx->ind].events = POLLIN;
        dopoll = 1;
        break;
      case DSSTATE_ERR:
      case DSSTATE_FIN:
        break;
      default:
        dslog("Unexpected state value %d", ctx->iostate);
    }

    ctx = ctx->next;
  }

  if(dopoll)
  {
    // REFACTOR to epoll
    int rc = poll(head->h_pollfds, head->h_conns_num, CONNECTION_TIMEOUT_MS);
    if (rc < 0)
    {
      dslogerr(errno, "Poll call failed");
      return -1;
    }
    if(rc == 0)
    {
      dslogw("Connections timeout");
      return -1;
    }

    // bad bad poll
    ctx = head;
    while(ctx)
    {
      ctx->ioready = 0;

      if(head->h_pollfds[ctx->ind].revents)
      {
        if(ctx->iostate == DSSTATE_CONN)
          setstate_connection(ctx, DSSTATE_OUT);

        ctx->ioready = 1;
      }

      ctx = ctx->next;
    }
  }

  if(!dopoll && !doconn)
  {
    dstrace("Poll finds nothing to do");
    return -1;
  }

  return 0;
}


void dssend(void *dsctx, int pack_signed, int keepalive, const char *msg, char **res, int *res_size)
{
  PDSTARGET ctx;
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

  // send+recv loop
  while(!poll_connections((PDSTARGET)dsctx))
  {
    ctx = (PDSTARGET)dsctx;
    while(ctx)
    {
      if(ctx->ioready)
        process_connection(ctx, pkt, pkt_size);

      ctx = ctx->next;
    }
  }

  // analyse results
  ctx = (PDSTARGET)dsctx;
  while(ctx && !rc)
  {
    if(!ctx->respkt || ctx->respkt_size == 0)
    {
      dslogw("DB returns no result");
      rc = -1;
    }
    else if(ctx->next &&
        (
          ctx->respkt_size != ctx->next->respkt_size || 
          memcmp(ctx->respkt, ctx->next->respkt, ctx->respkt_size)
        )
      )
    {
      dslogw("DBs returns different results");
      rc = -1;
    }

    if(!keepalive && ctx->sockfd > 0)
    {
      dstrace("Close connection %d because no keepalive", ctx->sockfd);
      close(ctx->sockfd);
      ctx->sockfd = -1;
    }

    ctx = ctx->next;
  }

  // Build result
  ctx = (PDSTARGET)dsctx;
  if(!rc && ctx->respkt)
  {
    dsunpack("ds", ctx->respkt, ctx->respkt_size, (const void **)res, res_size, 0);
    /* It will be freed in ctx_release routine after all, so it's safe
    *res = strdup(*res); // pointer inside existing buffer respkt which will be freed
    */
  }

  if(pkt)
    free(pkt);
}


void* dssend_init_ctx(const char *targets)
{
  PDSTARGET head = NULL, curr;
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
        head = (PDSTARGET)malloc(sizeof(DSTARGET));
        if(!head)
        {
          dslogerr(errno, "Cannot allocate first send context");
          rc = -1;
          break;
        }

        head->h_conns_num = 0;
        curr = head;
      }
      else
      {
        curr->next = (PDSTARGET)malloc(sizeof(DSTARGET));
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
      curr->respkt = NULL;
      curr->respkt_size = 0;
      curr->expected_size = -1;
      curr->send_offset = 0;
      curr->read_offset = 0;
      curr->ind = head->h_conns_num;
      curr->ioready = 0;
      curr->next = NULL;
      setstate_connection(curr, DSSTATE_0);
      head->h_conns_num++;
    }

    if(s1)
      pos = s1+1;
    else
      pos = NULL;
  };

  if(_targets)
    free(_targets);

  if(rc)
  {
    while(head)
    {
      curr = head;
      head = head->next;
      free(curr->address);
      free(curr);
    }
  }
  else
  {
    head->h_pollfds = (struct pollfd *)calloc(head->h_conns_num, sizeof(struct pollfd));
  }

  return head;
}


void dssend_release_ctx(void *dsctx)
{
  PDSTARGET ctx, curr;

  dstrace("Release context");

  ctx = (PDSTARGET)dsctx;
  if(ctx)
    free(ctx->h_pollfds);
  while(ctx)
  {
    curr = ctx->next;
    if(ctx->sockfd >= 0)
    {
      dstrace("Close connection %d in dbsync context release", ctx->sockfd);
      close(ctx->sockfd);
    }
    free(ctx->address);
    free(ctx->respkt);
    free(ctx);
    ctx = curr;
  }
}
