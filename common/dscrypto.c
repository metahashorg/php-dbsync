#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ec.h>
#include <openssl/err.h>

#include "dsmisc.h"



void dscrypto_init(void)
{
  OpenSSL_add_all_algorithms();
  
  /* Load the human readable error strings for libcrypto */
  ERR_load_crypto_strings();

  //ERR_load_BIO_strings();
}


void dscrypto_cleanup(void)
{
  /* Removes all digests and ciphers */
  EVP_cleanup();

  /* if you omit the next, a small leak may be left when you make use of the BIO (low level API) for e.g. base64 transformations */
  CRYPTO_cleanup_all_ex_data();

  /* Remove error strings */
  ERR_free_strings();
}


EVP_PKEY *g_public;
void* dscrypto_load_public(const char *path)
{
  FILE *fp = fopen(path, "r");
  if(!fp)
  {
    dslogerr(errno, "Cannot open public key file '%s'", path);
    return NULL;
  }

  EVP_PKEY *pkey = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
  if(!pkey)
  {
    char errbuf[256];
    dslog("Cannot load public key: %s", ERR_error_string(ERR_get_error(), errbuf));
  }
  
  fclose(fp);
  
  g_public = pkey;
  
  return pkey;
}


EVP_PKEY *g_private;
void* dscrypto_load_private(const char *path)
{
  FILE *fp = fopen(path, "r");
  if(!fp)
  {
    dslogerr(errno, "Cannot open private key file '%s'", path);
    return NULL;
  }

  EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  if(!pkey)
  {
    char errbuf[256];
    dslog("Cannot load private key: %s", ERR_error_string(ERR_get_error(), errbuf));
  }
  
  fclose(fp);
  
  g_private = pkey;
  
  return pkey;
}


void dscrypto_keyfree(void *key)
{
  if(key)
  {
    EVP_PKEY_free((EVP_PKEY *)key);
  }
  else
  {
    if(g_private)
      EVP_PKEY_free(g_private);
    if(g_public)
      EVP_PKEY_free(g_public);    
  }
}


// Uses public key
int dscrypto_verify(void *key, const void *data, int data_size, void *signature_buf, int signature_size)
{
  int rc, ret = 0;
  EVP_PKEY *pkey = key;

  if(!pkey)
    pkey = g_public;

  if(!pkey)
  {
    dslog("Have no public key to verify signature");
    return -1;
  }

  dstrace("Verifying signature of size %d for %d size message", signature_size, data_size);

  /* Create the Message Digest Context */
  EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
  if(!mdctx)
  {
    dslog("Cannot create digest context");
    ret = -1;
  }

  // Initialise the DigestVerify operation - SHA-256 has been selected as the message digest function in this example
  if(!ret)
  {
    rc = EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey);
    if(rc != 1)
    {
      dslog("Cannot initialise SHA-256 verification");
      ret = -1;
    }
  }

  // Call update with the message
  if(!ret)
  {
    rc = EVP_DigestVerifyUpdate(mdctx, data, data_size);
    if(rc != 1)
    {
      dslog("Failed to update data for verification");
      ret = -1;
    }
  }

  // Obtain the signature
  if(!ret)
  {
    rc = EVP_DigestVerifyFinal(mdctx, signature_buf, signature_size);
    if(rc != 1)
    {
      dslog("Fail to verify signature");
      ret = -1;
    }
  }

  EVP_MD_CTX_destroy(mdctx);
  
  return ret;
}


// Uses private key
int dscrypto_signature(void *key, const void *data, int data_size, void **signature_buf, int *signature_size)
{
  int rc, ret = 0;
  EVP_PKEY *pkey = key;

  if(!pkey)
    pkey = g_private;

  if(!pkey)
  {
    dslog("Have no private key to build signature");
    return -1;
  }

  /* Create the Message Digest Context */
  EVP_MD_CTX *mdctx = EVP_MD_CTX_create();
  if(!mdctx)
  {
    dslog("Cannot create digest context");
    ret = -1;
  }

  // Initialise the DigestSign operation - SHA-256 has been selected as the message digest function in this example
  if(!ret)
  {
    rc = EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, pkey);
    if(rc != 1)
    {
      dslog("Cannot initialise SHA-256 signing");
      ret = -1;
    }
  }

  // Call update with the message
  if(!ret)
  {
    rc = EVP_DigestSignUpdate(mdctx, data, data_size);
    if(rc != 1)
    {
      dslog("Failed to sign the data");
      ret = -1;
    }
  }
  
  // Finalise the DigestSign operation
  // First call EVP_DigestSignFinal to obtain the length of the signature.
  size_t _signature_size = 0;
  if(!ret)
  {
    rc = EVP_DigestSignFinal(mdctx, NULL, &_signature_size);
    if(rc != 1)
    {
      dslog("Cannot obtain signature length");
      ret = -1;
    }
  }

  // Allocate memory for the signature based on size returned
  if(!ret)
  {
    *signature_size = _signature_size;

    *signature_buf = malloc(_signature_size);
    if(!*signature_buf)
    {
      dslogerr(errno, "Cannot allocate signature buffer");
      ret = -1;
    }
  }
  
  // Obtain the signature
  if(!ret)
  {
    rc = EVP_DigestSignFinal(mdctx, *signature_buf, &_signature_size);
    if(rc != 1)
    {
      dslog("Cannot obtain signature");
      ret = -1;
    }
  }

  EVP_MD_CTX_destroy(mdctx);
  
  dstrace("Built signature of size %d for %d size message", *signature_size, data_size);

  return ret;
}
