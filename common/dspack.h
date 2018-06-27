#ifndef __DSPACK_H__
#define __DSPACK_H__

#define DSPACK_SIGNED 1

int dspack(const char *tag, const void *data, int data_size, void **res, int *res_size, int options);
int dspack_bufsize(const char *tag, const void *data, int data_size, int *size);
int dspack_complete(const char *tag, const void *data, int data_size);
int dsunpack(const char *tag, const void *data, int data_size, const void **res, int *res_size, int options);

#endif /* __DSPACK_H__ */
