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


typedef struct _dstarget {
  char  *address;
  int   port;
  int   sockfd;
  
  struct _dstarget *next;
} DSTARGET, *PDSTARGET;


int wait_connection(int sockfd, int events, int timeout)
{
  struct pollfd pollfds[1];
  bzero((char *) &pollfds, sizeof(pollfds));
  
  pollfds[0].fd = sockfd;
  pollfds[0].events = events;
  
  int rc = poll(pollfds, 1, timeout);
  if (rc < 0)
  {
    dslogerr(errno, "Poll call failed");
    return -1;
  }
  if(rc == 0)
  {
    dslogw("Connection timeout");
    return -1;
  }

  return 0;
}


int create_connection(const char *address, int port, int timeout)
{
  struct sockaddr_in serv_addr;
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);

  if(inet_pton(AF_INET, address, &serv_addr.sin_addr) <= 0)
  {
    dslogerr(errno, "Bad address %s\n", address);
    return -1;
  }

  int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); 
  if(sockfd < 0) 
  {
    dslogerr(errno, "Fail opening socket");
    return -1;
  }

  if(connect(sockfd,(struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
  {
    if(errno != EINPROGRESS)
    {
      dslogwerr(errno, "Cannot connect\n");
      close(sockfd);
      return -1;
    }
  
    if(wait_connection(sockfd, POLLOUT, timeout))
    {
      dstrace("Waiting for connecting failed");
      close(sockfd);
      return -1;
    }
  }
  
  return sockfd;
}


int sendbuf(int sockfd, const char *msg, int size, int timeout)
{
  const char *pos = msg;
 int connect_once_more = 1;

  int rc = -1;
  while(size > 0)
  {
    rc = send(sockfd, pos, size, 0);

    if(rc < 0)
    {
      if(errno != EWOULDBLOCK && errno != EAGAIN)
      {
        dslogwerr(errno, "Data send error");
        break;
      }
      else
      {
        dstracerr(errno, "Waiting for outgoing data");
        if(wait_connection(sockfd, POLLOUT, timeout))
        {
          dstrace("Wait failed");
          break;
        }
      }
    }
    else if(rc == 0)
    {
      dstrace("Connection closed");
      break;
    }
    else
    {
      dstrace("Sent %d", rc);
      dsdump(pos, rc);
      pos += rc;
      size -= rc;
    }
  }
  
  return size;
}


int readbuf(int sockfd, void *buf, int *read_size, int timeout)
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
      else
      {
        if(timeout < 0)
          return 0;

        dstracerr(errno, "Waiting for incoming data");
        if(wait_connection(sockfd, POLLIN, timeout))
        {
          dstrace("Wait failed");
          return -1;
        }
      }
    }
    else // rc = 0
    {
      dstrace("Closed connection while reading the result from service");
      return -1;
    }
  }
  
  return *read_size;
}


int readpack(int sockfd, void **res, int *res_size, int timeout)
{
  char buf[64]; // enough to read the pack header
  int expected_size = -1;

  int read_size;
  int rc1, rc2, offset = 0;
  do {
    read_size = sizeof(buf) - offset;
    rc1 = readbuf(sockfd, buf+offset, &read_size, -1);
    offset += read_size;

    if(read_size > 0)
      rc2 = dspack_bufsize("ds", buf, read_size, &expected_size);
    else
      rc2 = 1; // need more

    if(rc2 > 0 && !rc1)
    {
      dstracerr(errno, "Waiting for incoming packet size data");
      if(wait_connection(sockfd, POLLIN, timeout))
      {
        dstrace("Wait failed");
        return -1;
      }
    }
  } while(rc1 >= 0 && rc2 > 0); // while not error and while need to read more
  read_size = offset;

  if(expected_size <= 0)
    return expected_size;

  dstrace("Expected data size %d", expected_size);

  char *data = (char *)malloc(expected_size);
  if(!data)
  {
    dslogerr(errno, "Cannot allocate result buffer");
    return -1;
  }

  memcpy(data, buf, read_size);

  int left_size = expected_size - read_size;
  readbuf(sockfd, data + read_size, &left_size, timeout);

  if(left_size == expected_size - read_size)
  {
    dstrace("data read OK");
    *res = data;
    *res_size = expected_size;
  }
  else
  {
    dslogw("data read failed, read size %d", read_size);
    free(data);
  }
  
  return expected_size;
}


void send_message(PDSTARGET ctx, const char *pkt, int pkt_size, void **res, int *res_size)
{
  int rc;

  if(!pkt_size)
  {
    dslog("Sending nothing prohibitted");
    return;
  }
  
  dstrace("Sending to \"%s:%d\"", ctx->address, ctx->port);
  //dsdump(pkt, pkt_size);

  if(ctx->sockfd <= 0)
  {
    // Send once on new connection
    ctx->sockfd = create_connection(ctx->address, ctx->port, CONNECTION_TIMEOUT_MS);
    if(ctx->sockfd <= 0)
      return;
    rc = sendbuf(ctx->sockfd, pkt, pkt_size, CONNECTION_TIMEOUT_MS);
  }
  else
  {
    // Send retry on existing connection
    rc = sendbuf(ctx->sockfd, pkt, pkt_size, CONNECTION_TIMEOUT_MS);
    if(rc)
    {
      close(ctx->sockfd);
      ctx->sockfd = create_connection(ctx->address, ctx->port, CONNECTION_TIMEOUT_MS);
      if(ctx->sockfd <= 0)
        return;
      rc = sendbuf(ctx->sockfd, pkt, pkt_size, CONNECTION_TIMEOUT_MS);
    }
  }

  if(rc)
  {
    if(ctx->sockfd <= 0)
    {
      dstrace("Close connection %d because of error", ctx->sockfd);
      close(ctx->sockfd);
    }
    ctx->sockfd = -1;
  }
  // Read service answer
  else if(res && res_size)
  {
    dstrace("Expecting answer");
    /* rc =*/ readpack(ctx->sockfd, res, res_size, CONNECTION_TIMEOUT_MS);
  }
}


void dssend(void *dsctx, int pack_signed, int keepalive, const char *msg, char **res, int *res_size)
{
  int rc, pack_options = 0;
  PDSTARGET ctx = (PDSTARGET)dsctx;

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

  void *respkt = NULL;
  int respkt_size = 0;
  void *prevpkt = NULL;
  int prevpkt_size = 0;
  void *pkt = NULL;
  int pkt_size = 0;
  int len = strlen(msg);
  rc = dspack("ds", msg, len + 1, &pkt, &pkt_size, pack_options); // add trailing 0 symbol
  if(rc)
  {
    dslog("Error: Packing failed");
    return;
  }

  // Loop through targets
  *res = NULL;
  *res_size = 0;
  while(ctx && !rc)
  {
    respkt = NULL;
    respkt_size = 0;
    send_message(ctx, pkt, pkt_size, &respkt, &respkt_size);
    if(!respkt || respkt_size == 0)
    {
      dslogw("DB returns no result");
      rc = -1;
    }
    else if(!prevpkt)
    {
      prevpkt = respkt;
      prevpkt_size = respkt_size;
    }
    else
    {
      if(!respkt || respkt_size != prevpkt_size || memcmp(respkt, prevpkt, respkt_size))
      {
        dslogw("DB returns different result");
        rc = -1;
      }
      
      free(prevpkt);
      prevpkt = respkt;
      prevpkt_size = respkt_size;
    }

    if(!keepalive && ctx->sockfd > 0)
    {
      dstrace("Close connection %d because no keepalive", ctx->sockfd);
      close(ctx->sockfd);
      ctx->sockfd = -1;
    }
    ctx = ctx->next;
  }
  
  if(!rc && respkt)
  {
    dsunpack("ds", respkt, respkt_size, (const void **)res, res_size, 0);
    *res = strdup(*res); // pointer inside existing buffer respkt which will be freed
  }

  if(respkt)
    free(respkt);
  if(pkt)
    free(pkt);
}


void* dssend_init_ctx(const char *targets)
{
  PDSTARGET ctx = NULL, curr;
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
      
      if(!ctx)
      {
        ctx = (PDSTARGET)malloc(sizeof(DSTARGET));
        if(!ctx)
        {
          dslogerr(errno, "Cannot allocate first send context");
          rc = -1;
          break;
        }
        curr = ctx;
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
      
      curr->address = strdup(address);
      curr->port = atoi(port);
      curr->sockfd = -1;
      curr->next = NULL;
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
    while(ctx)
    {
      curr = ctx->next;
      free(ctx->address);
      free(ctx);
      ctx = curr;
    }
  }

  return ctx;
}


void dssend_release_ctx(void *dsctx)
{
  PDSTARGET ctx, curr;

  dstrace("Release context");

  ctx = (PDSTARGET)dsctx;
  while(ctx)
  {
    curr = ctx->next;
    if(ctx->sockfd >= 0)
    {
      dstrace("Close connection %d in dbsync context release", ctx->sockfd);
      close(ctx->sockfd);
    }
    free(ctx->address);
    free(ctx);
    ctx = curr;
  }
}
