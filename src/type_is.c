#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "zend_iterators.h"

#include "php_tyhp.h"

#if defined(ZTS) && defined(COMPILE_DL_TYHP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/* Mirrors Type::is(). Kinds this build does not walk return null so the
 * caller can fall back. A forward/lazy cycle returns null as well: Type::is()
 * would recurse without finishing. */
#define TYHP_MAX_FOLLOW 64
#define TYHP_MAX_DEPTH 128

typedef enum {
	TYHP_IS_FALSE = 0,
	TYHP_IS_TRUE = 1,
	TYHP_IS_UNKNOWN = 2
} tyhp_is_result;

typedef struct {
	zend_object *obj;
	int nullable_force; /* -1 read isNullable, 0 false, 1 true */
	bool unknown;
} tyhp_view;

typedef struct {
	zend_object *objs[TYHP_MAX_FOLLOW];
	uint32_t count;
} tyhp_follow_stack;

static tyhp_is_result tyhp_type_is(zend_object *type, zval *value, int depth);
static tyhp_is_result tyhp_compatible(zend_object *broad, zend_object *narrow, int depth);
static tyhp_is_result tyhp_generic_args_compatible(zend_object *broad, zend_object *narrow, int depth);

static tyhp_view tyhp_view_of(zend_object *obj)
{
	tyhp_view view;

	view.obj = obj;
	view.nullable_force = -1;
	view.unknown = false;
	return view;
}

/* The class that declares the property. A private property of Tyhp\Type is
 * not readable with a subclass (Tyhp\NamedType) as the scope, and a silent
 * read of an inaccessible property looks like null. */
static zend_class_entry *tyhp_prop_scope(zend_object *obj, const char *name, size_t name_len)
{
	zend_class_entry *ce;

	for (ce = obj->ce; ce != NULL; ce = ce->parent) {
		zend_property_info *info = zend_hash_str_find_ptr(&ce->properties_info, name, name_len);

		if (info != NULL && info->ce == ce) {
			return ce;
		}
	}
	return obj->ce;
}

static bool tyhp_copy_prop(zend_object *obj, const char *name, size_t name_len, zval *dest)
{
	zval rv;
	zval *zv;
	bool temporary;

	ZVAL_UNDEF(dest);
	if (obj == NULL || EG(exception)) {
		return false;
	}

	zv = zend_read_property(tyhp_prop_scope(obj, name, name_len), obj, name, name_len, /* silent */ 1, &rv);
	if (EG(exception)) {
		if (zv == &rv) {
			zval_ptr_dtor(&rv);
		}
		return false;
	}
	if (zv == NULL) {
		return false;
	}

	temporary = (zv == &rv);
	if (Z_TYPE_P(zv) == IS_INDIRECT) {
		zv = Z_INDIRECT_P(zv);
		temporary = false;
	}
	if (zv == NULL || Z_TYPE_P(zv) == IS_UNDEF) {
		if (temporary) {
			zval_ptr_dtor(&rv);
		}
		return false;
	}
	if (Z_TYPE_P(zv) == IS_REFERENCE) {
		zv = Z_REFVAL_P(zv);
		if (zv == NULL || Z_TYPE_P(zv) == IS_UNDEF) {
			if (temporary) {
				zval_ptr_dtor(&rv);
			}
			return false;
		}
	}

	ZVAL_COPY(dest, zv);
	if (temporary) {
		zval_ptr_dtor(&rv);
	}
	return true;
}

#define TYHP_PROP(obj, name, dest) tyhp_copy_prop((obj), (name), sizeof(name) - 1, (dest))

static bool tyhp_ce_named(zend_class_entry *ce, const char *name, size_t len)
{
	while (ce != NULL) {
		if (zend_string_equals_cstr(ce->name, name, len)) {
			return true;
		}
		ce = ce->parent;
	}
	return false;
}

static bool tyhp_is_named_type_ce(zend_class_entry *ce)
{
	return tyhp_ce_named(ce, "Tyhp\\NamedType", sizeof("Tyhp\\NamedType") - 1);
}

static bool tyhp_is_type_ce(zend_class_entry *ce)
{
	return tyhp_ce_named(ce, "Tyhp\\Type", sizeof("Tyhp\\Type") - 1);
}

static bool tyhp_follow_contains(const tyhp_follow_stack *stack, const zend_object *obj)
{
	uint32_t i;

	for (i = 0; i < stack->count; i++) {
		if (stack->objs[i] == obj) {
			return true;
		}
	}
	return false;
}

static bool tyhp_flag(zend_object *obj, const char *name, size_t len)
{
	zval zv;
	bool result = false;

	if (!tyhp_copy_prop(obj, name, len, &zv)) {
		return false;
	}
	result = Z_TYPE(zv) == IS_TRUE;
	zval_ptr_dtor(&zv);
	return result;
}

static bool tyhp_view_is_nullable(const tyhp_view *view)
{
	if (view->nullable_force == 1) {
		return true;
	}
	if (view->nullable_force == 0) {
		return false;
	}
	return tyhp_flag(view->obj, "isNullable", sizeof("isNullable") - 1);
}

static bool tyhp_kind_is(zend_object *obj, const char *kind)
{
	zval zv;
	bool result = false;

	if (!TYHP_PROP(obj, "kind", &zv)) {
		return false;
	}
	if (Z_TYPE(zv) == IS_STRING) {
		result = zend_string_equals_cstr(Z_STR(zv), kind, strlen(kind));
	}
	zval_ptr_dtor(&zv);
	return result;
}

static bool tyhp_name_is(zend_object *obj, const char *name)
{
	zval zv;
	bool result = false;

	if (!TYHP_PROP(obj, "name", &zv)) {
		return false;
	}
	if (Z_TYPE(zv) == IS_STRING) {
		result = zend_string_equals_cstr(Z_STR(zv), name, strlen(name));
	}
	zval_ptr_dtor(&zv);
	return result;
}

static void tyhp_apply_lazy(tyhp_view *inner, zend_string *op)
{
	bool placeholder;

	if (inner->unknown || EG(exception)) {
		inner->unknown = true;
		return;
	}

	placeholder = tyhp_flag(inner->obj, "placeholder", sizeof("placeholder") - 1);
	if (EG(exception)) {
		inner->unknown = true;
		return;
	}

	if (zend_string_equals_literal(op, "nullable")) {
		if (placeholder) {
			inner->unknown = true;
			return;
		}
		if (tyhp_view_is_nullable(inner)) {
			return;
		}
		if (tyhp_kind_is(inner->obj, "scalar")
			&& (tyhp_name_is(inner->obj, "null") || tyhp_name_is(inner->obj, "mixed"))) {
			return;
		}
		inner->nullable_force = 1;
		return;
	}

	if (zend_string_equals_literal(op, "nonNullable")) {
		if (placeholder) {
			inner->unknown = true;
			return;
		}
		if (!tyhp_view_is_nullable(inner)) {
			return;
		}
		inner->nullable_force = 0;
		return;
	}

	if (zend_string_equals_literal(op, "readonly") || zend_string_equals_literal(op, "name")) {
		if (placeholder) {
			inner->unknown = true;
		}
		return;
	}
}

static tyhp_view tyhp_follow(zend_object *obj, tyhp_follow_stack *stack, int depth)
{
	tyhp_view result = tyhp_view_of(obj);
	zval forward_zv, lazy_zv, op_zv;
	bool pushed = false;

	ZVAL_UNDEF(&forward_zv);
	ZVAL_UNDEF(&lazy_zv);
	ZVAL_UNDEF(&op_zv);

	if (obj == NULL || depth > TYHP_MAX_FOLLOW || tyhp_follow_contains(stack, obj) || stack->count >= TYHP_MAX_FOLLOW) {
		result.unknown = true;
		goto done;
	}

	stack->objs[stack->count++] = obj;
	pushed = true;

	if (!TYHP_PROP(obj, "forward", &forward_zv)) {
		if (EG(exception)) {
			result.unknown = true;
			goto done;
		}
	} else if (Z_TYPE(forward_zv) == IS_OBJECT) {
		result = tyhp_follow(Z_OBJ(forward_zv), stack, depth + 1);
		goto done;
	} else if (Z_TYPE(forward_zv) != IS_NULL) {
		result.unknown = true;
		goto done;
	}

	if (!TYHP_PROP(obj, "lazyInner", &lazy_zv)) {
		if (EG(exception)) {
			result.unknown = true;
			goto done;
		}
	} else if (Z_TYPE(lazy_zv) == IS_OBJECT) {
		result = tyhp_follow(Z_OBJ(lazy_zv), stack, depth + 1);
		if (result.unknown || EG(exception)) {
			result.unknown = true;
			goto done;
		}
		if (!TYHP_PROP(obj, "lazyOp", &op_zv) || Z_TYPE(op_zv) != IS_STRING) {
			if (EG(exception)) {
				result.unknown = true;
			}
			goto done;
		}
		tyhp_apply_lazy(&result, Z_STR(op_zv));
		goto done;
	} else if (Z_TYPE(lazy_zv) != IS_NULL && Z_TYPE(lazy_zv) != IS_UNDEF) {
		result.unknown = true;
		goto done;
	}

done:
	zval_ptr_dtor(&forward_zv);
	zval_ptr_dtor(&lazy_zv);
	zval_ptr_dtor(&op_zv);
	if (pushed) {
		stack->count--;
	}
	return result;
}

static zend_class_entry *tyhp_lookup_class(zend_string *name, bool autoload)
{
	zend_string *stripped = NULL;
	zend_string *lookup;
	zend_class_entry *ce;
	uint32_t flags = autoload ? 0 : ZEND_FETCH_CLASS_NO_AUTOLOAD;

	if (name == NULL || ZSTR_LEN(name) == 0) {
		return NULL;
	}
	lookup = name;
	if (ZSTR_VAL(name)[0] == '\\') {
		if (ZSTR_LEN(name) == 1) {
			return NULL;
		}
		stripped = zend_string_init(ZSTR_VAL(name) + 1, ZSTR_LEN(name) - 1, 0);
		lookup = stripped;
	}

	ce = zend_lookup_class_ex(lookup, NULL, flags);
	if (stripped != NULL) {
		zend_string_release(stripped);
	}
	return ce;
}

static bool tyhp_ce_is_class(const zend_class_entry *ce)
{
	return ce != NULL
		&& (ce->ce_flags & ZEND_ACC_INTERFACE) == 0
		&& (ce->ce_flags & ZEND_ACC_TRAIT) == 0;
}

static bool tyhp_ce_is_interface(const zend_class_entry *ce)
{
	return ce != NULL && (ce->ce_flags & ZEND_ACC_INTERFACE) != 0;
}

static bool tyhp_ce_is_trait(const zend_class_entry *ce)
{
	return ce != NULL && (ce->ce_flags & ZEND_ACC_TRAIT) != 0;
}

static tyhp_is_result tyhp_object_is_named_type(zend_object *value, zend_string *class_name)
{
	zend_class_entry *ce;

	if (class_name == NULL || ZSTR_LEN(class_name) == 0) {
		return TYHP_IS_FALSE;
	}

	ce = tyhp_lookup_class(class_name, false);
	if (EG(exception)) {
		return TYHP_IS_UNKNOWN;
	}
	if (tyhp_ce_is_class(ce) || tyhp_ce_is_interface(ce)) {
		return instanceof_function(value->ce, ce) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	return TYHP_IS_FALSE;
}

static tyhp_is_result tyhp_named_type_is_a(zend_string *narrow_name, zend_string *broad_name)
{
	zend_class_entry *narrow;
	zend_class_entry *broad;

	if (narrow_name == NULL || ZSTR_LEN(narrow_name) == 0 || broad_name == NULL || ZSTR_LEN(broad_name) == 0) {
		return TYHP_IS_FALSE;
	}

	narrow = tyhp_lookup_class(narrow_name, true);
	if (EG(exception)) {
		return TYHP_IS_UNKNOWN;
	}

	if (tyhp_ce_is_class(narrow)) {
		broad = tyhp_lookup_class(broad_name, false);
		if (EG(exception)) {
			return TYHP_IS_UNKNOWN;
		}
		if (tyhp_ce_is_class(broad) || tyhp_ce_is_interface(broad)) {
			return instanceof_function(narrow, broad) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
		}
		return TYHP_IS_FALSE;
	}

	if (tyhp_ce_is_interface(narrow)) {
		broad = tyhp_lookup_class(broad_name, false);
		if (EG(exception)) {
			return TYHP_IS_UNKNOWN;
		}
		if (tyhp_ce_is_class(broad) || tyhp_ce_is_interface(broad)) {
			return instanceof_function(narrow, broad) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
		}
		return TYHP_IS_FALSE;
	}

	if (!tyhp_ce_is_trait(narrow)) {
		return TYHP_IS_FALSE;
	}

	broad = tyhp_lookup_class(broad_name, false);
	if (EG(exception)) {
		return TYHP_IS_UNKNOWN;
	}
	if (!tyhp_ce_is_trait(broad)) {
		return TYHP_IS_FALSE;
	}
	return narrow == broad ? TYHP_IS_TRUE : TYHP_IS_FALSE;
}

static bool tyhp_is_iterable(zval *value)
{
	if (Z_TYPE_P(value) == IS_ARRAY) {
		return true;
	}
	if (Z_TYPE_P(value) == IS_OBJECT && zend_ce_traversable != NULL) {
		return instanceof_function(Z_OBJCE_P(value), zend_ce_traversable);
	}
	return false;
}

static zval *tyhp_deref(zval *zv)
{
	if (zv == NULL) {
		return NULL;
	}
	if (Z_TYPE_P(zv) == IS_INDIRECT) {
		zv = Z_INDIRECT_P(zv);
	}
	if (zv != NULL && Z_TYPE_P(zv) == IS_REFERENCE) {
		zv = Z_REFVAL_P(zv);
	}
	return zv;
}

static tyhp_is_result tyhp_scalar_name(zend_string *name, zval *value)
{
	if (name == NULL) {
		return TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "string")) {
		return Z_TYPE_P(value) == IS_STRING ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "int")) {
		return Z_TYPE_P(value) == IS_LONG ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "float")) {
		return (Z_TYPE_P(value) == IS_DOUBLE || Z_TYPE_P(value) == IS_LONG) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "bool")) {
		return (Z_TYPE_P(value) == IS_TRUE || Z_TYPE_P(value) == IS_FALSE) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "null") || zend_string_equals_literal(name, "void")) {
		return Z_TYPE_P(value) == IS_NULL ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "mixed")) {
		return TYHP_IS_TRUE;
	}
	if (zend_string_equals_literal(name, "never")) {
		return TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "array")) {
		return Z_TYPE_P(value) == IS_ARRAY ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "object")) {
		return Z_TYPE_P(value) == IS_OBJECT ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "callable")) {
		return zend_is_callable(value, 0, NULL) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "iterable")) {
		return tyhp_is_iterable(value) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	if (zend_string_equals_literal(name, "resource")) {
		/* Closed resources stay IS_RESOURCE; is_resource() is false for them,
		 * and get_debug_type() reports "resource (closed)". Either matches. */
		return Z_TYPE_P(value) == IS_RESOURCE ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	return TYHP_IS_FALSE;
}

static tyhp_is_result tyhp_is_scalar(zend_object *type, zval *value)
{
	zval name_zv;
	tyhp_is_result result;
	bool is_null;

	if (!TYHP_PROP(type, "name", &name_zv)) {
		return EG(exception) ? TYHP_IS_UNKNOWN : TYHP_IS_FALSE;
	}
	if (Z_TYPE(name_zv) != IS_STRING) {
		is_null = Z_TYPE(name_zv) == IS_NULL;
		zval_ptr_dtor(&name_zv);
		return is_null ? TYHP_IS_FALSE : TYHP_IS_UNKNOWN;
	}
	result = tyhp_scalar_name(Z_STR(name_zv), value);
	zval_ptr_dtor(&name_zv);
	return result;
}

static tyhp_is_result tyhp_is_class(zend_object *type, zval *value)
{
	zval name_zv;
	zend_string *name = NULL;
	tyhp_is_result result;

	if (Z_TYPE_P(value) != IS_OBJECT) {
		return TYHP_IS_FALSE;
	}

	ZVAL_UNDEF(&name_zv);
	if (!TYHP_PROP(type, "name", &name_zv)) {
		if (EG(exception)) {
			return TYHP_IS_UNKNOWN;
		}
	} else if (Z_TYPE(name_zv) == IS_STRING) {
		name = Z_STR(name_zv);
	} else if (Z_TYPE(name_zv) != IS_NULL) {
		zval_ptr_dtor(&name_zv);
		return TYHP_IS_UNKNOWN;
	}

	result = tyhp_object_is_named_type(Z_OBJ_P(value), name);
	zval_ptr_dtor(&name_zv);
	return result;
}

static tyhp_is_result tyhp_is_union(zend_object *type, zval *value, int depth)
{
	zval args;
	zval *member;
	bool unknown = false;

	ZVAL_UNDEF(&args);
	if (!TYHP_PROP(type, "typeArgs", &args) || Z_TYPE(args) != IS_ARRAY) {
		zval_ptr_dtor(&args);
		return TYHP_IS_UNKNOWN;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(args), member) {
		zval *elem = tyhp_deref(member);
		tyhp_is_result one;

		if (elem == NULL || Z_TYPE_P(elem) != IS_OBJECT) {
			zval_ptr_dtor(&args);
			return TYHP_IS_UNKNOWN;
		}
		one = tyhp_type_is(Z_OBJ_P(elem), value, depth + 1);
		if (EG(exception)) {
			zval_ptr_dtor(&args);
			return TYHP_IS_UNKNOWN;
		}
		if (one == TYHP_IS_TRUE) {
			zval_ptr_dtor(&args);
			return TYHP_IS_TRUE;
		}
		if (one == TYHP_IS_UNKNOWN) {
			unknown = true;
		}
	} ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&args);
	return unknown ? TYHP_IS_UNKNOWN : TYHP_IS_FALSE;
}

static tyhp_is_result tyhp_is_intersection(zend_object *type, zval *value, int depth)
{
	zval args;
	zval *member;

	ZVAL_UNDEF(&args);
	if (!TYHP_PROP(type, "typeArgs", &args) || Z_TYPE(args) != IS_ARRAY) {
		zval_ptr_dtor(&args);
		return TYHP_IS_UNKNOWN;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(args), member) {
		zval *elem = tyhp_deref(member);
		tyhp_is_result one;

		if (elem == NULL || Z_TYPE_P(elem) != IS_OBJECT) {
			zval_ptr_dtor(&args);
			return TYHP_IS_UNKNOWN;
		}
		one = tyhp_type_is(Z_OBJ_P(elem), value, depth + 1);
		if (EG(exception) || one != TYHP_IS_TRUE) {
			zval_ptr_dtor(&args);
			return EG(exception) ? TYHP_IS_UNKNOWN : one;
		}
	} ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&args);
	return TYHP_IS_TRUE;
}

static bool tyhp_key_exists(HashTable *ht, zval *key, bool *supported)
{
	key = tyhp_deref(key);
	*supported = true;
	if (key == NULL) {
		*supported = false;
		return false;
	}
	if (Z_TYPE_P(key) == IS_LONG) {
		return zend_hash_index_exists(ht, (zend_ulong) Z_LVAL_P(key));
	}
	if (Z_TYPE_P(key) == IS_STRING) {
		return zend_symtable_exists(ht, Z_STR_P(key));
	}
	*supported = false;
	return false;
}

static zval *tyhp_strict_find(HashTable *ht, zval *key)
{
	zend_ulong h;
	zend_string *str;
	zval *entry;

	key = tyhp_deref(key);
	if (key == NULL) {
		return NULL;
	}

	if (Z_TYPE_P(key) == IS_LONG) {
		zend_ulong want = (zend_ulong) Z_LVAL_P(key);

		ZEND_HASH_FOREACH_KEY_VAL(ht, h, str, entry) {
			if (str == NULL && h == want) {
				return entry;
			}
		} ZEND_HASH_FOREACH_END();
		return NULL;
	}

	if (Z_TYPE_P(key) == IS_STRING) {
		ZEND_HASH_FOREACH_KEY_VAL(ht, h, str, entry) {
			(void) h;
			if (str != NULL && zend_string_equals(str, Z_STR_P(key))) {
				return entry;
			}
		} ZEND_HASH_FOREACH_END();
	}
	return NULL;
}

static tyhp_is_result tyhp_is_struct(zend_object *type, zval *value, int depth)
{
	zval required_zv, fields_zv;
	zval *required_key;
	zend_ulong field_h;
	zend_string *field_key;
	zval *field_type;
	zval null_zv;

	if (Z_TYPE_P(value) != IS_ARRAY) {
		return TYHP_IS_FALSE;
	}

	ZVAL_UNDEF(&required_zv);
	ZVAL_UNDEF(&fields_zv);
	if (!TYHP_PROP(type, "structRequiredKeys", &required_zv) || Z_TYPE(required_zv) != IS_ARRAY) {
		zval_ptr_dtor(&required_zv);
		return TYHP_IS_UNKNOWN;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(required_zv), required_key) {
		bool supported = false;
		bool exists = tyhp_key_exists(Z_ARRVAL_P(value), required_key, &supported);

		if (!supported) {
			zval_ptr_dtor(&required_zv);
			return TYHP_IS_UNKNOWN;
		}
		if (!exists) {
			zval_ptr_dtor(&required_zv);
			return TYHP_IS_FALSE;
		}
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(&required_zv);

	if (!TYHP_PROP(type, "structFields", &fields_zv) || Z_TYPE(fields_zv) != IS_ARRAY) {
		zval_ptr_dtor(&fields_zv);
		return TYHP_IS_UNKNOWN;
	}

	ZVAL_NULL(&null_zv);
	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL(fields_zv), field_h, field_key, field_type) {
		zval key_zv;
		zval *present;
		zval *elem = tyhp_deref(field_type);
		zval *field_value;
		tyhp_is_result one;
		bool supported = false;

		if (elem == NULL || Z_TYPE_P(elem) != IS_OBJECT) {
			zval_ptr_dtor(&fields_zv);
			return TYHP_IS_UNKNOWN;
		}

		if (field_key != NULL) {
			ZVAL_STR(&key_zv, field_key);
		} else {
			ZVAL_LONG(&key_zv, (zend_long) field_h);
		}

		if (!tyhp_key_exists(Z_ARRVAL_P(value), &key_zv, &supported)) {
			if (!supported) {
				zval_ptr_dtor(&fields_zv);
				return TYHP_IS_UNKNOWN;
			}
			continue;
		}

		present = tyhp_strict_find(Z_ARRVAL_P(value), &key_zv);
		if (present == NULL) {
			field_value = &null_zv;
		} else {
			field_value = tyhp_deref(present);
			if (field_value == NULL) {
				field_value = &null_zv;
			}
		}

		one = tyhp_type_is(Z_OBJ_P(elem), field_value, depth + 1);
		if (EG(exception) || one != TYHP_IS_TRUE) {
			zval_ptr_dtor(&fields_zv);
			return EG(exception) ? TYHP_IS_UNKNOWN : one;
		}
	} ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&fields_zv);
	return TYHP_IS_TRUE;
}

static tyhp_is_result tyhp_each_array(HashTable *ht, zend_object *key_type, zend_object *val_type, int depth)
{
	zend_ulong h;
	zend_string *key_str;
	zval *entry;

	ZEND_HASH_FOREACH_KEY_VAL(ht, h, key_str, entry) {
		zval key_zv;
		zval *elem = tyhp_deref(entry);
		tyhp_is_result one;

		if (elem == NULL) {
			return TYHP_IS_UNKNOWN;
		}
		if (key_type != NULL) {
			if (key_str != NULL) {
				ZVAL_STR(&key_zv, key_str);
			} else {
				ZVAL_LONG(&key_zv, (zend_long) h);
			}
			one = tyhp_type_is(key_type, &key_zv, depth + 1);
			if (EG(exception) || one != TYHP_IS_TRUE) {
				return EG(exception) ? TYHP_IS_UNKNOWN : one;
			}
		}
		one = tyhp_type_is(val_type, elem, depth + 1);
		if (EG(exception) || one != TYHP_IS_TRUE) {
			return EG(exception) ? TYHP_IS_UNKNOWN : one;
		}
	} ZEND_HASH_FOREACH_END();

	return TYHP_IS_TRUE;
}

static tyhp_is_result tyhp_each_iterator(zval *value, zend_object *key_type, zend_object *val_type, int depth)
{
	zend_class_entry *ce;
	zend_object_iterator *iter;
	zend_ulong index = 0;
	tyhp_is_result result = TYHP_IS_TRUE;

	if (Z_TYPE_P(value) != IS_OBJECT) {
		return TYHP_IS_UNKNOWN;
	}
	ce = Z_OBJCE_P(value);
	if (ce->get_iterator == NULL) {
		return TYHP_IS_UNKNOWN;
	}

	iter = ce->get_iterator(ce, value, 0);
	if (iter == NULL || EG(exception)) {
		if (iter != NULL) {
			zend_iterator_dtor(iter);
		}
		return TYHP_IS_UNKNOWN;
	}

	if (iter->funcs->rewind) {
		iter->funcs->rewind(iter);
		if (EG(exception)) {
			zend_iterator_dtor(iter);
			return TYHP_IS_UNKNOWN;
		}
	}

	while (iter->funcs->valid(iter) == SUCCESS) {
		zval *entry;
		zval key;
		zval index_key;
		zval *key_ptr;
		tyhp_is_result one;

		if (EG(exception)) {
			result = TYHP_IS_UNKNOWN;
			break;
		}

		entry = iter->funcs->get_current_data(iter);
		if (EG(exception) || entry == NULL) {
			result = TYHP_IS_UNKNOWN;
			break;
		}
		entry = tyhp_deref(entry);
		if (entry == NULL) {
			result = TYHP_IS_UNKNOWN;
			break;
		}

		ZVAL_UNDEF(&key);
		if (key_type != NULL) {
			if (iter->funcs->get_current_key) {
				iter->funcs->get_current_key(iter, &key);
				if (EG(exception)) {
					zval_ptr_dtor(&key);
					result = TYHP_IS_UNKNOWN;
					break;
				}
				key_ptr = tyhp_deref(&key);
			} else {
				ZVAL_LONG(&index_key, (zend_long) index);
				key_ptr = &index_key;
			}
			if (key_ptr == NULL) {
				zval_ptr_dtor(&key);
				result = TYHP_IS_UNKNOWN;
				break;
			}
			one = tyhp_type_is(key_type, key_ptr, depth + 1);
			if (EG(exception) || one != TYHP_IS_TRUE) {
				zval_ptr_dtor(&key);
				result = EG(exception) ? TYHP_IS_UNKNOWN : one;
				break;
			}
		}

		one = tyhp_type_is(val_type, entry, depth + 1);
		zval_ptr_dtor(&key);
		if (EG(exception) || one != TYHP_IS_TRUE) {
			result = EG(exception) ? TYHP_IS_UNKNOWN : one;
			break;
		}

		index++;
		iter->funcs->move_forward(iter);
		if (EG(exception)) {
			result = TYHP_IS_UNKNOWN;
			break;
		}
	}

	zend_iterator_dtor(iter);
	return result;
}

static tyhp_is_result tyhp_is_array_of(zend_object *type, zval *value, int depth, bool as_array)
{
	zval args;
	uint32_t argc;
	zval *key_arg = NULL;
	zval *val_arg;
	zend_object *key_type = NULL;
	zend_object *val_type;
	tyhp_is_result result;

	if (as_array) {
		if (Z_TYPE_P(value) != IS_ARRAY) {
			return TYHP_IS_FALSE;
		}
	} else if (!tyhp_is_iterable(value)) {
		return TYHP_IS_FALSE;
	}

	ZVAL_UNDEF(&args);
	if (!TYHP_PROP(type, "typeArgs", &args) || Z_TYPE(args) != IS_ARRAY) {
		zval_ptr_dtor(&args);
		return TYHP_IS_UNKNOWN;
	}

	argc = zend_hash_num_elements(Z_ARRVAL(args));
	if (argc == 0) {
		zval_ptr_dtor(&args);
		if (!as_array && Z_TYPE_P(value) != IS_ARRAY) {
			return tyhp_is_iterable(value) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
		}
		return TYHP_IS_TRUE;
	}

	if (argc == 1) {
		val_arg = zend_hash_index_find(Z_ARRVAL(args), 0);
	} else {
		key_arg = zend_hash_index_find(Z_ARRVAL(args), 0);
		val_arg = zend_hash_index_find(Z_ARRVAL(args), 1);
	}
	if (val_arg == NULL || (argc != 1 && key_arg == NULL)) {
		zval_ptr_dtor(&args);
		return TYHP_IS_UNKNOWN;
	}
	val_arg = tyhp_deref(val_arg);
	if (key_arg != NULL) {
		key_arg = tyhp_deref(key_arg);
	}
	if (val_arg == NULL || Z_TYPE_P(val_arg) != IS_OBJECT || (key_arg != NULL && Z_TYPE_P(key_arg) != IS_OBJECT)) {
		zval_ptr_dtor(&args);
		return TYHP_IS_UNKNOWN;
	}

	val_type = Z_OBJ_P(val_arg);
	if (key_arg != NULL) {
		key_type = Z_OBJ_P(key_arg);
	}

	if (Z_TYPE_P(value) == IS_ARRAY) {
		result = tyhp_each_array(Z_ARRVAL_P(value), key_type, val_type, depth);
	} else {
		result = tyhp_each_iterator(value, key_type, val_type, depth);
	}
	zval_ptr_dtor(&args);
	return result;
}

static zend_object *tyhp_generic_object_type(zval *value, zval *hold)
{
	zend_string *prop;
	zval bag;
	zval retval;
	int exists;

	ZVAL_UNDEF(hold);
	ZVAL_UNDEF(&bag);
	ZVAL_UNDEF(&retval);
	if (Z_TYPE_P(value) != IS_OBJECT || Z_OBJ_HANDLER_P(value, has_property) == NULL) {
		return NULL;
	}

	prop = zend_string_init("__tyhpGeneric", sizeof("__tyhpGeneric") - 1, 0);
	exists = Z_OBJ_HANDLER_P(value, has_property)(Z_OBJ_P(value), prop, ZEND_PROPERTY_EXISTS, NULL);
	zend_string_release(prop);
	if (!exists || EG(exception)) {
		return NULL;
	}

	if (!TYHP_PROP(Z_OBJ_P(value), "__tyhpGeneric", &bag) || Z_TYPE(bag) != IS_OBJECT) {
		zval_ptr_dtor(&bag);
		return NULL;
	}
	if (!zend_hash_str_exists(&Z_OBJCE(bag)->function_table, "objecttype", sizeof("objecttype") - 1)) {
		zval_ptr_dtor(&bag);
		return NULL;
	}

	zend_call_method_with_0_params(Z_OBJ(bag), Z_OBJCE(bag), NULL, "objectType", &retval);
	zval_ptr_dtor(&bag);
	if (EG(exception) || Z_TYPE(retval) != IS_OBJECT || !tyhp_is_type_ce(Z_OBJCE(retval))) {
		zval_ptr_dtor(&retval);
		return NULL;
	}

	ZVAL_COPY_VALUE(hold, &retval);
	return Z_OBJ_P(hold);
}

static tyhp_is_result tyhp_is_generic(zend_object *type, zval *value, int depth)
{
	zval name_zv;
	bool as_array = false;
	bool as_iterable = false;
	tyhp_is_result named;
	zval recorded;
	zend_object *recorded_type;

	ZVAL_UNDEF(&name_zv);
	if (!TYHP_PROP(type, "name", &name_zv)) {
		if (EG(exception)) {
			return TYHP_IS_UNKNOWN;
		}
	} else if (Z_TYPE(name_zv) == IS_STRING) {
		as_array = zend_string_equals_literal(Z_STR(name_zv), "array");
		as_iterable = zend_string_equals_literal(Z_STR(name_zv), "iterable");
	} else if (Z_TYPE(name_zv) != IS_NULL) {
		zval_ptr_dtor(&name_zv);
		return TYHP_IS_UNKNOWN;
	}

	if (as_array || as_iterable) {
		tyhp_is_result result = tyhp_is_array_of(type, value, depth, as_array);
		zval_ptr_dtor(&name_zv);
		return result;
	}

	if (Z_TYPE_P(value) != IS_OBJECT) {
		zval_ptr_dtor(&name_zv);
		return TYHP_IS_FALSE;
	}

	named = tyhp_object_is_named_type(
		Z_OBJ_P(value),
		Z_TYPE(name_zv) == IS_STRING ? Z_STR(name_zv) : NULL
	);
	zval_ptr_dtor(&name_zv);
	if (named != TYHP_IS_TRUE) {
		return named;
	}

	recorded_type = tyhp_generic_object_type(value, &recorded);
	if (EG(exception)) {
		zval_ptr_dtor(&recorded);
		return TYHP_IS_UNKNOWN;
	}
	if (recorded_type == NULL) {
		zval_ptr_dtor(&recorded);
		return TYHP_IS_TRUE;
	}

	named = tyhp_compatible(type, recorded_type, depth + 1);
	zval_ptr_dtor(&recorded);
	return named;
}

static bool tyhp_unwrap_named(zend_object *obj, zend_object **out, zval *hold)
{
	ZVAL_UNDEF(hold);
	*out = obj;
	if (obj == NULL) {
		return false;
	}
	if (!tyhp_is_named_type_ce(obj->ce)) {
		return true;
	}
	if (!TYHP_PROP(obj, "underlyingType", hold) || Z_TYPE_P(hold) != IS_OBJECT) {
		zval_ptr_dtor(hold);
		ZVAL_UNDEF(hold);
		return false;
	}
	*out = Z_OBJ_P(hold);
	return true;
}

static bool tyhp_read_kind_name(zend_object *obj, zval *kind_zv, zval *name_zv)
{
	ZVAL_UNDEF(kind_zv);
	ZVAL_UNDEF(name_zv);
	if (!TYHP_PROP(obj, "kind", kind_zv) || Z_TYPE_P(kind_zv) != IS_STRING) {
		return false;
	}
	if (!TYHP_PROP(obj, "name", name_zv)) {
		if (EG(exception)) {
			return false;
		}
		ZVAL_NULL(name_zv);
		return true;
	}
	if (Z_TYPE_P(name_zv) != IS_STRING && Z_TYPE_P(name_zv) != IS_NULL) {
		return false;
	}
	return true;
}

static bool tyhp_same_name(zval *left, zval *right)
{
	if (Z_TYPE_P(left) == IS_NULL && Z_TYPE_P(right) == IS_NULL) {
		return true;
	}
	if (Z_TYPE_P(left) != IS_STRING || Z_TYPE_P(right) != IS_STRING) {
		return false;
	}
	return zend_string_equals(Z_STR_P(left), Z_STR_P(right));
}

static tyhp_is_result tyhp_union_compatible(zend_object *broad, zend_object *narrow, zend_string *narrow_kind, int depth)
{
	zval broad_args, narrow_args;
	zval *broad_member;
	zval *narrow_member;
	bool narrow_is_union;

	ZVAL_UNDEF(&broad_args);
	ZVAL_UNDEF(&narrow_args);
	narrow_is_union = narrow_kind != NULL && zend_string_equals_literal(narrow_kind, "union");

	if (!TYHP_PROP(broad, "typeArgs", &broad_args) || Z_TYPE(broad_args) != IS_ARRAY) {
		zval_ptr_dtor(&broad_args);
		return TYHP_IS_UNKNOWN;
	}

	if (!narrow_is_union) {
		bool unknown = false;

		ZEND_HASH_FOREACH_VAL(Z_ARRVAL(broad_args), broad_member) {
			zval *elem = tyhp_deref(broad_member);
			tyhp_is_result one;

			if (elem == NULL || Z_TYPE_P(elem) != IS_OBJECT) {
				zval_ptr_dtor(&broad_args);
				return TYHP_IS_UNKNOWN;
			}
			one = tyhp_compatible(Z_OBJ_P(elem), narrow, depth + 1);
			if (EG(exception)) {
				zval_ptr_dtor(&broad_args);
				return TYHP_IS_UNKNOWN;
			}
			if (one == TYHP_IS_TRUE) {
				zval_ptr_dtor(&broad_args);
				return TYHP_IS_TRUE;
			}
			if (one == TYHP_IS_UNKNOWN) {
				unknown = true;
			}
		} ZEND_HASH_FOREACH_END();

		zval_ptr_dtor(&broad_args);
		return unknown ? TYHP_IS_UNKNOWN : TYHP_IS_FALSE;
	}

	if (!TYHP_PROP(narrow, "typeArgs", &narrow_args) || Z_TYPE(narrow_args) != IS_ARRAY) {
		zval_ptr_dtor(&broad_args);
		zval_ptr_dtor(&narrow_args);
		return TYHP_IS_UNKNOWN;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(narrow_args), narrow_member) {
		zval *narrow_elem = tyhp_deref(narrow_member);
		bool found = false;

		if (narrow_elem == NULL || Z_TYPE_P(narrow_elem) != IS_OBJECT) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_UNKNOWN;
		}

		{
			bool member_unknown = false;

			ZEND_HASH_FOREACH_VAL(Z_ARRVAL(broad_args), broad_member) {
				zval *broad_elem = tyhp_deref(broad_member);
				tyhp_is_result one;

				if (broad_elem == NULL || Z_TYPE_P(broad_elem) != IS_OBJECT) {
					zval_ptr_dtor(&broad_args);
					zval_ptr_dtor(&narrow_args);
					return TYHP_IS_UNKNOWN;
				}
				one = tyhp_compatible(Z_OBJ_P(broad_elem), Z_OBJ_P(narrow_elem), depth + 1);
				if (EG(exception)) {
					zval_ptr_dtor(&broad_args);
					zval_ptr_dtor(&narrow_args);
					return TYHP_IS_UNKNOWN;
				}
				if (one == TYHP_IS_TRUE) {
					found = true;
					break;
				}
				if (one == TYHP_IS_UNKNOWN) {
					member_unknown = true;
				}
			} ZEND_HASH_FOREACH_END();

			if (!found && member_unknown) {
				zval_ptr_dtor(&broad_args);
				zval_ptr_dtor(&narrow_args);
				return TYHP_IS_UNKNOWN;
			}
		}

		if (!found) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_FALSE;
		}
	} ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&broad_args);
	zval_ptr_dtor(&narrow_args);
	return TYHP_IS_TRUE;
}

static tyhp_is_result tyhp_intersection_compatible(zend_object *broad, zend_object *narrow, zend_string *narrow_kind, int depth)
{
	zval broad_args, narrow_args;
	zval *broad_member;
	bool narrow_is_intersection;

	ZVAL_UNDEF(&broad_args);
	ZVAL_UNDEF(&narrow_args);
	narrow_is_intersection = narrow_kind != NULL && zend_string_equals_literal(narrow_kind, "intersection");

	if (!TYHP_PROP(broad, "typeArgs", &broad_args) || Z_TYPE(broad_args) != IS_ARRAY) {
		zval_ptr_dtor(&broad_args);
		return TYHP_IS_UNKNOWN;
	}
	if (narrow_is_intersection) {
		if (!TYHP_PROP(narrow, "typeArgs", &narrow_args) || Z_TYPE(narrow_args) != IS_ARRAY) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_UNKNOWN;
		}
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(broad_args), broad_member) {
		zval *broad_elem = tyhp_deref(broad_member);
		bool satisfied = false;

		if (broad_elem == NULL || Z_TYPE_P(broad_elem) != IS_OBJECT) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_UNKNOWN;
		}

		if (!narrow_is_intersection) {
			tyhp_is_result one = tyhp_compatible(Z_OBJ_P(broad_elem), narrow, depth + 1);
			if (one != TYHP_IS_TRUE) {
				zval_ptr_dtor(&broad_args);
				zval_ptr_dtor(&narrow_args);
				return EG(exception) ? TYHP_IS_UNKNOWN : one;
			}
			continue;
		}

		{
			zval *narrow_member;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL(narrow_args), narrow_member) {
				zval *narrow_elem = tyhp_deref(narrow_member);
				tyhp_is_result one;

				if (narrow_elem == NULL || Z_TYPE_P(narrow_elem) != IS_OBJECT) {
					zval_ptr_dtor(&broad_args);
					zval_ptr_dtor(&narrow_args);
					return TYHP_IS_UNKNOWN;
				}
				one = tyhp_compatible(Z_OBJ_P(broad_elem), Z_OBJ_P(narrow_elem), depth + 1);
				if (EG(exception) || one == TYHP_IS_UNKNOWN) {
					zval_ptr_dtor(&broad_args);
					zval_ptr_dtor(&narrow_args);
					return TYHP_IS_UNKNOWN;
				}
				if (one == TYHP_IS_TRUE) {
					satisfied = true;
					break;
				}
			} ZEND_HASH_FOREACH_END();
		}

		if (!satisfied) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_FALSE;
		}
	} ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&broad_args);
	zval_ptr_dtor(&narrow_args);
	return TYHP_IS_TRUE;
}

static tyhp_is_result tyhp_compatible(zend_object *broad_obj, zend_object *narrow_obj, int depth)
{
	zval hold_broad, hold_narrow;
	zval broad_kind, broad_name, narrow_kind, narrow_name;
	zend_object *broad;
	zend_object *narrow;
	tyhp_follow_stack stack;
	tyhp_view broad_view, narrow_view;
	bool same_kind;
	bool same_name;
	tyhp_is_result result = TYHP_IS_FALSE;

	if (EG(exception) || depth > TYHP_MAX_DEPTH) {
		return TYHP_IS_UNKNOWN;
	}

	ZVAL_UNDEF(&hold_broad);
	ZVAL_UNDEF(&hold_narrow);
	if (!tyhp_unwrap_named(broad_obj, &broad, &hold_broad) || !tyhp_unwrap_named(narrow_obj, &narrow, &hold_narrow)) {
		zval_ptr_dtor(&hold_broad);
		zval_ptr_dtor(&hold_narrow);
		return TYHP_IS_UNKNOWN;
	}

	stack.count = 0;
	broad_view = tyhp_follow(broad, &stack, 0);
	stack.count = 0;
	narrow_view = tyhp_follow(narrow, &stack, 0);
	zval_ptr_dtor(&hold_broad);
	zval_ptr_dtor(&hold_narrow);
	if (broad_view.unknown || narrow_view.unknown || broad_view.obj == NULL || narrow_view.obj == NULL || EG(exception)) {
		return TYHP_IS_UNKNOWN;
	}

	ZVAL_UNDEF(&broad_kind);
	ZVAL_UNDEF(&broad_name);
	ZVAL_UNDEF(&narrow_kind);
	ZVAL_UNDEF(&narrow_name);
	if (!tyhp_read_kind_name(broad_view.obj, &broad_kind, &broad_name)
		|| !tyhp_read_kind_name(narrow_view.obj, &narrow_kind, &narrow_name)) {
		result = TYHP_IS_UNKNOWN;
		goto cleanup;
	}

	if (zend_string_equals_literal(Z_STR(broad_kind), "scalar") && Z_TYPE(broad_name) == IS_STRING
		&& zend_string_equals_literal(Z_STR(broad_name), "mixed")) {
		result = TYHP_IS_TRUE;
		goto cleanup;
	}
	if (zend_string_equals_literal(Z_STR(narrow_kind), "scalar") && Z_TYPE(narrow_name) == IS_STRING
		&& zend_string_equals_literal(Z_STR(narrow_name), "never")) {
		result = TYHP_IS_TRUE;
		goto cleanup;
	}

	if (tyhp_view_is_nullable(&narrow_view) && !tyhp_view_is_nullable(&broad_view)) {
		result = TYHP_IS_FALSE;
		goto cleanup;
	}
	if (EG(exception)) {
		result = TYHP_IS_UNKNOWN;
		goto cleanup;
	}

	if (zend_string_equals_literal(Z_STR(broad_kind), "union")) {
		result = tyhp_union_compatible(
			broad_view.obj,
			narrow_view.obj,
			Z_TYPE(narrow_kind) == IS_STRING ? Z_STR(narrow_kind) : NULL,
			depth
		);
		goto cleanup;
	}
	if (zend_string_equals_literal(Z_STR(broad_kind), "intersection")) {
		result = tyhp_intersection_compatible(
			broad_view.obj,
			narrow_view.obj,
			Z_TYPE(narrow_kind) == IS_STRING ? Z_STR(narrow_kind) : NULL,
			depth
		);
		goto cleanup;
	}

	same_kind = zend_string_equals(Z_STR(broad_kind), Z_STR(narrow_kind));
	same_name = tyhp_same_name(&broad_name, &narrow_name);
	if (same_kind && same_name) {
		if (zend_string_equals_literal(Z_STR(broad_kind), "generic")) {
			result = tyhp_generic_args_compatible(broad_view.obj, narrow_view.obj, depth);
		} else {
			result = TYHP_IS_TRUE;
		}
		goto cleanup;
	}

	if (zend_string_equals_literal(Z_STR(broad_kind), "class")
		&& zend_string_equals_literal(Z_STR(narrow_kind), "class")) {
		result = tyhp_named_type_is_a(
			Z_TYPE(narrow_name) == IS_STRING ? Z_STR(narrow_name) : NULL,
			Z_TYPE(broad_name) == IS_STRING ? Z_STR(broad_name) : NULL
		);
		goto cleanup;
	}

	result = TYHP_IS_FALSE;

cleanup:
	if (EG(exception)) {
		result = TYHP_IS_UNKNOWN;
	}
	zval_ptr_dtor(&broad_kind);
	zval_ptr_dtor(&broad_name);
	zval_ptr_dtor(&narrow_kind);
	zval_ptr_dtor(&narrow_name);
	return result;
}

/* Defined below the forward declarations' callers. tyhp_generic_args_compatible
 * is used from tyhp_compatible and calls back into it. */
static tyhp_is_result tyhp_generic_args_compatible(zend_object *broad, zend_object *narrow, int depth)
{
	zval broad_args, narrow_args;
	zend_ulong h;
	zend_string *key;
	zval *broad_member;

	ZVAL_UNDEF(&broad_args);
	ZVAL_UNDEF(&narrow_args);
	if (!TYHP_PROP(broad, "typeArgs", &broad_args) || Z_TYPE(broad_args) != IS_ARRAY
		|| !TYHP_PROP(narrow, "typeArgs", &narrow_args) || Z_TYPE(narrow_args) != IS_ARRAY) {
		zval_ptr_dtor(&broad_args);
		zval_ptr_dtor(&narrow_args);
		return TYHP_IS_UNKNOWN;
	}

	if (zend_hash_num_elements(Z_ARRVAL(broad_args)) != zend_hash_num_elements(Z_ARRVAL(narrow_args))) {
		zval_ptr_dtor(&broad_args);
		zval_ptr_dtor(&narrow_args);
		return TYHP_IS_FALSE;
	}

	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL(broad_args), h, key, broad_member) {
		zval *narrow_member;
		zval *left = tyhp_deref(broad_member);
		zval *right;
		tyhp_is_result one;

		if (key != NULL) {
			narrow_member = zend_hash_find(Z_ARRVAL(narrow_args), key);
		} else {
			narrow_member = zend_hash_index_find(Z_ARRVAL(narrow_args), h);
		}
		if (narrow_member == NULL) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_FALSE;
		}
		right = tyhp_deref(narrow_member);
		if (left == NULL || right == NULL || Z_TYPE_P(left) != IS_OBJECT || Z_TYPE_P(right) != IS_OBJECT) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return TYHP_IS_UNKNOWN;
		}
		one = tyhp_compatible(Z_OBJ_P(left), Z_OBJ_P(right), depth + 1);
		if (EG(exception) || one != TYHP_IS_TRUE) {
			zval_ptr_dtor(&broad_args);
			zval_ptr_dtor(&narrow_args);
			return EG(exception) ? TYHP_IS_UNKNOWN : one;
		}
	} ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&broad_args);
	zval_ptr_dtor(&narrow_args);
	return TYHP_IS_TRUE;
}

static tyhp_is_result tyhp_match(zend_object *type, zend_string *kind, zval *value, int depth)
{
	if (zend_string_equals_literal(kind, "scalar")) {
		return tyhp_is_scalar(type, value);
	}
	if (zend_string_equals_literal(kind, "class")) {
		return tyhp_is_class(type, value);
	}
	if (zend_string_equals_literal(kind, "union")) {
		return tyhp_is_union(type, value, depth);
	}
	if (zend_string_equals_literal(kind, "intersection")) {
		return tyhp_is_intersection(type, value, depth);
	}
	if (zend_string_equals_literal(kind, "generic")) {
		return tyhp_is_generic(type, value, depth);
	}
	if (zend_string_equals_literal(kind, "struct")) {
		return tyhp_is_struct(type, value, depth);
	}
	if (zend_string_equals_literal(kind, "callableShape")) {
		return zend_is_callable(value, 0, NULL) ? TYHP_IS_TRUE : TYHP_IS_FALSE;
	}
	return TYHP_IS_UNKNOWN;
}

static tyhp_is_result tyhp_type_is(zend_object *obj, zval *value, int depth)
{
	tyhp_follow_stack stack;
	tyhp_view view;
	zval underlying;
	zval kind_zv;
	zval held;
	tyhp_is_result result;

	if (EG(exception) || obj == NULL || depth > TYHP_MAX_DEPTH) {
		return TYHP_IS_UNKNOWN;
	}

	if (tyhp_is_named_type_ce(obj->ce)) {
		ZVAL_UNDEF(&underlying);
		if (!TYHP_PROP(obj, "underlyingType", &underlying) || Z_TYPE(underlying) != IS_OBJECT) {
			zval_ptr_dtor(&underlying);
			return TYHP_IS_UNKNOWN;
		}
		result = tyhp_type_is(Z_OBJ(underlying), value, depth + 1);
		zval_ptr_dtor(&underlying);
		return result;
	}

	stack.count = 0;
	view = tyhp_follow(obj, &stack, 0);
	if (view.unknown || view.obj == NULL || EG(exception)) {
		return TYHP_IS_UNKNOWN;
	}

	if (Z_TYPE_P(value) == IS_NULL && tyhp_view_is_nullable(&view)) {
		return EG(exception) ? TYHP_IS_UNKNOWN : TYHP_IS_TRUE;
	}
	if (EG(exception)) {
		return TYHP_IS_UNKNOWN;
	}

	if (!TYHP_PROP(view.obj, "kind", &kind_zv) || Z_TYPE(kind_zv) != IS_STRING) {
		zval_ptr_dtor(&kind_zv);
		return TYHP_IS_UNKNOWN;
	}

	/* Hold the value for the walk. Autoloading, Iterator methods, and a
	 * generic bag's objectType() run user code that could otherwise reassign
	 * or mutate it through a reference while the walk still points into it. */
	ZVAL_COPY_DEREF(&held, value);
	result = tyhp_match(view.obj, Z_STR(kind_zv), &held, depth);
	zval_ptr_dtor(&held);
	zval_ptr_dtor(&kind_zv);
	return result;
}

PHP_METHOD(Ext, typeIs)
{
	zval *type;
	zval *value;
	tyhp_is_result result;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_OBJECT(type)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	ZVAL_DEREF(value);
	result = tyhp_type_is(Z_OBJ_P(type), value, 0);
	if (EG(exception)) {
		return;
	}
	if (result == TYHP_IS_UNKNOWN) {
		RETURN_NULL();
	}
	RETURN_BOOL(result == TYHP_IS_TRUE);
}
