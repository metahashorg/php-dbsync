#ifndef SEND_H
#define SEND_H


void dssend(void *dsctx, int pack_signed, int keepalive, const char *msg, char **res, int *res_size);
void* dssend_init_ctx(const char *targets);
void dssend_release_ctx(void *dsctx);

#endif // SEND_H
