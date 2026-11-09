#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_closures.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"

#include "php_tyhp.h"

#if defined(ZTS) && defined(COMPILE_DL_TYHP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/*
 * Intern map for bindGet / bindPut. This file does not call a binder.
 * The key is the target plus arguments normalized the way Type::arg does,
 * without throwing: null stays null, a Tyhp\Type (or subclass) is keyed by
 * its handle, a Tyhp\BuiltIn or any other non-Type with toType() is replaced
 * by that Type, and a class / interface / enum name is replaced by
 * Type::fromClassName() when that method exists. Anything else cannot be
 * normalized. Instance methods (an array whose first element is an object,
 * or a Closure with a bound $this) are not stored.
 *
 * The map lives in module globals. Cached closures and normalized Type objects
 * are request objects, so the map holds a reference to each (their handles
 * cannot be reused while an entry exists) and drops those references in
 * RSHUTDOWN, while the objects are still valid. A SAPI that serves several
 * requests per process (FPM, the CLI server, embed) would otherwise read freed
 * objects on the next request. After that point the map is closed and
 * bindGet / bindPut do nothing until the next RINIT. MSHUTDOWN / GSHUTDOWN
 * only free the table itself.
 */

typedef enum {
	TYHP_TARGET_REJECT = 0,
	TYHP_TARGET_STRING,
	TYHP_TARGET_STATIC,
	TYHP_TARGET_CLOSURE
} tyhp_target_kind;

typedef struct {
	tyhp_target_kind kind;
	zend_string *text;
	zend_string *class_name;
	zend_string *method;
	zend_object *closure;
} tyhp_target;

typedef struct {
	zval *items;
	uint32_t count;
} tyhp_norms;

typedef struct {
	zval closure;
	zval target_hold;
	uint32_t argc;
	zval *args;
} tyhp_bind_entry;

/* Frees the entry's own memory without touching the objects it references. */
static void tyhp_bind_entry_forget(zval *zv)
{
	tyhp_bind_entry *entry = Z_PTR_P(zv);

	if (entry->args != NULL) {
		pefree(entry->args, 1);
	}
	pefree(entry, 1);
}

static void tyhp_bind_entry_release(tyhp_bind_entry *entry)
{
	zval closure = entry->closure;
	zval target_hold = entry->target_hold;
	zval *args = entry->args;
	uint32_t argc = entry->argc;
	uint32_t i;

	/* Free the entry first: dropping the last reference to a Type can run user code. */
	pefree(entry, 1);
	zval_ptr_dtor(&closure);
	zval_ptr_dtor(&target_hold);
	for (i = 0; i < argc; i++) {
		zval_ptr_dtor(&args[i]);
	}
	if (args != NULL) {
		pefree(args, 1);
	}
}

static void tyhp_bind_entry_dtor(zval *zv)
{
	tyhp_bind_entry_release(Z_PTR_P(zv));
}

/*
 * Closes the map and frees the table. With release true the cached objects are
 * dereferenced (RSHUTDOWN, objects still valid). With release false they are
 * only forgotten (the request heap is already gone, or the shutdown is unclean).
 */
void tyhp_bind_map_shutdown(zend_tyhp_globals *g, bool release)
{
	HashTable *ht = g->bind_map;

	g->bind_closed = 1;
	if (ht == NULL) {
		return;
	}
	g->bind_map = NULL;

	if (!release) {
		ht->pDestructor = tyhp_bind_entry_forget;
	}
	zend_hash_destroy(ht);
	pefree(ht, 1);
}

static HashTable *tyhp_bind_map_get(void)
{
	HashTable *ht = TYHP_G(bind_map);

	if (ht != NULL) {
		return ht;
	}

	ht = pemalloc(sizeof(HashTable), 1);
	zend_hash_init(ht, 8, NULL, tyhp_bind_entry_dtor, 1);
	TYHP_G(bind_map) = ht;
	return ht;
}

static zval *tyhp_deref(zval *zv)
{
	if (zv != NULL && Z_TYPE_P(zv) == IS_REFERENCE) {
		return Z_REFVAL_P(zv);
	}
	return zv;
}

static bool tyhp_ce_is(zend_class_entry *ce, const char *name, size_t len)
{
	while (ce != NULL) {
		if (ZSTR_LEN(ce->name) == len && zend_binary_strcasecmp(ZSTR_VAL(ce->name), len, name, len) == 0) {
			return true;
		}
		ce = ce->parent;
	}
	return false;
}

static bool tyhp_is_type_object(zend_object *obj)
{
	return tyhp_ce_is(obj->ce, "Tyhp\\Type", sizeof("Tyhp\\Type") - 1);
}

static void tyhp_clear_call_error(zval *retval)
{
	if (EG(exception)) {
		zend_clear_exception();
	}
	if (retval != NULL && Z_TYPE_P(retval) != IS_UNDEF) {
		zval_ptr_dtor(retval);
		ZVAL_UNDEF(retval);
	}
}

static bool tyhp_claim_type(zval *retval, zval *out)
{
	zval *value = retval;

	ZVAL_UNDEF(out);
	if (Z_TYPE_P(retval) == IS_REFERENCE) {
		value = Z_REFVAL_P(retval);
	}
	if (Z_TYPE_P(value) == IS_OBJECT && tyhp_is_type_object(Z_OBJ_P(value))) {
		ZVAL_COPY(out, value);
		zval_ptr_dtor(retval);
		return true;
	}
	if (Z_TYPE_P(retval) != IS_UNDEF) {
		zval_ptr_dtor(retval);
	}
	return false;
}

static bool tyhp_call_to_type(zval *obj, zval *out)
{
	zend_function *fn;
	zval retval;
	zval hold;
	bool ok;

	ZVAL_UNDEF(out);
	/* Hold the object: user code may reassign the reference that owns it. */
	ZVAL_COPY(&hold, obj);
	fn = zend_hash_str_find_ptr(&Z_OBJCE(hold)->function_table, "totype", sizeof("totype") - 1);
	if (fn == NULL || !(fn->common.fn_flags & ZEND_ACC_PUBLIC)) {
		zval_ptr_dtor(&hold);
		return false;
	}

	ZVAL_UNDEF(&retval);
	zend_call_known_instance_method_with_0_params(fn, Z_OBJ(hold), &retval);
	if (EG(exception)) {
		tyhp_clear_call_error(&retval);
		ok = false;
	} else {
		ok = tyhp_claim_type(&retval, out);
	}
	zval_ptr_dtor(&hold);
	return ok;
}

static zend_class_entry *tyhp_lookup_user_class(zend_string *name)
{
	zend_class_entry *ce;

	if (ZSTR_LEN(name) == 0) {
		return NULL;
	}
	ce = zend_lookup_class(name);
	if (EG(exception)) {
		zend_clear_exception();
		return NULL;
	}
	return ce;
}

/* class_exists || interface_exists || enum_exists, with autoload. Traits are neither. */
static bool tyhp_is_classish(zend_string *name)
{
	zend_class_entry *ce = tyhp_lookup_user_class(name);

	if (ce == NULL) {
		return false;
	}
	return (ce->ce_flags & ZEND_ACC_TRAIT) == 0;
}

static bool tyhp_from_class_name(zend_string *class_name, zval *out)
{
	zend_string *type_name;
	zend_class_entry *ce;
	zend_function *fn;
	zval param;
	zval retval;

	ZVAL_UNDEF(out);
	type_name = zend_string_init("Tyhp\\Type", sizeof("Tyhp\\Type") - 1, 0);
	ce = zend_lookup_class(type_name);
	zend_string_release(type_name);
	if (EG(exception)) {
		zend_clear_exception();
		return false;
	}
	if (ce == NULL) {
		return false;
	}

	fn = zend_hash_str_find_ptr(&ce->function_table, "fromclassname", sizeof("fromclassname") - 1);
	if (fn == NULL || !(fn->common.fn_flags & ZEND_ACC_PUBLIC) || !(fn->common.fn_flags & ZEND_ACC_STATIC)) {
		return false;
	}

	ZVAL_STR(&param, class_name);
	ZVAL_UNDEF(&retval);
	zend_call_known_function(fn, NULL, ce, &retval, 1, &param, NULL);
	if (EG(exception)) {
		tyhp_clear_call_error(&retval);
		return false;
	}
	return tyhp_claim_type(&retval, out);
}

static bool tyhp_normalize_one(zval *value, zval *out)
{
	ZVAL_UNDEF(out);
	value = tyhp_deref(value);
	if (value == NULL) {
		return false;
	}
	if (Z_TYPE_P(value) == IS_NULL) {
		ZVAL_NULL(out);
		return true;
	}
	if (Z_TYPE_P(value) == IS_OBJECT) {
		if (tyhp_is_type_object(Z_OBJ_P(value))) {
			ZVAL_COPY(out, value);
			return true;
		}
		return tyhp_call_to_type(value, out);
	}
	if (Z_TYPE_P(value) == IS_STRING) {
		/* Autoload and fromClassName run user code that may rewrite the source reference. */
		zend_string *name = zend_string_copy(Z_STR_P(value));
		bool ok = tyhp_is_classish(name) && tyhp_from_class_name(name, out);

		zend_string_release(name);
		return ok;
	}
	return false;
}

static void tyhp_norms_dtor(tyhp_norms *norms)
{
	uint32_t i;

	if (norms->items == NULL) {
		norms->count = 0;
		return;
	}
	for (i = 0; i < norms->count; i++) {
		zval_ptr_dtor(&norms->items[i]);
	}
	efree(norms->items);
	norms->items = NULL;
	norms->count = 0;
}

static bool tyhp_normalize_args(zval *args, tyhp_norms *out)
{
	uint32_t n = zend_hash_num_elements(Z_ARRVAL_P(args));
	uint32_t i = 0;
	zval *zv;

	out->count = 0;
	out->items = NULL;
	if (n == 0) {
		return true;
	}

	out->items = ecalloc(n, sizeof(zval));
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(args), zv) {
		if (!tyhp_normalize_one(zv, &out->items[i])) {
			tyhp_norms_dtor(out);
			return false;
		}
		i++;
		out->count = i;
	} ZEND_HASH_FOREACH_END();
	return true;
}

/* The target strings are held: normalizing arguments runs user code that may rewrite array elements. */
static void tyhp_target_release(tyhp_target *target)
{
	if (target->text != NULL) {
		zend_string_release(target->text);
	}
	if (target->class_name != NULL) {
		zend_string_release(target->class_name);
	}
	if (target->method != NULL) {
		zend_string_release(target->method);
	}
	memset(target, 0, sizeof(*target));
}

static tyhp_target_kind tyhp_classify_target(zval *target, tyhp_target *out)
{
	memset(out, 0, sizeof(*out));
	target = tyhp_deref(target);
	if (target == NULL) {
		return TYHP_TARGET_REJECT;
	}

	if (Z_TYPE_P(target) == IS_STRING) {
		out->kind = TYHP_TARGET_STRING;
		out->text = zend_string_copy(Z_STR_P(target));
		return TYHP_TARGET_STRING;
	}

	if (Z_TYPE_P(target) == IS_ARRAY) {
		HashTable *ht = Z_ARRVAL_P(target);
		zval *first = NULL;
		zval *z0;
		zval *z1;

		z0 = zend_hash_index_find(ht, 0);
		if (z0 != NULL) {
			first = z0;
		} else {
			ZEND_HASH_FOREACH_VAL(ht, first) {
				break;
			} ZEND_HASH_FOREACH_END();
		}
		first = tyhp_deref(first);
		if (first != NULL && Z_TYPE_P(first) == IS_OBJECT) {
			return TYHP_TARGET_REJECT;
		}

		z1 = zend_hash_index_find(ht, 1);
		if (zend_hash_num_elements(ht) == 2 && z0 != NULL && z1 != NULL) {
			z0 = tyhp_deref(z0);
			z1 = tyhp_deref(z1);
			if (Z_TYPE_P(z0) == IS_STRING && Z_TYPE_P(z1) == IS_STRING) {
				out->kind = TYHP_TARGET_STATIC;
				out->class_name = zend_string_copy(Z_STR_P(z0));
				out->method = zend_string_copy(Z_STR_P(z1));
				return TYHP_TARGET_STATIC;
			}
		}
		return TYHP_TARGET_REJECT;
	}

	if (Z_TYPE_P(target) == IS_OBJECT && instanceof_function(Z_OBJCE_P(target), zend_ce_closure)) {
		zval *this_ptr = zend_get_closure_this_ptr(target);

		if (this_ptr != NULL && Z_TYPE_P(this_ptr) == IS_REFERENCE) {
			this_ptr = Z_REFVAL_P(this_ptr);
		}
		if (this_ptr != NULL && Z_TYPE_P(this_ptr) == IS_OBJECT) {
			return TYHP_TARGET_REJECT;
		}
		out->kind = TYHP_TARGET_CLOSURE;
		out->closure = Z_OBJ_P(target);
		return TYHP_TARGET_CLOSURE;
	}

	return TYHP_TARGET_REJECT;
}

static void tyhp_key_u32(smart_str *key, uint32_t n)
{
	char buf[4];

	buf[0] = (char) (n & 0xff);
	buf[1] = (char) ((n >> 8) & 0xff);
	buf[2] = (char) ((n >> 16) & 0xff);
	buf[3] = (char) ((n >> 24) & 0xff);
	smart_str_appendl(key, buf, 4);
}

static void tyhp_key_size(smart_str *key, size_t n)
{
	uint64_t wide = (uint64_t) n;

	tyhp_key_u32(key, (uint32_t) wide);
	tyhp_key_u32(key, (uint32_t) (wide >> 32));
}

static zend_string *tyhp_bind_key(const tyhp_target *target, zval *args, uint32_t argc)
{
	smart_str key = {0};
	uint32_t i;

	if (target->kind == TYHP_TARGET_STRING) {
		smart_str_appendc(&key, 'f');
		tyhp_key_size(&key, ZSTR_LEN(target->text));
		smart_str_appendl(&key, ZSTR_VAL(target->text), ZSTR_LEN(target->text));
	} else if (target->kind == TYHP_TARGET_STATIC) {
		smart_str_appendc(&key, 's');
		tyhp_key_size(&key, ZSTR_LEN(target->class_name));
		smart_str_appendl(&key, ZSTR_VAL(target->class_name), ZSTR_LEN(target->class_name));
		tyhp_key_size(&key, ZSTR_LEN(target->method));
		smart_str_appendl(&key, ZSTR_VAL(target->method), ZSTR_LEN(target->method));
	} else {
		smart_str_appendc(&key, 'c');
		tyhp_key_u32(&key, target->closure->handle);
	}

	tyhp_key_u32(&key, argc);
	for (i = 0; i < argc; i++) {
		if (Z_TYPE(args[i]) == IS_NULL) {
			smart_str_appendc(&key, 'n');
		} else {
			smart_str_appendc(&key, 'h');
			tyhp_key_u32(&key, (uint32_t) Z_OBJ_HANDLE(args[i]));
		}
	}
	smart_str_0(&key);
	return key.s;
}

static tyhp_bind_entry *tyhp_entry_new(zval *bound, const tyhp_target *target, tyhp_norms *norms)
{
	tyhp_bind_entry *entry = pemalloc(sizeof(*entry), 1);
	uint32_t i;

	ZVAL_COPY(&entry->closure, bound);
	if (target->kind == TYHP_TARGET_CLOSURE) {
		ZVAL_OBJ_COPY(&entry->target_hold, target->closure);
	} else {
		ZVAL_UNDEF(&entry->target_hold);
	}

	entry->argc = norms->count;
	entry->args = NULL;
	if (norms->count > 0) {
		entry->args = pemalloc(sizeof(zval) * norms->count, 1);
		for (i = 0; i < norms->count; i++) {
			ZVAL_COPY_VALUE(&entry->args[i], &norms->items[i]);
			ZVAL_UNDEF(&norms->items[i]);
		}
	}
	return entry;
}

/* Copies the interned closure into out. The caller owns that reference. */
static bool tyhp_find_closure(const tyhp_target *target, tyhp_norms *norms, zval *out)
{
	HashTable *ht = TYHP_G(bind_map);
	zend_string *key;
	zval *found;

	if (ht == NULL) {
		return false;
	}
	key = tyhp_bind_key(target, norms->items, norms->count);
	found = zend_hash_str_find(ht, ZSTR_VAL(key), ZSTR_LEN(key));
	if (found != NULL) {
		tyhp_bind_entry *entry = Z_PTR_P(found);

		ZVAL_COPY(out, &entry->closure);
	}
	zend_string_release(key);
	return found != NULL;
}

/* Bind calls never throw. An exception raised while releasing user objects is dropped. */
static void tyhp_swallow_exception(void)
{
	if (EG(exception)) {
		zend_clear_exception();
	}
}

PHP_METHOD(Ext, bindGet)
{
	zval *target_zv;
	zval *args_zv;
	tyhp_target target;
	tyhp_norms norms;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_ZVAL(target_zv)
		Z_PARAM_ARRAY(args_zv)
	ZEND_PARSE_PARAMETERS_END();

	if (TYHP_G(bind_closed)) {
		RETURN_NULL();
	}
	if (tyhp_classify_target(target_zv, &target) == TYHP_TARGET_REJECT) {
		RETURN_NULL();
	}
	if (!tyhp_normalize_args(args_zv, &norms)) {
		tyhp_target_release(&target);
		tyhp_swallow_exception();
		RETURN_NULL();
	}

	if (!tyhp_find_closure(&target, &norms, return_value)) {
		ZVAL_NULL(return_value);
	}
	tyhp_norms_dtor(&norms);
	tyhp_target_release(&target);
	tyhp_swallow_exception();
}

PHP_METHOD(Ext, bindPut)
{
	zval *target_zv;
	zval *args_zv;
	zval *bound;
	tyhp_target target;
	tyhp_norms norms;
	tyhp_bind_entry *entry;
	tyhp_bind_entry *old = NULL;
	HashTable *ht;
	zend_string *key;
	zval *existing;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_ZVAL(target_zv)
		Z_PARAM_ARRAY(args_zv)
		Z_PARAM_OBJECT_OF_CLASS(bound, zend_ce_closure)
	ZEND_PARSE_PARAMETERS_END();

	if (TYHP_G(bind_closed)) {
		return;
	}
	if (tyhp_classify_target(target_zv, &target) == TYHP_TARGET_REJECT) {
		return;
	}
	if (!tyhp_normalize_args(args_zv, &norms)) {
		tyhp_target_release(&target);
		tyhp_swallow_exception();
		return;
	}

	/* User code may have run above. Re-check that the map is still open. */
	if (TYHP_G(bind_closed)) {
		tyhp_norms_dtor(&norms);
		tyhp_target_release(&target);
		tyhp_swallow_exception();
		return;
	}

	key = tyhp_bind_key(&target, norms.items, norms.count);
	entry = tyhp_entry_new(bound, &target, &norms);
	ht = tyhp_bind_map_get();

	/* The key is copied into persistent memory by the table. */
	existing = zend_hash_str_find(ht, ZSTR_VAL(key), ZSTR_LEN(key));
	if (existing != NULL) {
		old = Z_PTR_P(existing);
		ZVAL_PTR(existing, entry);
	} else {
		zval slot;

		ZVAL_PTR(&slot, entry);
		zend_hash_str_add_new(ht, ZSTR_VAL(key), ZSTR_LEN(key), &slot);
	}
	zend_string_release(key);

	/* Released after the table is consistent: freeing the old entry can run user code. */
	if (old != NULL) {
		tyhp_bind_entry_release(old);
	}
	tyhp_norms_dtor(&norms);
	tyhp_target_release(&target);
	tyhp_swallow_exception();
}
