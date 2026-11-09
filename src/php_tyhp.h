#ifndef PHP_TYHP_H
#define PHP_TYHP_H

extern zend_module_entry tyhp_module_entry;
#define phpext_tyhp_ptr &tyhp_module_entry

#define PHP_TYHP_VERSION "0.1.0"

ZEND_BEGIN_MODULE_GLOBALS(tyhp)
	HashTable *bind_map;
	/* True outside a request (before RINIT, after RSHUTDOWN): bindGet / bindPut do nothing. */
	bool bind_closed;
ZEND_END_MODULE_GLOBALS(tyhp)

ZEND_EXTERN_MODULE_GLOBALS(tyhp)

#define TYHP_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(tyhp, v)

PHP_METHOD(Ext, typeIs);
PHP_METHOD(Ext, typeParse);
PHP_METHOD(Ext, bindGet);
PHP_METHOD(Ext, bindPut);
PHP_METHOD(Ext, closureTree);
PHP_METHOD(Ext, dnsStart);
PHP_METHOD(Ext, dnsTake);
PHP_METHOD(Ext, dnsCancel);

/* Process-lifetime DNS jobs. The side thread never calls these. */
void tyhp_dns_module_init(void);
void tyhp_dns_module_shutdown(void);
void tyhp_dns_request_shutdown(void);

/*
 * Closes and frees the bind map. release true drops the references the map
 * holds on request objects (RSHUTDOWN). release false only frees the table.
 * Safe to call more than once.
 */
void tyhp_bind_map_shutdown(zend_tyhp_globals *g, bool release);

#endif
