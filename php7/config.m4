dnl $Id$
dnl config.m4 for extension dbsync


PHP_ARG_ENABLE(dbsync-debug, whether to enable debugging support in dbsync,
[  --enable-dbsync-debug        example: Enable debugging support in dbsync], no, no)

dnl Check whether to enable debugging
if test "$PHP_DBSYNC_DEBUG" != "no"; then
  AC_DEFINE(DSDEBUG,1,[Include debugging support in dbsync])
  DSDEBUG="-DDSDEBUG"
fi

  
PHP_ARG_ENABLE(dbsync, whether to enable dbsync support,
Make sure that the comment is aligned:
[  --enable-dbsync           Enable dbsync support])

if test "$PHP_DBSYNC" != "no"; then
  AC_DEFINE(HAVE_DBSYNCLIB,1,[ ])

  PHP_ADD_LIBRARY(:libcrypto.so.1.1, 1, DBSYNC_SHARED_LIBADD)
  
  rm -f common;ln -sf ../common
  PHP_ADD_INCLUDE(common)

  rm -f driver;ln -sf ../driver
  PHP_ADD_INCLUDE(driver)

  PHP_NEW_EXTENSION(dbsync, dbsync.c driver/dssend.c common/dspack.c common/dscrypto.c common/dsmisc.c, $ext_shared,, $DSDEBUG -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
  PHP_SUBST(DBSYNC_SHARED_LIBADD)
fi
