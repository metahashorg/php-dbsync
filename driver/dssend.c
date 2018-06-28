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



int wait_connection(int sockfd, int events)
{
  struct pollfd pollfds[1];
  bzero((char *) &pollfds, sizeof(pollfds));
  
  pollfds[0].fd = sockfd;
  pollfds[0].events = events;
  
  int rc = poll(pollfds, 1, CONNECTION_TIMEOUT_MS);
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


void send_message(const char *address, const char *port, const char *msg, int size, void **res, int *res_size)
{
  int rc;
  
  if(!size)
  {
    dstrace("Sending nothing prohibitted");
    return;
  }
  
  dstrace("Sending to \"%s:%s\"", address, port);
  dsdump(msg, size);

  struct sockaddr_in serv_addr;
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(atoi(port));

  if(inet_pton(AF_INET, address, &serv_addr.sin_addr) <= 0)
  {
    dslogerr(errno, "Bad address %s\n", address);
    return;
  }

  int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); 
  if(sockfd < 0) 
  {
    dslogerr(errno, "Fail opening socket");
    return;
  }

  if(connect(sockfd,(struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
  {
    if(errno != EINPROGRESS)
    {
      dstracerr(errno, "Cannot connect\n");
      close(sockfd);
      return;
    }
  
    if(wait_connection(sockfd, POLLOUT))
    {
      dstrace("Waiting for connecting failed");
      close(sockfd);
      return;
    }
  }

  const char *pos = msg;

  rc = -1;
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
        if(wait_connection(sockfd, POLLOUT))
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

  // Read service answer
  if(!size && res && res_size)
  {
    char msgbuf[64];
    int msg_size = 0;
    int expected_size = -1;

    dstrace("Expecting answer");

    do
    {
      rc = recv(sockfd, &msgbuf[msg_size], sizeof(msgbuf) - msg_size, 0);
      if(rc > 0)
      {
        dstrace("Received initial answer chunk %d size", rc);

        msg_size += rc;
        int rc1 = dspack_bufsize("ds", msgbuf, msg_size, &expected_size);
        if(rc1 <= 0)
          break;
      }
      else if(rc < 0)
      {
        if(errno != EWOULDBLOCK && errno != EAGAIN)
        {
          dstracerr(errno, "Error to read the result from service");
          break;
        }
        else
        {
          dstracerr(errno, "Waiting for incoming data");
          if(wait_connection(sockfd, POLLIN))
          {
            dstrace("Wait failed");
            break;
          }

          rc = 1;
        }
      }
      else // rc = 0
      {
        dstrace("Closed connection while reading the result from service");
      }
    } while(rc > 0);

    if(expected_size > 0)
    {
      dstrace("Expected answer size %d", expected_size);

      char *answer = (char *)malloc(expected_size);
      int answer_size = 0;

      if(!answer)
      {
        dslogerr(errno, "Cannot allocate result buffer");
      }
      else
      {
        memcpy(answer, msgbuf, msg_size);
        answer_size = msg_size;

        do
        {
          rc = recv(sockfd, answer + answer_size, expected_size - answer_size, 0);
          if(rc > 0)
          {
            dstrace("Received answer chunk %d size", rc);
            answer_size += rc;
          }
          else if(rc < 0)
          {
            if(errno != EWOULDBLOCK && errno != EAGAIN)
            {
              dstracerr(errno, "Error to read the result from service");
              break;
            }
            else
            {
              dstracerr(errno, "Waiting for incoming data");
              if(wait_connection(sockfd, POLLIN))
              {
                dstrace("Wait failed");
                break;
              }

              rc = 1;
            }
          }
          else // rc = 0
          {
            dstrace("Closed connection while reading the result from service");
          }
        } while(rc > 0 && answer_size < expected_size);

        if(answer_size == expected_size)
        {
          dstrace("Answer read OK");
          *res = answer;
          *res_size = answer_size;
        }
        else
        {
          dstrace("Answer read failed, read size %d", answer_size);
          free(answer);
        }
      } // malloc
    } // expected_size
  } // !rc && res && res_size

  close(sockfd);

  dstrace("Send data done");
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
