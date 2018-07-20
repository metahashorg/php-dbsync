#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "dspack.h"
#include "dscrypto.h"
#include "dsmisc.h"


static int _pack(const char *tag, const void *data, int data_size, void **res, int *res_size)
{
  char lenbuf[16];
  snprintf(lenbuf, sizeof(lenbuf), "%d", data_size);
  
  *res_size = data_size + strlen(tag) + strlen(lenbuf) + 2;

  char *buf = (char *)malloc(*res_size);
  if(!buf)
  {
    dslogerr(errno, "Cannot allocate pack buffer");
    return -1;
  }

  *res = buf;

  strcpy(buf, tag);
  strcat(buf, ":");
  strcat(buf, lenbuf);
  strcat(buf, ":");
  
  memcpy(buf + strlen(buf), data, data_size);

  return 0;
}


static int _unpack(const char *tag, const void *data, int data_size, const void **res, int *res_size)
{
  const char *pstr = data;
  
  if(data_size < strlen(tag) + 3)
  {
    dstrace("Unpacking error: Expected %d packet size is abnormal", data_size);
    return -1;
  }
    
  if(strncmp(pstr, tag, strlen(tag)))
  {
    dstrace("Unpacking error: wrong packet tag");
    return -1;
  }

  pstr = strchr(pstr, ':');
  if(!pstr)
  {
    dstrace("Unpacking error: wrong packet format");
    return -1;
  }
  pstr += 1;
  
  const char *pend = strchr(pstr, ':');
  if(!pstr)
  {
    dstrace("Unpacking error: wrong packet format");
    return -1;
  }
  
  if(pend - pstr > 10)
  {
    dstrace("Unpacking error: wrong packet format");
    return -1;
  }
    
  char lenbuf[16];
  memcpy(lenbuf, pstr, pend - pstr);
  lenbuf[pend - pstr] = 0;
  
  sscanf(lenbuf, "%d", res_size);

  if(*res_size > data_size - strlen(tag) - strlen(lenbuf) - 2)
  {
    dstrace("Unpack error: Expected %d message size but received %d", *res_size, data_size - strlen(tag) - strlen(lenbuf) - 2);
    return -1;
  }
  
  *res = data + strlen(tag) + strlen(lenbuf) + 2;

  return *res_size + strlen(tag) + strlen(lenbuf) + 2;
}


int dspack_bufsize(const char *tag, const void *data, int data_size, int *size)
{
  const char *pstr = data;

  if(data_size < strlen(tag) + 3)
    return 1; // need more

  if(strncmp(pstr, tag, strlen(tag)))
  {
    dslogw("Pack size: wrong packet tag");
    return -1; // wrong data
  }
  
  pstr = strchr(pstr, ':');
  if(!pstr)
  {
    dslogw("Unpacking error: wrong packet format (1)");
    return -1; // need more
  }
  pstr += 1;
  
  const char *pend = strchr(pstr, ':');
  if(!pend)
  {
    if(data_size - (pstr - (const char *)data) > 10)
    {
      dslogw("Unpacking error: wrong packet format (2)");
      return -1;
    }

    dslogw("Buf size not received yet");
    return 1; // need more
  }

  if(pend - pstr > 10)
  {
    dslogw("Unpacking error: wrong packet format (3)");
    return -1;
  }
    
  char lenbuf[16];
  memcpy(lenbuf, pstr, pend - pstr);
  lenbuf[pend - pstr] = 0;
  
  int len = 0;
  sscanf(lenbuf, "%d", &len);

  *size = len + strlen(tag)+ strlen(lenbuf) + 2;

  dstrace("Found buf size %d", *size);
  
  return 0;
}


int dspack_complete(const char *tag, const void *data, int data_size)
{
  int size = 0;
  int rc = dspack_bufsize(tag, data, data_size, &size);
  
  if(!rc)
  {
    if(data_size < size)
      rc = 1;
    else if(data_size > size)
      rc = -1;
    else
      rc = 0;
  }

  return rc;
}


int dspack(const char *tag, const void *data, int data_size, void **res, int *res_size, int options)
{
  int rc;

  void *pkt = NULL;
  int pkt_size = 0;
  rc = _pack(tag, data, data_size, &pkt, &pkt_size);
  if(rc)
  {
    dstrace("Error: Packing failed");
    return rc;
  }

  *res = pkt;
  *res_size = pkt_size;

  if(options & DSPACK_SIGNED)
  {
    dstrace("Signing '%s' packet: %s", tag, *res);
    
    void *signature = NULL;
    int signature_size = 0;
    rc = dscrypto_signature(NULL, data, data_size, &signature, &signature_size);
    if(rc)
    {
      dstrace("ERROR: cannot build signature");
    }
    else
    {
      int pkt_signed_size = pkt_size + signature_size;
      void *pkt_signed = (char *)malloc(pkt_signed_size);
      if(!pkt_signed)
      {
        dslogerr(errno, "Cannot allocate buffer for signed packet");
        rc = -1;
      }
      else
      {
        memcpy(pkt_signed, pkt, pkt_size);
        memcpy(pkt_signed + pkt_size, signature, signature_size);

        rc = _pack(tag, pkt_signed, pkt_signed_size, res, res_size);

        free(pkt_signed);
      }
      free(signature);
    }

    free(pkt);
  }

  if(rc)
    dslog("Error: Packing failed");

  dstrace("Packet dump:");
  dsdump(*res, *res_size);
  

  return rc;
}


int dsunpack(const char *tag, const void *data, int data_size, const void **res, int *res_size, int options)
{
  int rc;

  int offset = _unpack(tag, data, data_size, res, res_size);
  
  if(offset != data_size)
  {
    dslogw("Bad first packet format (%d)", offset);
    return -1;
  }

  dstrace("Pack 1 extracted, buf size %d", *res_size);
  
  rc = 0;

  if(options & DSPACK_SIGNED)
  {
    rc = -1;

    const void *buf = NULL;
    int buf_size = 0;
    offset = _unpack(tag, *res, *res_size, &buf, &buf_size);

    dstrace("Signature offset %d", offset);
    dstrace("Message \"%s\" size %d", (char *)buf, buf_size);

    if(offset >= 0 && offset < *res_size)
    {
      const void* signature = *res + offset;
      int signature_size = data_size - (*res - data) - offset;
      
      rc = dscrypto_verify(NULL, buf, buf_size, (unsigned char *)signature, signature_size);
      if(!rc)
      {
        *res = buf;
        *res_size = buf_size;
      }
    } // offset check
    else
    {
      dslogw("Incorrect packet format, bad offset");
    } // offset check
  } // DSPACK_SIGNED
  
  return rc;
}
