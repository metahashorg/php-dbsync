#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/times.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dsredis.h"
#include "dsmisc.h"
#include "dspack.h"
#include "dscrypto.h"



#define INI_PATH "/etc/php-dbsync.ini"

#define CONNECTION_TIMEOUT_MS 3000
#define POLL_TIMEOUT_MS       CONNECTION_TIMEOUT_MS

#define LISTEN_BACKLOG_SIZE   100
#define POLL_QUEUE_SIZE       1024
#define READ_BUFFER_SIZE      1024


clock_t g_clocks_per_second;

int g_pack_options = 0;
char *g_db_addresses = "redis:127.0.0.1:6379";


int process_command(const char *cmd, void **res, int *res_size)
{
  int rc, ret = -1;
  *res = NULL;
  *res_size = 0;

  dstrace("Processing command: %s on \"%s\"", cmd, g_db_addresses);

  // Loop through targets
  char *_targets = strdup(g_db_addresses);
  const char *pos = _targets;
  do
  {
    rc = -1;

    char *s1 = strchr(pos, ',');
    if(s1)
      s1[0] = 0;

    char *s2 = strchr(pos, ':');
    if(!s2 || (s1 && s2 > s1))
    {
      dslog("Bad database string format '%s'", pos);
    }
    else
    {
      const char *db = pos;
      const char *address = s2 + 1;

      char *s3 = strchr(address, ':');
      if(!s3 || (s1 && (s3 > s1)))
      {
        dslog("Bad db string format \"%s\"", pos);
      }
      else
      {
        const char *port = s3 + 1;
        
        s2[0] = 0;
        s3[0] = 0;

        dstrace("Process on db %s:%s:%d", db, address, atoi(port));

        unsigned char *dbres = NULL;
        int dbres_size = 0;
        if(!strcmp(db, "redis"))
          rc = dsredis(address, atoi(port), cmd, &dbres, &dbres_size);
        
        if(!rc)
          ret = 0; // At least one is successful

        void *chunk = NULL;
        int chunk_size = 0;
        if(!rc && dbres_size > 0)
        {
          rc = dspack(db, dbres, dbres_size, &chunk, &chunk_size, 0);
        }
        
        if(!rc && chunk_size > 0)
        {
          dstrace("Add chunk of size %d to %d result", chunk_size, *res_size);
          *res = realloc(*res, *res_size + chunk_size);
          if(*res)
          {
            memcpy(*res + *res_size, chunk, chunk_size);
            *res_size += chunk_size;
          }
          else
            dslogerr(errno, "Result reallocation for new chunk failed");
        }
        if(chunk)
          free(chunk);
        if(dbres)
          free(dbres);
      }
    }
    
    if(s1)
      pos = s1+1;
    else
      pos = NULL;
  } while(pos);
  
  free(_targets);
  
  return ret;
}


int try_command(const unsigned char *buf, int buf_size, void **res, int *res_size)
{
  int rc = dspack_complete("ds", buf, buf_size);
  if(!rc)
  {
    dstrace("Packet ready");

    const void *data = NULL;
    int data_size = 0;
    rc = dsunpack("ds", buf, buf_size, &data, &data_size, g_pack_options);
    
    if(rc)
    {
      dslog("Fail to unpack");
    }
    else
    {
      void *buf = NULL;
      int buf_size;

      dstrace("Pack extracted, data size %d", data_size);

      if(((const char *)data)[data_size - 1] != 0)
        dstrace("Incorrect message trailing symbol detected");
      else
        rc = process_command(data, &buf, &buf_size);
      
      if(!rc)
        /*rc = */dspack("ds", buf, buf_size, res, res_size, 0);
      
      if(buf)
        free(buf);
    }
  } // pack_complete
  else if(rc < 0)
  {
    dstrace("Abnormal packet detected");
  }
  else //if(rc > 0)
  {
    dstrace("Packet is not complete");
  }
  
  return rc;
}



void process_conns(const char *address, int port)
{
  int i, rc;
  char buffer[READ_BUFFER_SIZE];
  int connbuf_insize[POLL_QUEUE_SIZE];
  void *connbuf_in[POLL_QUEUE_SIZE];
  int connbuf_outsize[POLL_QUEUE_SIZE];
  void *connbuf_out[POLL_QUEUE_SIZE];
  void *connbuf_outptr = NULL;
  int conn_timeout_ms[POLL_QUEUE_SIZE];
  
  // init buffers
  for(i = 0; i < POLL_QUEUE_SIZE; i++)
  {
    connbuf_in[i] = malloc(READ_BUFFER_SIZE);
    if(!connbuf_in[i])
      dierr(errno, "Cannot allocate buffer for incoming data");
  }
  bzero(connbuf_insize, sizeof(connbuf_insize));
  bzero(connbuf_out, sizeof(connbuf_out));
  bzero(connbuf_outsize, sizeof(connbuf_outsize));

  // Listen socket init
  int listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); 
  if(listenfd < 0) 
    dierr(errno, "Fail to create listening socket");

  int on = 1;
  rc = setsockopt(listenfd, SOL_SOCKET,  SO_REUSEADDR,
                  (char *)&on, sizeof(on));
  if (rc < 0)
    dierr(errno, "Set socket being reusable failed");

  // bind
  struct sockaddr_in serv_addr;
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);

  if(inet_pton(AF_INET, address, &serv_addr.sin_addr) <= 0)
    die("Bad address %s\n", address);
  
  rc = bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
  if (rc < 0)
    dierr(errno, "Binding to given address failed");
  
  rc = listen(listenfd, LISTEN_BACKLOG_SIZE);
  if (rc < 0)
    dierr(errno, "Failed to mark connection being listen");
  
  // Polling init
  struct pollfd pollfds[POLL_QUEUE_SIZE];
  bzero((char *) &pollfds, sizeof(pollfds));
  
  pollfds[0].fd = listenfd;
  pollfds[0].events = POLLIN;
  
  // Accept&Process loop
  int conns_num = 1;
  while(1)
  {
    clock_t start = times(NULL);
    rc = poll(pollfds, conns_num, POLL_TIMEOUT_MS);
    if (rc < 0)
    {
      dslogerr(errno, "Poll call failed");
      continue;
    }
    
    int gap_ms = (times(NULL) - start) * 1000 / g_clocks_per_second;
    //dstrace("Spent %d ms in poll", gap_ms);

    for(i = 0; i < conns_num; i++)
    {
      // accept new connection
      if(pollfds[i].fd == listenfd)
      {
        if(pollfds[i].revents == 0)
          continue;
      
        if(pollfds[i].revents != POLLIN)
        {
          dslog("Abnormal poll behaviour for accept");
          continue;
        }

        int newfd;
        do {
          newfd = accept(listenfd, NULL, NULL);

          if(newfd < 0)
          {
            if(errno != EWOULDBLOCK)
              dslogerr(errno, "Fail to accept new connection");
          }
          else
          {
            dstrace("Accepted socket %d", newfd);

            // DROP too much connections, get rid of DDOS
            if(conns_num >= POLL_QUEUE_SIZE)
            {
              dslog("Drop connection because queue is full");
              close(newfd);
            }
            else
            {
              rc = fcntl(newfd, F_GETFL, 0);
              if(rc >= 0)
                rc = fcntl(newfd, F_SETFL, rc | O_NONBLOCK);
              if(rc < 0)
              {
                close(newfd);
                dslogerr(errno, "Failed to set new connection to non-blocking mode");
              }
              else
              {
                pollfds[conns_num].fd = newfd;
                pollfds[conns_num].events = POLLIN;
                pollfds[conns_num].revents = 0;
                connbuf_insize[conns_num] = 0;
                conn_timeout_ms[conns_num] = CONNECTION_TIMEOUT_MS + gap_ms; // gap_ms will be decremented 
                conns_num++;
              }
            }
          }
        } while(newfd >= 0);
      }// if listenfd
      else
      {
        int close_conn = 0;
        
        conn_timeout_ms[i] -= gap_ms;
        dstrace("Connection %d have %d ms till timeout", pollfds[i].fd, conn_timeout_ms[i]);

        if(pollfds[i].revents & POLLIN)
        {
          dstrace("Incoming event on %d", pollfds[i].fd);

          do {
            rc = recv(pollfds[i].fd, buffer, sizeof(buffer), 0);
            /* DATA RECEIVED */
            if(rc > 0)
            {
              dstrace("Received %d bytes", rc);

              int size = rc;
              if(size > READ_BUFFER_SIZE - connbuf_insize[i] - 1)
                size = READ_BUFFER_SIZE - connbuf_insize[i] - 1;

              memcpy(connbuf_in[i] + connbuf_insize[i], buffer, size);
              connbuf_insize[i] += size;
              conn_timeout_ms[i] = CONNECTION_TIMEOUT_MS;
            }

            /* CLOSE CONNECTION */
            else if(rc == 0)
            {
              dstrace("Close incoming connection %d detected", pollfds[i].fd);

              // connection is closing
              close_conn = 1;

              dstrace("Test data:");
              dsdump(connbuf_in[i], connbuf_insize[i]);

              try_command(connbuf_in[i], connbuf_insize[i], &connbuf_out[i], &connbuf_outsize[i]);
            }

            /* UPCOMING DATA or DONE */
            else if(rc < 0 && errno == EWOULDBLOCK)
            {
              int rc1 = try_command(connbuf_in[i], connbuf_insize[i], &connbuf_out[i], &connbuf_outsize[i]);
              if(!rc1)
              {
                dstrace("Command is processed, poll to send an answer");
                pollfds[i].events = POLLOUT;
                connbuf_outptr = connbuf_out[i];

                connbuf_insize[i] = 0; // reset for safety
              }
              else if(rc1 > 0)
              {
                // continue to poll the request
                dstrace("Continue to poll %d with incoming data", pollfds[i].fd);
              }
              else // rc1 < 0
              {
                dstrace("Closing connection because of abnormal packet");
                close_conn = 1;
              }
            }

            /* ERROR */
            else //if(rc < 0 && errno != EWOULDBLOCK)
            {
              dstracerr(errno, "Failed to read connection data");
              close_conn = 1;
            }
          } while(rc > 0);
        } // POLLIN

        else if(pollfds[i].revents & POLLOUT)
        {
          dstrace("Outgoing event on %d", pollfds[i].fd);
          do {
            rc = send(pollfds[i].fd, connbuf_outptr, connbuf_outsize[i], 0);
            /* DATA block sent */
            if(rc > 0)
            {
              dstrace("Sent %d bytes of answer", rc);
              
              connbuf_outptr += rc;
              connbuf_outsize[i] -= rc;
              
              if(connbuf_outsize[i] <= 0)
              {
                dstrace("Connection %d sent all data, closing", pollfds[i].fd);
                close_conn = 1;
                rc = -1;
              }

              conn_timeout_ms[i] = CONNECTION_TIMEOUT_MS;
            }
            else if (rc < 0 && errno == EWOULDBLOCK && connbuf_outsize[i] > 0)
            {
              dstrace("Continue to poll %d with outgoing data", pollfds[i].fd);
            }
            else
            {
              if(rc == 0)
                dstrace("Outgoing connection %d closed", pollfds[i].fd);
              else
                dstrace("Outgoing connection %d error %d, closing", pollfds[i].fd, errno);

              close_conn = 1;
            }
          } while(rc > 0);
        } // POLLOUT

        if(conn_timeout_ms[i] <= 0)
        {
          dstrace("Connection %d timeout", pollfds[i].fd);
          close_conn = 1;
        }

        if(close_conn)
        {
          dstrace("Closing connection %d", pollfds[i].fd);
          close(pollfds[i].fd);
          
          if(connbuf_out[i])
            free(connbuf_out[i]);
          connbuf_out[i] = NULL;
          connbuf_outsize[i] = 0;
          connbuf_insize[i] = 0;
          
          // Close connection and remove from polling
          if(i < conns_num-1)
          {
            // switch with latest in pool
            pollfds[i].fd = pollfds[conns_num-1].fd;
            pollfds[i].revents = pollfds[conns_num-1].revents;

            connbuf_insize[i] = connbuf_insize[conns_num-1];
            connbuf_insize[conns_num-1] = 0;

            unsigned char *ptr = connbuf_in[i];
            connbuf_in[i] = connbuf_in[conns_num-1];
            connbuf_in[conns_num-1] = ptr;

            connbuf_outsize[i] = connbuf_outsize[conns_num-1];
            connbuf_outsize[conns_num-1] = 0;

            connbuf_out[i] = connbuf_out[conns_num-1];
            connbuf_out[conns_num-1] = NULL;
          }
          conns_num--;
          i--;
        }
      } // not listenfd
    } // for conns_num
  } // while(1)

  close(listenfd);
  for(i = 0; i < POLL_QUEUE_SIZE; i++)
    free(connbuf_in[i]);
}

// Usage ./dbsyncd [-b 127.0.0.1] [-p 1111] [-s public_key_path] [-d db_addresses]
int main(int argc, char *argv[])
{
  openlog("dbsyncd", LOG_PID, LOG_DAEMON);
  
  char *listen_address = "127.0.0.1";
  char *listen_port = "1111";

  int c;
  while ((c = getopt (argc, argv, "b:p:s:d:")) != -1)
  {
    switch(c)
    {
      case 'b':
        listen_address = optarg;
        break;
      case 'p':
        listen_port = optarg;
        break;
      case 's':
        g_pack_options |= DSPACK_SIGNED;
        dscrypto_init();
        if(!dscrypto_load_public(optarg))
          return -1;
        break;
      case 'd':
        g_db_addresses = strdup(optarg);
        break;
    }
  }
  
  g_clocks_per_second = sysconf(_SC_CLK_TCK);

  process_conns(listen_address, atoi(listen_port));

  free(g_db_addresses);
  free(listen_address);
  free(listen_port);
  
  closelog();
  
  return 0;
}
