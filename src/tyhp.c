#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_tyhp.h"

ZEND_DECLARE_MODULE_GLOBALS(tyhp)

/* \Tyhp\Ext::typeIs(object $type, mixed $value): ?bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_tyhp_ext_type_is, 0, 2, MAY_BE_BOOL | MAY_BE_NULL)
	ZEND_ARG_TYPE_INFO(0, type, IS_OBJECT, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::typeParse(string $spelling): ?array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tyhp_ext_type_parse, 0, 1, IS_ARRAY, 1)
	ZEND_ARG_TYPE_INFO(0, spelling, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::bindGet(mixed $target, array $typeArguments): ?Closure */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_tyhp_ext_bind_get, 0, 2, Closure, 1)
	ZEND_ARG_TYPE_INFO(0, target, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, typeArguments, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::bindPut(mixed $target, array $typeArguments, Closure $bound): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tyhp_ext_bind_put, 0, 3, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, target, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, typeArguments, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, bound, Closure, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::closureTree(Closure $fn, string $mode): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tyhp_ext_closure_tree, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, fn, Closure, 0)
	ZEND_ARG_TYPE_INFO(0, mode, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::dnsStart(string $host): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tyhp_ext_dns_start, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, host, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::dnsTake(int $id): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tyhp_ext_dns_take, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* \Tyhp\Ext::dnsCancel(int $id): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_tyhp_ext_dns_cancel, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry tyhp_ext_methods[] = {
	ZEND_ME(Ext, typeIs, arginfo_tyhp_ext_type_is, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, typeParse, arginfo_tyhp_ext_type_parse, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, bindGet, arginfo_tyhp_ext_bind_get, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, bindPut, arginfo_tyhp_ext_bind_put, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, closureTree, arginfo_tyhp_ext_closure_tree, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, dnsStart, arginfo_tyhp_ext_dns_start, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, dnsTake, arginfo_tyhp_ext_dns_take, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Ext, dnsCancel, arginfo_tyhp_ext_dns_cancel, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_FE_END
};

PHP_GINIT_FUNCTION(tyhp)
{
#if defined(ZTS) && defined(COMPILE_DL_TYHP)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	tyhp_globals->bind_map = NULL;
	tyhp_globals->bind_closed = 1;
}

PHP_GSHUTDOWN_FUNCTION(tyhp)
{
	tyhp_bind_map_shutdown(tyhp_globals, 0);
}

PHP_MINIT_FUNCTION(tyhp)
{
	zend_class_entry ce;
	zend_class_entry *ext_ce;

#if defined(ZTS) && defined(COMPILE_DL_TYHP)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	tyhp_dns_module_init();

	INIT_NS_CLASS_ENTRY(ce, "Tyhp", "Ext", tyhp_ext_methods);
	ext_ce = zend_register_internal_class(&ce);
	ext_ce->ce_flags |= ZEND_ACC_FINAL;

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(tyhp)
{
#if defined(ZTS) && defined(COMPILE_DL_TYHP)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	tyhp_bind_map_shutdown(ZEND_MODULE_GLOBALS_BULK(tyhp), 0);
	tyhp_dns_module_shutdown();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(tyhp)
{
#if defined(ZTS) && defined(COMPILE_DL_TYHP)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	TYHP_G(bind_closed) = 0;
	return SUCCESS;
}

/*
 * The bind map holds references to request objects (closures and Type
 * instances). Drop them while those objects are still valid, so the next
 * request on this process never sees freed memory. After an unclean shutdown
 * the request heap is discarded as a whole, so the map is only forgotten.
 */
PHP_RSHUTDOWN_FUNCTION(tyhp)
{
	/* Leave the thread running. Joining here can block the request on the lookup. */
	tyhp_dns_request_shutdown();
	tyhp_bind_map_shutdown(ZEND_MODULE_GLOBALS_BULK(tyhp), CG(unclean_shutdown) ? 0 : 1);
	return SUCCESS;
}

PHP_MINFO_FUNCTION(tyhp)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "tyhp support", "enabled");
	php_info_print_table_row(2, "tyhp version", PHP_TYHP_VERSION);
	php_info_print_table_end();
}

zend_module_entry tyhp_module_entry = {
	STANDARD_MODULE_HEADER,
	"tyhp",
	NULL,
	PHP_MINIT(tyhp),
	PHP_MSHUTDOWN(tyhp),
	PHP_RINIT(tyhp),
	PHP_RSHUTDOWN(tyhp),
	PHP_MINFO(tyhp),
	PHP_TYHP_VERSION,
	PHP_MODULE_GLOBALS(tyhp),
	PHP_GINIT(tyhp),
	PHP_GSHUTDOWN(tyhp),
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_TYHP
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(tyhp)
#endif
