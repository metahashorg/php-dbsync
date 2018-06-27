#ifndef __DSCRYPTO_H__
#define __DSCRYPTO_H__

void  dscrypto_init(void);
void  dscrypto_cleanup(void);
void* dscrypto_load_public(const char *path);
void* dscrypto_load_private(const char *path);
void  dscrypto_keyfree(void *key);
int   dscrypto_verify(void *key, const void *data, int data_size, void *signature_buf, int signature_size);
int   dscrypto_signature(void *key, const void *data, int data_size, void **signature_buf, int *signature_size);

#endif /* __DSCRYPTO_H__ */
