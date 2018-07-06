#ifndef PHP_DBSYNC_H
#define PHP_DBSYNC_H

extern zend_module_entry dbsync_module_entry;
#define phpext_dbsync_ptr &dbsync_module_entry

#define PHP_DBSYNC_VERSION "0.1.0"

#ifdef PHP_WIN32
#	define PHP_DBSYNC_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_DBSYNC_API __attribute__ ((visibility("default")))
#else
#	define PHP_DBSYNC_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

ZEND_BEGIN_MODULE_GLOBALS(dbsync)
char *g_dbsync_servers;
char *g_dbsync_signkey;
void *g_dbsync_ctx;
zend_long g_dbsync_keepalive; // 0 no keepalive, 1 per request, 2 totally
ZEND_END_MODULE_GLOBALS(dbsync)

/* Always refer to the globals in your function as DBSYNC_G(variable).
   You are encouraged to rename these macros something shorter, see
   examples in any other php module directory.
*/
#define DBSYNC_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(dbsync, v)

#if defined(ZTS) && defined(COMPILE_DL_DBSYNC)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#endif	/* PHP_DBSYNC_H */
