#ifndef DSREDIS_H
#define DSREDIS_H

int dsredis(const char *hostname, int port, const char *cmd, unsigned char **res, int *res_size);

#endif /* DSREDIS_H */
