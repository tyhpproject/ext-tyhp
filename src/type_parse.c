#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#include "php_tyhp.h"

#if defined(ZTS) && defined(COMPILE_DL_TYHP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

/*
 * typeParse mirrors Type::parse: tokenizeSpelling, parseUnionSpelling,
 * parseIntersectionSpelling, parsePrefixSpelling, parseNamedSpelling, and
 * parseSpellingTypeArguments. It returns an array graph, or null when this
 * parser does not accept the spelling. It does not throw. The PHP caller
 * falls back to Type::parse, which throws.
 *
 * There is no alias registry in this extension. A name Type::parse would look
 * up in Type::$registry — a bare name, Name<args>, struct Name, object Name,
 * or callable Name — stays kind "unresolved". The later hydrator checks the
 * registry and turns that node into a class, generic, struct, object shape,
 * callable shape, or alias. Parentheses are grouping only and do not appear
 * in the graph. Nesting deeper than 128 returns null.
 */

#define TYHP_PARSE_MAX_DEPTH 128

typedef struct {
	zend_string **items;
	uint32_t count;
	uint32_t cap;
} tyhp_tokens;

typedef struct {
	zend_string **tokens;
	uint32_t count;
	uint32_t pos;
} tyhp_parser;

typedef struct {
	zval *items;
	uint32_t count;
	uint32_t cap;
} tyhp_zlist;

static bool tyhp_parse_union(tyhp_parser *parser, int depth, zval *out);

static void tyhp_tokens_dtor(tyhp_tokens *tokens)
{
	uint32_t i;

	for (i = 0; i < tokens->count; i++) {
		zend_string_release(tokens->items[i]);
	}
	if (tokens->items) {
		efree(tokens->items);
	}
	tokens->items = NULL;
	tokens->count = 0;
	tokens->cap = 0;
}

static void tyhp_tokens_push(tyhp_tokens *tokens, zend_string *token)
{
	if (tokens->count == tokens->cap) {
		uint32_t cap = tokens->cap ? tokens->cap * 2 : 8;

		tokens->items = erealloc(tokens->items, cap * sizeof(zend_string *));
		tokens->cap = cap;
	}
	tokens->items[tokens->count++] = token;
}

static bool tyhp_is_name_byte(unsigned char c)
{
	return (c >= 'a' && c <= 'z')
		|| (c >= 'A' && c <= 'Z')
		|| (c >= '0' && c <= '9')
		|| c == '_'
		|| c == '\\'
		|| c >= 0x80;
}

static bool tyhp_tokenize(zend_string *spelling, tyhp_tokens *out)
{
	const unsigned char *bytes = (const unsigned char *) ZSTR_VAL(spelling);
	size_t len = ZSTR_LEN(spelling);
	size_t i = 0;

	out->items = NULL;
	out->count = 0;
	out->cap = 0;

	while (i < len) {
		unsigned char c = bytes[i];

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			i++;
			continue;
		}
		if (c == '?' || c == '|' || c == '&' || c == '<' || c == '>' || c == ',' || c == '(' || c == ')') {
			char punct = (char) c;

			tyhp_tokens_push(out, zend_string_init(&punct, 1, 0));
			i++;
			continue;
		}
		if (tyhp_is_name_byte(c)) {
			size_t start = i;

			while (i < len && tyhp_is_name_byte(bytes[i])) {
				i++;
			}
			tyhp_tokens_push(out, zend_string_init((const char *) bytes + start, i - start, 0));
			continue;
		}

		tyhp_tokens_dtor(out);
		return false;
	}

	return true;
}

static zend_string *tyhp_peek(const tyhp_parser *parser)
{
	if (parser->pos >= parser->count) {
		return NULL;
	}
	return parser->tokens[parser->pos];
}

static bool tyhp_token_is(const zend_string *token, const char *lit, size_t len)
{
	return token != NULL && zend_string_equals_cstr(token, lit, len);
}

static bool tyhp_peek_is(const tyhp_parser *parser, const char *lit, size_t len)
{
	return tyhp_token_is(tyhp_peek(parser), lit, len);
}

static bool tyhp_expect(tyhp_parser *parser, const char *lit, size_t len)
{
	if (!tyhp_peek_is(parser, lit, len)) {
		return false;
	}
	parser->pos++;
	return true;
}

static bool tyhp_is_punct(const zend_string *token)
{
	return token != NULL
		&& ZSTR_LEN(token) == 1
		&& strchr("?|&<>,()", ZSTR_VAL(token)[0]) != NULL;
}

static zend_string *tyhp_ltrim_backslash(zend_string *token)
{
	size_t i = 0;

	while (i < ZSTR_LEN(token) && ZSTR_VAL(token)[i] == '\\') {
		i++;
	}
	return zend_string_init(ZSTR_VAL(token) + i, ZSTR_LEN(token) - i, 0);
}

static bool tyhp_eq_ci(const zend_string *name, const char *lit, size_t len)
{
	size_t i;

	if (ZSTR_LEN(name) != len) {
		return false;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) ZSTR_VAL(name)[i];

		if (c >= 'A' && c <= 'Z') {
			c = (unsigned char) (c + ('a' - 'A'));
		}
		if (c != (unsigned char) lit[i]) {
			return false;
		}
	}
	return true;
}

static void tyhp_zlist_init(tyhp_zlist *list)
{
	list->items = NULL;
	list->count = 0;
	list->cap = 0;
}

static void tyhp_zlist_push(tyhp_zlist *list, zval *zv)
{
	if (list->count == list->cap) {
		uint32_t cap = list->cap ? list->cap * 2 : 4;

		list->items = erealloc(list->items, cap * sizeof(zval));
		list->cap = cap;
	}
	ZVAL_COPY_VALUE(&list->items[list->count], zv);
	ZVAL_UNDEF(zv);
	list->count++;
}

static void tyhp_zlist_dtor(tyhp_zlist *list)
{
	uint32_t i;

	for (i = 0; i < list->count; i++) {
		zval_ptr_dtor(&list->items[i]);
	}
	if (list->items) {
		efree(list->items);
	}
	list->items = NULL;
	list->count = 0;
	list->cap = 0;
}

static void tyhp_zlist_free_buf(tyhp_zlist *list)
{
	if (list->items) {
		efree(list->items);
	}
	list->items = NULL;
	list->count = 0;
	list->cap = 0;
}

static void tyhp_zlist_to_array(tyhp_zlist *list, zval *out)
{
	uint32_t i;

	array_init_size(out, list->count);
	for (i = 0; i < list->count; i++) {
		add_next_index_zval(out, &list->items[i]);
		ZVAL_UNDEF(&list->items[i]);
	}
	tyhp_zlist_free_buf(list);
}

static zval *tyhp_field(zval *node, const char *name, size_t len)
{
	if (Z_TYPE_P(node) != IS_ARRAY) {
		return NULL;
	}
	return zend_hash_str_find(Z_ARRVAL_P(node), name, len);
}

static bool tyhp_kind_is(zval *node, const char *kind, size_t len)
{
	zval *field = tyhp_field(node, "kind", sizeof("kind") - 1);

	return field != NULL
		&& Z_TYPE_P(field) == IS_STRING
		&& zend_string_equals_cstr(Z_STR_P(field), kind, len);
}

static bool tyhp_is_null_scalar(zval *node)
{
	zval *name;

	if (!tyhp_kind_is(node, "scalar", sizeof("scalar") - 1)) {
		return false;
	}
	name = tyhp_field(node, "name", sizeof("name") - 1);
	return name != NULL
		&& Z_TYPE_P(name) == IS_STRING
		&& zend_string_equals_literal(Z_STR_P(name), "null");
}

static bool tyhp_node_nullable(zval *node)
{
	zval *flag = tyhp_field(node, "nullable", sizeof("nullable") - 1);

	return flag != NULL && Z_TYPE_P(flag) == IS_TRUE;
}

static void tyhp_set_nullable(zval *node, bool nullable)
{
	zval *flag = tyhp_field(node, "nullable", sizeof("nullable") - 1);

	if (flag != NULL) {
		ZVAL_BOOL(flag, nullable);
	}
}

/* Type::asNullable(). mixed and the null scalar do not become nullable. */
static void tyhp_as_nullable(zval *node)
{
	zval *name;

	if (tyhp_node_nullable(node)) {
		return;
	}
	if (tyhp_kind_is(node, "scalar", sizeof("scalar") - 1)) {
		name = tyhp_field(node, "name", sizeof("name") - 1);
		if (name != NULL && Z_TYPE_P(name) == IS_STRING
			&& (zend_string_equals_literal(Z_STR_P(name), "null")
				|| zend_string_equals_literal(Z_STR_P(name), "mixed"))) {
			return;
		}
	}
	tyhp_set_nullable(node, true);
}

static void tyhp_as_non_nullable(zval *node)
{
	tyhp_set_nullable(node, false);
}

static void tyhp_node_scalar(zval *out, const char *name, bool nullable)
{
	array_init_size(out, 3);
	add_assoc_string(out, "kind", "scalar");
	add_assoc_string(out, "name", name);
	add_assoc_bool(out, "nullable", nullable);
}

static void tyhp_node_class(zval *out, const char *name)
{
	array_init_size(out, 3);
	add_assoc_string(out, "kind", "class");
	add_assoc_string(out, "name", name);
	add_assoc_bool(out, "nullable", false);
}

static void tyhp_node_unresolved(zval *out, const char *form, zend_string *name, zval *args)
{
	array_init_size(out, 5);
	add_assoc_string(out, "kind", "unresolved");
	add_assoc_string(out, "form", form);
	add_assoc_str(out, "name", zend_string_copy(name));
	add_assoc_bool(out, "nullable", false);
	add_assoc_zval(out, "args", args);
	ZVAL_UNDEF(args);
}

static void tyhp_node_generic(zval *out, const char *name, zval *args)
{
	array_init_size(out, 4);
	add_assoc_string(out, "kind", "generic");
	add_assoc_string(out, "name", name);
	add_assoc_bool(out, "nullable", false);
	add_assoc_zval(out, "args", args);
	ZVAL_UNDEF(args);
}

static void tyhp_node_combo(zval *out, const char *kind, tyhp_zlist *members, bool nullable)
{
	zval arr;
	uint32_t i;

	array_init_size(&arr, members->count);
	for (i = 0; i < members->count; i++) {
		add_next_index_zval(&arr, &members->items[i]);
		ZVAL_UNDEF(&members->items[i]);
	}
	tyhp_zlist_free_buf(members);

	array_init_size(out, 3);
	add_assoc_string(out, "kind", kind);
	add_assoc_bool(out, "nullable", false);
	add_assoc_zval(out, "members", &arr);
	ZVAL_UNDEF(&arr);
	if (nullable) {
		tyhp_as_nullable(out);
	}
}

/* One level, matching Type::union() / Type::intersection(). */
static void tyhp_flatten(tyhp_zlist *list, const char *kind, size_t kind_len)
{
	tyhp_zlist flat;
	uint32_t i;
	bool nested = false;

	for (i = 0; i < list->count; i++) {
		if (tyhp_kind_is(&list->items[i], kind, kind_len)) {
			nested = true;
			break;
		}
	}
	if (!nested) {
		return;
	}

	tyhp_zlist_init(&flat);
	for (i = 0; i < list->count; i++) {
		if (tyhp_kind_is(&list->items[i], kind, kind_len)) {
			zval *members = tyhp_field(&list->items[i], "members", sizeof("members") - 1);
			zval *member;

			if (members != NULL && Z_TYPE_P(members) == IS_ARRAY) {
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(members), member) {
					zval copy;

					ZVAL_COPY(&copy, member);
					tyhp_zlist_push(&flat, &copy);
				} ZEND_HASH_FOREACH_END();
			}
			zval_ptr_dtor(&list->items[i]);
			ZVAL_UNDEF(&list->items[i]);
		} else {
			tyhp_zlist_push(&flat, &list->items[i]);
		}
	}
	tyhp_zlist_free_buf(list);
	*list = flat;
}

static void tyhp_wrap_arg(zval *pair, const char *pname, zval *type)
{
	array_init_size(pair, 2);
	add_assoc_string(pair, "name", pname);
	add_assoc_zval(pair, "type", type);
	ZVAL_UNDEF(type);
}

/* Form "name" args are NamedType('T'.$index, $arg). Consumes nodes. */
static void tyhp_wrap_indexed_args(zval *nodes, zval *out)
{
	zval *type;
	uint32_t index = 0;

	array_init_size(out, zend_hash_num_elements(Z_ARRVAL_P(nodes)));
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(nodes), type) {
		zval pair;
		zval copy;
		char pname[32];

		snprintf(pname, sizeof(pname), "T%u", index);
		ZVAL_COPY(&copy, type);
		tyhp_wrap_arg(&pair, pname, &copy);
		add_next_index_zval(out, &pair);
		ZVAL_UNDEF(&pair);
		index++;
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(nodes);
	ZVAL_UNDEF(nodes);
}

/* array / iterable parameter names. n is 1 or 2. Consumes nodes. */
static void tyhp_wrap_generic_args(zval *nodes, uint32_t n, zval *out)
{
	zval *type;
	uint32_t index = 0;
	const char *names[2];

	names[0] = (n == 1) ? "TValue" : "TKey";
	names[1] = "TValue";

	array_init_size(out, n);
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(nodes), type) {
		zval pair;
		zval copy;

		if (index >= n) {
			break;
		}
		ZVAL_COPY(&copy, type);
		tyhp_wrap_arg(&pair, names[index], &copy);
		add_next_index_zval(out, &pair);
		ZVAL_UNDEF(&pair);
		index++;
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(nodes);
	ZVAL_UNDEF(nodes);
}

static bool tyhp_parse_args(tyhp_parser *parser, int depth, zval *out)
{
	tyhp_zlist args;

	ZVAL_UNDEF(out);
	if (!tyhp_peek_is(parser, "<", 1)) {
		array_init(out);
		return true;
	}

	parser->pos++;
	tyhp_zlist_init(&args);

	{
		zval arg;

		if (!tyhp_parse_union(parser, depth + 1, &arg)) {
			tyhp_zlist_dtor(&args);
			return false;
		}
		tyhp_zlist_push(&args, &arg);
	}

	while (tyhp_peek_is(parser, ",", 1)) {
		zval arg;

		parser->pos++;
		if (!tyhp_parse_union(parser, depth + 1, &arg)) {
			tyhp_zlist_dtor(&args);
			return false;
		}
		tyhp_zlist_push(&args, &arg);
	}

	if (!tyhp_expect(parser, ">", 1)) {
		tyhp_zlist_dtor(&args);
		return false;
	}

	tyhp_zlist_to_array(&args, out);
	return true;
}

static bool tyhp_parse_named(tyhp_parser *parser, int depth, zval *out)
{
	zend_string *token;
	zend_string *name;
	zend_string *next;
	zval args;
	bool array_or_iterable = false;
	const char *generic_name = NULL;

	ZVAL_UNDEF(out);
	ZVAL_UNDEF(&args);

	token = tyhp_peek(parser);
	if (token == NULL || tyhp_is_punct(token)) {
		return false;
	}
	parser->pos++;
	name = tyhp_ltrim_backslash(token);

	next = tyhp_peek(parser);
	if ((tyhp_eq_ci(name, "struct", 6) || tyhp_eq_ci(name, "object", 6) || tyhp_eq_ci(name, "callable", 8))
		&& next != NULL
		&& !tyhp_is_punct(next)) {
		zend_string *shape_name;
		const char *form;

		parser->pos++;
		shape_name = tyhp_ltrim_backslash(next);
		if (!tyhp_parse_args(parser, depth, &args)) {
			zend_string_release(name);
			zend_string_release(shape_name);
			return false;
		}
		if (tyhp_eq_ci(name, "struct", 6)) {
			form = "struct";
		} else if (tyhp_eq_ci(name, "object", 6)) {
			form = "object";
		} else {
			form = "callable";
		}
		tyhp_node_unresolved(out, form, shape_name, &args);
		zend_string_release(name);
		zend_string_release(shape_name);
		return true;
	}

	if (!tyhp_parse_args(parser, depth, &args)) {
		zend_string_release(name);
		return false;
	}

	if (tyhp_eq_ci(name, "array", 5)) {
		array_or_iterable = true;
		generic_name = "array";
	} else if (tyhp_eq_ci(name, "iterable", 8)) {
		array_or_iterable = true;
		generic_name = "iterable";
	}

	if (array_or_iterable) {
		uint32_t n = zend_hash_num_elements(Z_ARRVAL(args));

		if (n == 0) {
			zval_ptr_dtor(&args);
			zend_string_release(name);
			tyhp_node_scalar(out, generic_name, false);
			return true;
		}
		if (n > 2) {
			zval_ptr_dtor(&args);
			zend_string_release(name);
			return false;
		}
		{
			zval wrapped;

			tyhp_wrap_generic_args(&args, n, &wrapped);
			tyhp_node_generic(out, generic_name, &wrapped);
		}
		zend_string_release(name);
		return true;
	}

	if (tyhp_eq_ci(name, "true", 4) || tyhp_eq_ci(name, "false", 5)) {
		uint32_t n = zend_hash_num_elements(Z_ARRVAL(args));

		zval_ptr_dtor(&args);
		zend_string_release(name);
		if (n != 0) {
			return false;
		}
		tyhp_node_scalar(out, "bool", false);
		return true;
	}

	if (tyhp_eq_ci(name, "decimal", 7)) {
		uint32_t n = zend_hash_num_elements(Z_ARRVAL(args));

		zval_ptr_dtor(&args);
		zend_string_release(name);
		if (n != 0) {
			return false;
		}
		tyhp_node_class(out, "Tyhp\\Decimal");
		return true;
	}

	if (tyhp_eq_ci(name, "string", 6) || tyhp_eq_ci(name, "int", 3) || tyhp_eq_ci(name, "float", 5)
		|| tyhp_eq_ci(name, "bool", 4) || tyhp_eq_ci(name, "null", 4) || tyhp_eq_ci(name, "void", 4)
		|| tyhp_eq_ci(name, "mixed", 5) || tyhp_eq_ci(name, "never", 5) || tyhp_eq_ci(name, "object", 6)
		|| tyhp_eq_ci(name, "callable", 8) || tyhp_eq_ci(name, "resource", 8)) {
		uint32_t n = zend_hash_num_elements(Z_ARRVAL(args));
		const char *scalar_name;

		if (tyhp_eq_ci(name, "string", 6)) {
			scalar_name = "string";
		} else if (tyhp_eq_ci(name, "int", 3)) {
			scalar_name = "int";
		} else if (tyhp_eq_ci(name, "float", 5)) {
			scalar_name = "float";
		} else if (tyhp_eq_ci(name, "bool", 4)) {
			scalar_name = "bool";
		} else if (tyhp_eq_ci(name, "null", 4)) {
			scalar_name = "null";
		} else if (tyhp_eq_ci(name, "void", 4)) {
			scalar_name = "void";
		} else if (tyhp_eq_ci(name, "mixed", 5)) {
			scalar_name = "mixed";
		} else if (tyhp_eq_ci(name, "never", 5)) {
			scalar_name = "never";
		} else if (tyhp_eq_ci(name, "object", 6)) {
			scalar_name = "object";
		} else if (tyhp_eq_ci(name, "callable", 8)) {
			scalar_name = "callable";
		} else {
			scalar_name = "resource";
		}

		zval_ptr_dtor(&args);
		zend_string_release(name);
		if (n != 0) {
			return false;
		}
		tyhp_node_scalar(out, scalar_name, strcmp(scalar_name, "null") == 0);
		return true;
	}

	{
		uint32_t n = zend_hash_num_elements(Z_ARRVAL(args));
		zval wrapped;

		if (n == 0) {
			tyhp_node_unresolved(out, "name", name, &args);
		} else {
			tyhp_wrap_indexed_args(&args, &wrapped);
			tyhp_node_unresolved(out, "name", name, &wrapped);
		}
		zend_string_release(name);
		return true;
	}
}

static bool tyhp_parse_prefix(tyhp_parser *parser, int depth, zval *out)
{
	ZVAL_UNDEF(out);
	if (depth > TYHP_PARSE_MAX_DEPTH) {
		return false;
	}

	if (tyhp_peek_is(parser, "?", 1)) {
		parser->pos++;
		if (!tyhp_parse_prefix(parser, depth + 1, out)) {
			return false;
		}
		tyhp_as_nullable(out);
		return true;
	}

	if (tyhp_peek_is(parser, "(", 1)) {
		parser->pos++;
		if (!tyhp_parse_union(parser, depth + 1, out)) {
			return false;
		}
		if (!tyhp_expect(parser, ")", 1)) {
			zval_ptr_dtor(out);
			ZVAL_UNDEF(out);
			return false;
		}
		return true;
	}

	return tyhp_parse_named(parser, depth, out);
}

static bool tyhp_parse_intersection(tyhp_parser *parser, int depth, zval *out)
{
	tyhp_zlist members;
	zval first;

	ZVAL_UNDEF(out);
	if (!tyhp_parse_prefix(parser, depth, &first)) {
		return false;
	}

	if (!tyhp_peek_is(parser, "&", 1)) {
		ZVAL_COPY_VALUE(out, &first);
		return true;
	}

	tyhp_zlist_init(&members);
	tyhp_zlist_push(&members, &first);
	while (tyhp_peek_is(parser, "&", 1)) {
		zval next;

		parser->pos++;
		if (!tyhp_parse_prefix(parser, depth, &next)) {
			tyhp_zlist_dtor(&members);
			return false;
		}
		tyhp_zlist_push(&members, &next);
	}

	tyhp_flatten(&members, "intersection", sizeof("intersection") - 1);
	tyhp_node_combo(out, "intersection", &members, false);
	return true;
}

static bool tyhp_parse_union(tyhp_parser *parser, int depth, zval *out)
{
	tyhp_zlist members;
	tyhp_zlist non_null;
	zval first;
	bool nullable = false;
	uint32_t i;

	ZVAL_UNDEF(out);
	if (depth > TYHP_PARSE_MAX_DEPTH) {
		return false;
	}
	if (!tyhp_parse_intersection(parser, depth, &first)) {
		return false;
	}

	if (!tyhp_peek_is(parser, "|", 1)) {
		ZVAL_COPY_VALUE(out, &first);
		return true;
	}

	tyhp_zlist_init(&members);
	tyhp_zlist_push(&members, &first);
	while (tyhp_peek_is(parser, "|", 1)) {
		zval next;

		parser->pos++;
		if (!tyhp_parse_intersection(parser, depth, &next)) {
			tyhp_zlist_dtor(&members);
			return false;
		}
		tyhp_zlist_push(&members, &next);
	}

	tyhp_zlist_init(&non_null);
	for (i = 0; i < members.count; i++) {
		if (tyhp_is_null_scalar(&members.items[i])) {
			nullable = true;
			zval_ptr_dtor(&members.items[i]);
			ZVAL_UNDEF(&members.items[i]);
			continue;
		}
		if (tyhp_node_nullable(&members.items[i])) {
			nullable = true;
			tyhp_as_non_nullable(&members.items[i]);
		}
		tyhp_zlist_push(&non_null, &members.items[i]);
	}
	tyhp_zlist_free_buf(&members);

	if (non_null.count == 0) {
		tyhp_zlist_free_buf(&non_null);
		tyhp_node_scalar(out, "null", true);
		return true;
	}
	if (non_null.count == 1) {
		ZVAL_COPY_VALUE(out, &non_null.items[0]);
		tyhp_zlist_free_buf(&non_null);
		if (nullable) {
			tyhp_as_nullable(out);
		}
		return true;
	}

	tyhp_flatten(&non_null, "union", sizeof("union") - 1);
	tyhp_node_combo(out, "union", &non_null, nullable);
	return true;
}

PHP_METHOD(Ext, typeParse)
{
	zend_string *spelling;
	tyhp_tokens tokens;
	tyhp_parser parser;
	zval node;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(spelling)
	ZEND_PARSE_PARAMETERS_END();

	if (!tyhp_tokenize(spelling, &tokens)) {
		RETURN_NULL();
	}
	if (tokens.count == 0) {
		tyhp_tokens_dtor(&tokens);
		RETURN_NULL();
	}

	parser.tokens = tokens.items;
	parser.count = tokens.count;
	parser.pos = 0;
	ZVAL_UNDEF(&node);

	if (!tyhp_parse_union(&parser, 0, &node) || parser.pos != parser.count) {
		if (Z_TYPE(node) != IS_UNDEF) {
			zval_ptr_dtor(&node);
		}
		tyhp_tokens_dtor(&tokens);
		RETURN_NULL();
	}

	tyhp_tokens_dtor(&tokens);
	RETVAL_ZVAL(&node, 0, 0);
	ZVAL_UNDEF(&node);
}
