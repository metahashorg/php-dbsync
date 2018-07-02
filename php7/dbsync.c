/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Vitali Smolin                                                |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_dbsync.h"

#include "dsmisc.h"
#include "dssend.h"
#include "dscrypto.h"



ZEND_DECLARE_MODULE_GLOBALS(dbsync)

/* True global resources - no need for thread safety here */
static int le_dbsync;

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("dbsync.servers", "127.0.0.1:1111", PHP_INI_ALL, OnUpdateString, g_dbsync_servers, zend_dbsync_globals, dbsync_globals)
    STD_PHP_INI_ENTRY("dbsync.signkey", NULL, PHP_INI_ALL, OnUpdateString, g_dbsync_signkey, zend_dbsync_globals, dbsync_globals)
PHP_INI_END()



ZEND_BEGIN_ARG_INFO_EX(arginfo_dbsync_send, 0, 0, 2)
  ZEND_ARG_INFO(0, cmd)
  ZEND_ARG_INFO(0, servers)
ZEND_END_ARG_INFO();

PHP_FUNCTION(dbsync_send)
{
  zend_string *cmd = NULL;
  zend_string *servers = NULL;
  size_t cmd_len, len;
  zend_string *strg = NULL;

  ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STR(cmd);
    Z_PARAM_OPTIONAL
    Z_PARAM_STR(servers);
  ZEND_PARSE_PARAMETERS_END();

  char *res = NULL;
  int res_size = 0;
  if(servers)
    dssend(ZSTR_VAL(servers), DBSYNC_G(g_dbsync_signkey)?1:0, ZSTR_VAL(cmd), &res, &res_size);
  else
    dssend(DBSYNC_G(g_dbsync_servers), DBSYNC_G(g_dbsync_signkey)?1:0, ZSTR_VAL(cmd), &res, &res_size);

  if(res)
  {
    dstrace("Return to script the string of size: %d", res_size);

    strg = strpprintf(res_size, "%s", res);
    free(res);
    RETURN_STR(strg);
  }
}


static void php_dbsync_init_globals(zend_dbsync_globals *dbsync_globals)
{
  dbsync_globals->g_dbsync_servers = NULL;
  dbsync_globals->g_dbsync_signkey = NULL;
}

PHP_MINIT_FUNCTION(dbsync)
{
  REGISTER_INI_ENTRIES();

  dscrypto_init();

  if(DBSYNC_G(g_dbsync_signkey))
    dscrypto_load_private(DBSYNC_G(g_dbsync_signkey));

  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(dbsync)
{
  UNREGISTER_INI_ENTRIES();

  dscrypto_keyfree(NULL);
  dscrypto_cleanup();

  return SUCCESS;
}

PHP_RINIT_FUNCTION(dbsync)
{
#if defined(COMPILE_DL_DBSYNC) && defined(ZTS)
  ZEND_TSRMLS_CACHE_UPDATE();
#endif
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(dbsync)
{
  return SUCCESS;
}

PHP_MINFO_FUNCTION(dbsync)
{
  php_info_print_table_start();
  php_info_print_table_header(2, "dbsync support", "enabled");
  php_info_print_table_end();

  DISPLAY_INI_ENTRIES();
}

/*
 * Every user visible function must have an entry in dbsync_functions[].
 */
const zend_function_entry dbsync_functions[] = {
  PHP_FE(dbsync_send, NULL)   /* Actual entry point for PHP. */
  PHP_FE_END  /* Must be the last line in dbsync_functions[] */
};


zend_module_entry dbsync_module_entry = {
  STANDARD_MODULE_HEADER,
  "dbsync",
  dbsync_functions,
  PHP_MINIT(dbsync),
  PHP_MSHUTDOWN(dbsync),
  PHP_RINIT(dbsync),
  PHP_RSHUTDOWN(dbsync),
  PHP_MINFO(dbsync),
  PHP_DBSYNC_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_DBSYNC
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(dbsync)
#endif
