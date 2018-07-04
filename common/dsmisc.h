#ifndef __DSMISC_H__
#define __DSMISC_H__

void die(const char *msg, ...);
void dierr(int err, const char *msg, ...);
void dslog(const char *msg, ...);
void dslogerr(int err, const char *msg, ...);
void dslogw(const char *msg, ...);
void dslogwerr(int err, const char *msg, ...);

#ifdef DSDEBUG
void dstrace(const char *msg, ...);
void dstracerr(int err, const char *msg, ...);
void dsdump(const unsigned char *buf, int buf_size);
#else
inline void dstrace(const char *msg, ...) {};
inline void dstracerr(int err, const char *msg, ...) {};
inline void dsdump(const unsigned char *buf, int buf_size) {};
#endif

#endif /* __DSMISC_H__ */
