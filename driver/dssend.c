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
    dstrace("Connection timeout");
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
      dstracerr(errno, "Cannot connect\n");
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

  int rc = -1;
  while(size > 0)
  {
    rc = send(sockfd, pos, size, 0);

    if(rc < 0)
    {
      if(errno != EWOULDBLOCK && errno != EAGAIN)
      {
        dstracerr(errno, "Data send error");
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
      dstracerr(errno, "Connection closed");
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

  do
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
      return 0;
    }
  } while(*read_size < buf_size);
  
  return *read_size;
}


int readpack(int sockfd, void **res, int *res_size, int timeout)
{
  int rc;
  char buf[64]; // enough to read the pack header
  int expected_size = -1;

  int read_size = sizeof(buf);
  rc = readbuf(sockfd, buf, &read_size, timeout);
  
  if(rc >= 0)
    /*rc = */dspack_bufsize("ds", buf, read_size, &expected_size);

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
  rc = readbuf(sockfd, data + read_size, &left_size, timeout);

  if(left_size == expected_size - read_size)
  {
    dstrace("data read OK");
    *res = data;
    *res_size = expected_size;
  }
  else
  {
    dstrace("data read failed, read size %d", read_size);
    free(data);
  }
  
  return expected_size;
}


void send_message(const char *address, const char *port, const char *pkt, int pkt_size, void **res, int *res_size)
{
  if(!pkt_size)
  {
    dstrace("Sending nothing prohibitted");
    return;
  }
  
  dstrace("Sending to \"%s:%s\"", address, port);
  //dsdump(pkt, pkt_size);

  int sockfd = create_connection(address, atoi(port), CONNECTION_TIMEOUT_MS);
  if(sockfd <= 0)
    return;
  
  int rc = sendbuf(sockfd, pkt, pkt_size, CONNECTION_TIMEOUT_MS);

  // Read service answer
  if(!rc && res && res_size)
  {
    dstrace("Expecting answer");
    /* rc =*/ readpack(sockfd, res, res_size, CONNECTION_TIMEOUT_MS);
  }

  close(sockfd);
}


void dssend(const char *targets, int pack_signed, const char *msg, char **res, int *res_size)
{
  int rc, pack_options = 0;
  
  dstrace("PHP send \"%s\" to \"%s\"", targets);

  if(pack_signed)
    pack_options |= DSPACK_SIGNED;

  void *respkt = NULL;
  int respkt_size = 0;
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
  char *_targets = strdup(targets);
  const char *pos = _targets;
  *res = NULL;
  *res_size = 0;
  do
  {
    char *s1 = strchr(pos, ',');
    char *s2 = strchr(pos, ':');
    if(!s2 || (s1 && (s2 > s1)))
    {
      dslog("Bad target format %s", pos);
    }
    else
    {
      s2[0] = 0;
      const char *address = pos;
      if(s1)
        s1[0] = 0;
      const char *port = s2+1;

      // read only first message
      if(!*res)
        send_message(address, port, pkt, pkt_size, &respkt, &respkt_size);
      else
        send_message(address, port, pkt, pkt_size, NULL, NULL);
    }
    
    if(s1)
      pos = s1+1;
    else
      pos = NULL;
  } while(pos);
  
  if(respkt)
  {
    dsunpack("ds", respkt, respkt_size, (const void **)res, res_size, 0);
    *res = strdup(*res); // pointer inside existing buffer
    free(respkt);
  }

  free(_targets);
  free(pkt);
}
