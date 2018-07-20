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
#define READ_BUFFER_SIZE      10240


typedef struct _db_address {
  char *db;
  char *address;
  int port;
  struct _db_address *next;

} DB_ADDRESS, *PDB_ADDRESS;

typedef struct _drv_connection {
  int sockfd;
  int connbuf_insize;
  unsigned char connbuf_in[READ_BUFFER_SIZE];
  int connbuf_outsize;
  unsigned char *connbuf_out;
  unsigned char *connbuf_outptr;
  int conn_timeout_ms;
  
  int trusted;

} DRV_CONNECTION, *PDRV_CONNECTION;


static clock_t      g_clocks_per_second;
static int          g_service_working = 0;
static int          g_pack_options = 0;
static int          g_keepalive = 1;
static PDB_ADDRESS  g_db_addresses = NULL;


#ifdef DSDEBUG
void service_command(const char *cmd)
{
  if(!strcmp(cmd, "QUIT"))
    g_service_working = 0;
}
#endif

void process_command(const char *cmd, void **res, int *res_size)
{
#ifdef DSDEBUG
  char *cmdpos = strchr(cmd, ':');
  if(!strncmp(cmd, "dbsyncd", cmdpos - cmd))
  {
    dstrace("Processing service command: %s", cmdpos + 1);

    service_command(cmdpos + 1);
    return;
  }
#endif

  int rc;
  *res = NULL;
  *res_size = 0;

  dstrace("Processing command: %s", cmd);

  // Loop through targets
  PDB_ADDRESS db_address = g_db_addresses;
  rc = 0; // all databases should have successful result
  while(!rc && db_address)
  {
    dstrace("Process on db %s:%s:%d", db_address->db, db_address->address, db_address->port);

    unsigned char *dbres = NULL;
    int dbres_size = 0;

    if(!strcmp(db_address->db, "redis"))
      rc = dsredis(db_address->address, db_address->port, cmd, &dbres, &dbres_size);

    void *chunk = NULL;
    int chunk_size = 0;
    if(!rc && dbres_size > 0)
      rc = dspack(db_address->db, dbres, dbres_size, &chunk, &chunk_size, 0);

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

    db_address = db_address->next;
  }

  if(rc && *res)
  {
    free(*res);
    *res = NULL;
    *res_size = 0;
  }
}


void free_db_addresses(void)
{
  PDB_ADDRESS prev;

  while(g_db_addresses)
  {
    free(g_db_addresses->address);
    free(g_db_addresses->db);

    prev = g_db_addresses;
    g_db_addresses = g_db_addresses->next;
    free(prev);
  }
}


void parse_db_addresses(const char *saddresses)
{
  PDB_ADDRESS pcurr = g_db_addresses;
  
  char *_targets = strdup(saddresses);
  const char *pos = _targets;
  do
  {
    char *s1 = strchr(pos, ',');
    if(s1)
      s1[0] = 0;

    char *s2 = strchr(pos, ':');
    if(!s2 || (s1 && s2 > s1))
    {
      dsdie("Bad database string format '%s'", pos);
    }
    else
    {
      const char *db = pos;
      const char *address = s2 + 1;

      char *s3 = strchr(address, ':');
      if(!s3 || (s1 && (s3 > s1)))
      {
        dsdie("Bad db string format \"%s\"", pos);
      }
      else
      {
        const char *port = s3 + 1;
        
        s2[0] = 0;
        s3[0] = 0;

        dstrace("Process on db %s:%s:%d", db, address, atoi(port));
        if(!pcurr)
        {
          pcurr = (PDB_ADDRESS)malloc(sizeof(DB_ADDRESS));
          g_db_addresses = pcurr;
        }
        else
        {
          pcurr->next = (PDB_ADDRESS)malloc(sizeof(DB_ADDRESS));
          pcurr = pcurr->next;
        }

        pcurr->db = strdup(db);
        pcurr->address = strdup(address);
        pcurr->port = atoi(port);
        pcurr->next = NULL;
      }
    }
    if(s1)
      pos = s1+1;
    else
      pos = NULL;
  } while(pos);

  free(_targets);
}


// return true for correct packet, to mark trustworthy connection
int try_command(const unsigned char *cmdbuf, int cmdbuf_size, unsigned char **res, int *res_size)
{
  int rc = dspack_complete("ds", cmdbuf, cmdbuf_size);
  if(!rc)
  {
    dstrace("Packet ready");

    const void *data = NULL;
    int data_size = 0;
    rc = dsunpack("ds", cmdbuf, cmdbuf_size, &data, &data_size, g_pack_options);
    
    if(rc)
    {
      dslog("Fail to unpack");
    }
    else
    {
      void *buf = NULL;
      int buf_size = 0;

      dstrace("Pack extracted, data size %d", data_size);

      if(((const char *)data)[data_size - 1] != 0)
        dstrace("Incorrect message trailing symbol detected");
      else
        process_command(data, &buf, &buf_size);
      
      if(res && res_size && buf && buf_size)
        /*rc = */dspack("ds", buf, buf_size, (void **)res, res_size, 0);
      
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

  PDRV_CONNECTION conns[POLL_QUEUE_SIZE];

  for(i = 0; i < POLL_QUEUE_SIZE; i++)
  {
    conns[i] = (PDRV_CONNECTION)malloc(sizeof(DRV_CONNECTION));
    if(!conns[i])
      dsdierr(errno, "Cannot allocate context for incoming data");
    conns[i]->sockfd = -1;
    conns[i]->trusted = 0;
    conns[i]->connbuf_insize = 0;
    conns[i]->connbuf_outsize = 0;
    conns[i]->connbuf_out = NULL;
    conns[i]->connbuf_outptr = NULL;
  }

  // Listen socket init
  int listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); 
  if(listenfd < 0) 
    dsdierr(errno, "Fail to create listening socket");

  int on = 1;
  rc = setsockopt(listenfd, SOL_SOCKET,  SO_REUSEADDR,
                  (char *)&on, sizeof(on));
  if (rc < 0)
    dsdierr(errno, "Set socket being reusable failed");

  // bind
  struct sockaddr_in serv_addr;
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);

  if(inet_pton(AF_INET, address, &serv_addr.sin_addr) <= 0)
    dsdie("Bad address %s\n", address);
  
  rc = bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
  if (rc < 0)
    dsdierr(errno, "Binding to given address failed");
  
  rc = listen(listenfd, LISTEN_BACKLOG_SIZE);
  if (rc < 0)
    dsdierr(errno, "Failed to mark connection being listen");
  
  // Polling init
  struct pollfd pollfds[POLL_QUEUE_SIZE];
  bzero((char *) &pollfds, sizeof(pollfds));
  
  pollfds[0].fd = listenfd;
  pollfds[0].events = POLLIN;
  
  // Accept&Process loop
  int conns_num = 1;
  g_service_working = 1;
  while(g_service_working)
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
                conns[conns_num]->sockfd = newfd;
                conns[conns_num]->connbuf_insize = 0;
                conns[conns_num]->conn_timeout_ms = CONNECTION_TIMEOUT_MS + gap_ms; // gap_ms will be decremented 
                conns_num++;
              }
            }
          }
        } while(newfd >= 0);
      }// if listenfd
      else
      {
        int close_conn = 0;
        int close_force = 0;
        
        conns[i]->conn_timeout_ms -= gap_ms;
        dstrace("Connection %d have %d ms till timeout", pollfds[i].fd, conns[i]->conn_timeout_ms);

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
              if(size > READ_BUFFER_SIZE - conns[i]->connbuf_insize - 1)
                size = READ_BUFFER_SIZE - conns[i]->connbuf_insize - 1;

              memcpy(conns[i]->connbuf_in + conns[i]->connbuf_insize, buffer, size);
              conns[i]->connbuf_insize += size;
              conns[i]->conn_timeout_ms = CONNECTION_TIMEOUT_MS;
            }

            /* CLOSE CONNECTION */
            else if(rc == 0)
            {
              dstrace("Close incoming connection %d detected", pollfds[i].fd);

              // connection is closing
              close_conn = 1;
              close_force = 1;

              // We are not interested in command without being able to answer
              //dstrace("Test data:");
              //dsdump(conns[i]->connbuf_in, conns[i]->connbuf_insize);

              //try_command(conns[i]->connbuf_in, conns[i]->connbuf_insize, NULL, NULL);
            }

            /* UPCOMING DATA or DONE */
            else if(rc < 0 && errno == EWOULDBLOCK)
            {
              int rc1 = try_command(conns[i]->connbuf_in, conns[i]->connbuf_insize, &conns[i]->connbuf_out, &conns[i]->connbuf_outsize);
              if(!rc1)
              {
                conns[i]->trusted = 1;
                if(!conns[i]->connbuf_out)
                {
                  dstrace("Command is processed, nothing to send, closing connection");
                  close_conn = 1;
                }
                else
                {
                  dstrace("Command is processed, poll to send an answer");
                  pollfds[i].events = POLLOUT;
                  conns[i]->connbuf_outptr = conns[i]->connbuf_out;
                  conns[i]->connbuf_insize = 0; // reset for safety
                }
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
                close_force = 1;
              }
            }

            /* ERROR */
            else //if(rc < 0 && errno != EWOULDBLOCK)
            {
              dstracerr(errno, "Failed to read connection data");
              close_conn = 1;
              close_force = 1;
            }
          } while(rc > 0);
        } // POLLIN

        else if(pollfds[i].revents & POLLOUT)
        {
          dstrace("Outgoing event on %d", pollfds[i].fd);
          do {
            rc = send(pollfds[i].fd, conns[i]->connbuf_outptr, conns[i]->connbuf_outsize, 0);
            /* DATA block sent */
            if(rc > 0)
            {
              dstrace("Sent %d bytes of answer", rc);
              
              conns[i]->connbuf_outptr += rc;
              conns[i]->connbuf_outsize -= rc;
              
              if(conns[i]->connbuf_outsize <= 0)
              {
                dstrace("Connection %d sent all data, closing", pollfds[i].fd);
                close_conn = 1;
                rc = -1;
              }

              conns[i]->conn_timeout_ms = CONNECTION_TIMEOUT_MS;
            }
            else if (rc < 0 && errno == EWOULDBLOCK && conns[i]->connbuf_outsize > 0)
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
              close_force = 1;
            }
          } while(rc > 0);
        } // POLLOUT

        if(conns[i]->conn_timeout_ms <= 0)
        {
          dslogw("Connection %d timeout", pollfds[i].fd);
          close_conn = 1;
          close_force = 1;
        }

        if(close_conn)
        {
          dstrace("Cleanup connection context %d", pollfds[i].fd);

          if(conns[i]->connbuf_out)
            free(conns[i]->connbuf_out);

          conns[i]->connbuf_insize = 0;
          conns[i]->connbuf_outsize = 0;
          conns[i]->connbuf_out = NULL;
          conns[i]->connbuf_outptr = NULL;

          // Close connection and remove from polling
          if(!conns[i]->trusted || !g_keepalive || close_force)
          {
            dstrace("Close connection %d", pollfds[i].fd);

            close(pollfds[i].fd);
            conns[i]->sockfd = -1;

            if(i < conns_num-1)
            {
              // switch with latest in pool
              pollfds[i].fd = pollfds[conns_num-1].fd;
              pollfds[i].revents = pollfds[conns_num-1].revents;

              PDRV_CONNECTION ptr = conns[i];
              conns[i] = conns[conns_num-1];
              conns[conns_num-1] = ptr;
            }
            conns_num--;
            i--;
          }
          else
          {
            // switch keepalived connection to wait for incoming data from driver
            pollfds[i].events = POLLIN;
          }
        }
      } // not listenfd
    } // for conns_num
  } // while(1)

  close(listenfd);
  for(i = 0; i < POLL_QUEUE_SIZE; i++)
    free(conns[i]);
}

// Usage ./dbsyncd [-b 127.0.0.1] [-p 1111] [-s public_key_path] [-d db_addresses] [-c]
int main(int argc, char *argv[])
{
  openlog("dbsyncd", LOG_PID, LOG_DAEMON);
  
  dslog("dbsyncd %s started", DSVERSION);
  
  char *listen_address = "127.0.0.1";
  char *listen_port = "1111";

  int c;
  while ((c = getopt (argc, argv, "b:p:s:d:c:")) != -1)
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
        parse_db_addresses(optarg);
        break;
      case 'c':
        g_keepalive = 0;
        break;
    }
  }
  
  if(!g_db_addresses)
    parse_db_addresses("redis:127.0.0.1:6379");
  
  g_clocks_per_second = sysconf(_SC_CLK_TCK);

  process_conns(listen_address, atoi(listen_port));

  free_db_addresses();
  dscrypto_keyfree(NULL);
  dscrypto_cleanup();
  closelog();
  
  return 0;
}
