#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>

#include "dsmisc.h"

#ifdef DSDEBUG
void dstrace(const char *msg, ...)
{
  va_list aptr;
  
  va_start(aptr, msg);
  vfprintf(stderr, msg, aptr);
  va_end(aptr);

  fprintf(stderr, "\n");
}

void dstracerr(int err, const char *msg, ...)
{
  char buf[256];
  if(!strerror_r(err, buf, sizeof(buf)))
    fprintf(stderr, "(%s) ", buf);

  va_list aptr;
  
  va_start(aptr, msg);
  vfprintf(stderr, msg, aptr);
  va_end(aptr);

  fprintf(stderr, "\n");
}


void dsdump(const unsigned char *buf, int buf_size)
{
  int i;

  for(i = 0; i < buf_size; i++)
    fprintf(stderr, "%X%X", buf[i] >> 4, buf[i] & 0x0F);
  fprintf(stderr, "\n");
}
#endif


void dslog(const char *msg, ...)
{
  char str[1024];
  va_list aptr;
  
  va_start(aptr, msg);
  vsnprintf(str, sizeof(str), msg, aptr);
  va_end(aptr);

  syslog(LOG_ERR, "%s", str);
  dstrace(str);
}


void dslogerr(int err, const char *msg, ...)
{
  char str[1024];
  va_list aptr;
  
  va_start(aptr, msg);
  vsnprintf(str, sizeof(str), msg, aptr);
  va_end(aptr);

  char buf[128];
  if(!strerror_r(err, buf, sizeof(buf)))
  {
    syslog(LOG_ERR, "%s (%s)", str, buf);
    dstrace("%s (%s)", str, buf);
  }
  else
  {
    syslog(LOG_ERR, "%s", str);
    dstrace(str);
  }
}


void die(const char *msg, ...)
{
  va_list aptr;
  
  fprintf(stderr, "ERROR: ");
  
  va_start(aptr, msg);
  vfprintf(stderr, msg, aptr);
  dslog(msg, aptr);
  va_end(aptr);

  fprintf(stderr, "\n");

  exit(1);
}

void dierr(int err, const char *msg, ...)
{
  char buf[256];

  if(!strerror_r(err, buf, sizeof(buf)))
    fprintf(stderr, "ERROR: (%s) ", buf);
  else
    fprintf(stderr, "ERROR: ");

  va_list aptr;
  
  va_start(aptr, msg);
  vfprintf(stderr, msg, aptr);
  dslogerr(err, msg, aptr);
  va_end(aptr);

  fprintf(stderr, "\n");

  exit(1);
}
