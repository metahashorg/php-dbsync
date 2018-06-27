#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <hiredis/hiredis.h>

#include "dsmisc.h"



int dsredis(const char *hostname, int port, const char *cmd, unsigned char **res, int *res_size)
{
  redisContext *c;
  redisReply *reply;

  struct timeval timeout = { 1, 500000 }; // 1.5 seconds
  c = redisConnectWithTimeout(hostname, port, timeout);
  if (c == NULL || c->err)
  {
    if (c)
    {
      dslog("REDIS connection error: %s", c->errstr);
      redisFree(c);
    }
    else 
    {
      dslog("REDIS connection error: can't allocate redis context");
    }

    return -1;
  }

  dstrace("Run redis command: \"%s\"", cmd);

  reply = redisCommand(c, cmd);

  int i, rc = 0;
  if(!reply)
  {
    dstrace("Redis NO result");
    rc = -1;
  }
  else if(reply->type == REDIS_REPLY_ERROR)
  {
    dstrace("Redis ERROR result: \"%s\"", reply->str);
    rc = -1;
  }
  else if(reply->type == REDIS_REPLY_ARRAY)
  {
    *res_size = 0;
    for(i = 0; i < reply->elements; i++)
    {
      dstrace("Redis ARRAY result #%d: \"%s\"", i, reply->element[i]->str);
      *res_size += strlen(reply->element[i]->str) + 1;
    }
    *res = (unsigned char *)malloc(*res_size);
    *res[0] = 0;
    for(i = 0; i < reply->elements; i++)
    {
      strcat((char *)*res, reply->element[i]->str);
      if(i < reply->elements - 1)
        strcat((char *)*res, "\n");
    }
  }
  else if(reply->type == REDIS_REPLY_STRING)
  {
    dstrace("Redis STRING result: \"%s\"", reply->str);
    
    *res = (unsigned char *)strdup(reply->str);
    *res_size = strlen(reply->str) + 1;
  }
  else if(reply->type == REDIS_REPLY_STATUS)
  {
    dstrace("Redis STATUS result: \"%s\"", reply->str);
    
    *res = (unsigned char *)strdup(reply->str);
    *res_size = strlen(reply->str) + 1;
  }
  else if(reply->type == REDIS_REPLY_INTEGER)
  {
    dstrace("Redis INTEGER result: \"%d\"", reply->integer);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", reply->integer);
    *res = (unsigned char *)strdup(buf);
    *res_size = strlen(buf) + 1;
  }
  else if(reply->type == REDIS_REPLY_NIL)
  {
    dstrace("Redis NIL result");
    *res_size = 0;
  }

  if(reply)
    freeReplyObject(reply);

  redisFree(c);

  return rc;
}
