/*
 * \Tyhp\Ext::closureTree(Closure $fn, string $mode): array
 *
 * Walks a closure op_array into a PHP array. It does not build Expression
 * objects, call the closure, or read its source file. Const and capture
 * zvals are copied into the returned array.
 *
 * Node keys (kind is always present):
 *   param:         name
 *   const:         value
 *   capture:       name, value
 *   prop:          object, name
 *   nullsafe_prop: object, name
 *   index:         object, index
 *   binary:        op, left, right
 *   unary:         op, expr
 *   ternary:       cond, then, else
 *                  Elvis (?:) is a ternary whose then is null.
 *   coalesce:      left, right          (?? only)
 *   cast:          type, expr           type is int|string|float|bool|array|object
 *   instanceof:    expr, class          class is a string, or a node when dynamic
 *   new:           class, args
 *   call:          object, name, args
 *   nullsafe_call: object, name, args
 *   static_call:   class, name, args
 *   array:         elements             list of ['key' => node|null, 'value' => node]
 *   lambda:        params, captures, body
 *
 * Operand order. The compiler stores a > b and a >= b as IS_SMALLER and
 * IS_SMALLER_OR_EQUAL with the operands swapped, so a < b and b > a are the
 * same opcode. When an op_array is linked, the VM also reorders the operands
 * of == != === !== * & | ^ xor so the operand with the higher type comes
 * first, which makes 1 == $x and $x == 1 the same opcode as well. <=> and
 * every other binary operator keep source order.
 * The walker recovers source order from the order the operands were computed
 * in: when both are computed values, the one produced first was written
 * first. When an operand is a literal, a variable, or a capture, the opcodes
 * do not record the order. The computed operand then goes on the left and the
 * literal on the right, with a variable or capture between them.
 * IS_SMALLER(X, Y) is reported as X < Y or as Y > X by the same rule.
 * With operands known to be int, the opcache optimizer can also reorder +
 * and rewrite 2 * $x as $x + $x. The tree reports what the opcodes say.
 * <> is reported as !=. and/or are reported as && / ||.
 * Unary plus and minus stay folded as multiply by 1 and -1.
 * FAST_CONCAT is rejected: it is also how interpolation is lowered, and a
 * user concat of a literal cannot be told apart from that.
 * A closure whose only RETURN is the implicit one added at the end of a body
 * (function () {}, function ($p) {}) is rejected at every depth. An arrow
 * function returns through an explicit RETURN, so fn () => null is accepted.
 * A nested arrow function that captures an outer capture carries the outer
 * value. A capture of an outer parameter has a null value, and its name
 * identifies the parameter.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "zend_closures.h"
#include "zend_compile.h"

#include "php_tyhp.h"

#if defined(ZTS) && defined(COMPILE_DL_TYHP)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#define TYHP_MAX_STEPS 100000
#define TYHP_MAX_NEST 64
#define TYHP_MAX_LAMBDA_DEPTH 32

#define TYHP_MSG_BODY "closureTree cannot read this closure body"
#define TYHP_MSG_USE "closureTree does not accept use bindings; write an arrow function"
#define TYHP_MSG_NO_RETURN "closureTree needs a closure body that returns an expression"

typedef struct _tyhp_walk tyhp_walk;

struct _tyhp_walk {
	const zend_op_array *op_array;
	const zend_op *ops;
	uint32_t count;
	uint32_t ip;
	uint32_t steps;
	uint32_t nest;
	zval *slots;
	/* Order in which each slot was last written. Operands of a reordered
	 * opcode are sorted by this to recover source order. */
	uint32_t *slot_seq;
	uint32_t slot_count;
	uint32_t seq;
	uint32_t last_slot;
	uint8_t *cv_used;
	uint8_t *cv_capture;
	uint32_t cv_count;
	uint32_t *capture_order;
	uint32_t capture_count;
	zend_string **param_names;
	uint32_t param_count;
	/* name => value bound by the enclosing closure. NULL at the root. */
	HashTable *outer;
	zval result;
	bool has_result;
	bool failed;
	bool nullsafe_fetch;
	uint32_t depth;
};

typedef struct _tyhp_branch {
	zval value;
	bool has_value;
	bool exits;
	int32_t qm_slot;
	uint32_t join;
} tyhp_branch;

typedef enum {
	/* The opcode keeps source order. */
	TYHP_ORDER_SOURCE,
	/* The VM may have swapped the operands of a commutative opcode. */
	TYHP_ORDER_COMMUTATIVE,
	/* IS_SMALLER family: X < Y, or Y > X when the operands were swapped. */
	TYHP_ORDER_SMALLER
} tyhp_order;

static void tyhp_fail(tyhp_walk *w, const char *message);
static void tyhp_fail_op(tyhp_walk *w, const zend_op *op);
static zend_result tyhp_parse_one(tyhp_walk *w);
static zend_result tyhp_walk_op_array(const zend_op_array *op_array, HashTable *outer, uint32_t depth, zval *body_out, zval *params_out, zval *captures_out);

static zend_class_entry *tyhp_iae_ce(void)
{
	zend_class_entry *ce = zend_hash_str_find_ptr(EG(class_table), "invalidargumentexception", sizeof("invalidargumentexception") - 1);
	if (!ce) {
		ce = zend_ce_exception;
	}
	return ce;
}

static void tyhp_fail(tyhp_walk *w, const char *message)
{
	w->failed = 1;
	if (!EG(exception)) {
		zend_throw_exception(tyhp_iae_ce(), message, 0);
	}
}

static void tyhp_fail_op(tyhp_walk *w, const zend_op *op)
{
	const char *name = zend_get_opcode_name(op->opcode);
	w->failed = 1;
	if (!EG(exception)) {
		zend_throw_exception_ex(tyhp_iae_ce(), 0, "closureTree cannot read opcode %s", name ? name : "unknown");
	}
}

static uint32_t tyhp_slot_of(const zend_op *op, uint32_t var)
{
	(void) op;
	return EX_VAR_TO_NUM(var);
}

static uint32_t tyhp_target(tyhp_walk *w, const zend_op *op, znode_op node)
{
	const zend_op *target = OP_JMP_ADDR(op, node);
	if (target < w->ops || target > w->ops + w->count) {
		return UINT32_MAX;
	}
	return (uint32_t) (target - w->ops);
}

static bool tyhp_is_kind(zval *node, const char *kind)
{
	zval *zv;

	if (!node || Z_TYPE_P(node) != IS_ARRAY) {
		return 0;
	}
	zv = zend_hash_str_find(Z_ARRVAL_P(node), "kind", sizeof("kind") - 1);
	if (!zv || Z_TYPE_P(zv) != IS_STRING) {
		return 0;
	}
	return zend_string_equals_cstr(Z_STR_P(zv), kind, strlen(kind));
}

/* Copies src into dst as an owned value. A reference is dereferenced, so dst
 * never aliases the closure's static variable or the caller's variable. */
static zend_result tyhp_copy_value(zval *dst, zval *src)
{
	int guard = 0;

	while (src && guard++ < 4) {
		if (Z_TYPE_P(src) == IS_REFERENCE) {
			src = Z_REFVAL_P(src);
			continue;
		}
		if (Z_TYPE_P(src) == IS_INDIRECT) {
			src = Z_INDIRECT_P(src);
			continue;
		}
		break;
	}
	if (!src || Z_TYPE_P(src) == IS_UNDEF) {
		ZVAL_NULL(dst);
		return SUCCESS;
	}
	if (Z_TYPE_P(src) == IS_CONSTANT_AST || Z_TYPE_P(src) == IS_PTR) {
		ZVAL_UNDEF(dst);
		return FAILURE;
	}
	ZVAL_COPY(dst, src);
	return SUCCESS;
}

static zend_result tyhp_node_const(tyhp_walk *w, zval *dst, zval *value)
{
	zval copied;

	ZVAL_UNDEF(dst);
	if (tyhp_copy_value(&copied, value) != SUCCESS) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	array_init(dst);
	add_assoc_string(dst, "kind", "const");
	add_assoc_zval(dst, "value", &copied);
	return SUCCESS;
}

/* Takes ownership of node. */
static void tyhp_store(tyhp_walk *w, uint32_t slot, zval *node)
{
	if (w->failed) {
		zval_ptr_dtor(node);
		return;
	}
	if (slot >= w->slot_count) {
		zval_ptr_dtor(node);
		tyhp_fail(w, TYHP_MSG_BODY);
		return;
	}
	zval_ptr_dtor(&w->slots[slot]);
	ZVAL_COPY_VALUE(&w->slots[slot], node);
	w->slot_seq[slot] = ++w->seq;
	w->last_slot = slot;
}

static zend_result tyhp_read(tyhp_walk *w, const zend_op *op, znode_op operand, uint8_t type, zval *out)
{
	uint32_t slot;

	ZVAL_UNDEF(out);
	if (w->failed) {
		return FAILURE;
	}
	if (type == IS_UNUSED) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	if (type == IS_CONST) {
		return tyhp_node_const(w, out, RT_CONSTANT(op, operand));
	}
	if (type != IS_CV && type != IS_TMP_VAR && type != IS_VAR) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	slot = tyhp_slot_of(op, operand.var);
	if (slot >= w->slot_count || Z_TYPE(w->slots[slot]) == IS_UNDEF) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	if (type == IS_CV) {
		if (slot >= w->cv_count) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
		w->cv_used[slot] = 1;
	}
	ZVAL_COPY(out, &w->slots[slot]);
	return SUCCESS;
}

/* Reads the value a RETURN hands back. The RETURN the compiler appends to
 * every body (extended_value -1) is not an expression the closure wrote. */
static zend_result tyhp_read_return(tyhp_walk *w, const zend_op *op, zval *out)
{
	ZVAL_UNDEF(out);
	if (op->extended_value == (uint32_t) -1) {
		tyhp_fail(w, TYHP_MSG_NO_RETURN);
		return FAILURE;
	}
	return tyhp_read(w, op, op->op1, op->op1_type, out);
}

static zend_result tyhp_result_slot(tyhp_walk *w, const zend_op *op, uint32_t *slot)
{
	if (op->result_type != IS_TMP_VAR && op->result_type != IS_VAR) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	*slot = tyhp_slot_of(op, op->result.var);
	if (*slot >= w->slot_count) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	return SUCCESS;
}

static zend_result tyhp_const_string(tyhp_walk *w, const zend_op *op, znode_op operand, uint8_t type, zend_string **out)
{
	zval *value;

	*out = NULL;
	if (type != IS_CONST) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	value = RT_CONSTANT(op, operand);
	if (Z_TYPE_P(value) != IS_STRING) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	*out = Z_STR_P(value);
	return SUCCESS;
}

/* Takes ownership of left and right. */
static void tyhp_node_binary(zval *dst, const char *op, zval *left, zval *right)
{
	array_init(dst);
	add_assoc_string(dst, "kind", "binary");
	add_assoc_string(dst, "op", op);
	add_assoc_zval(dst, "left", left);
	add_assoc_zval(dst, "right", right);
}

static bool tyhp_is_computed(uint8_t type)
{
	return type == IS_TMP_VAR || type == IS_VAR;
}

/* Computed values sort before variables, and variables before literals. */
static int tyhp_rank(uint8_t type)
{
	if (tyhp_is_computed(type)) {
		return 0;
	}
	return type == IS_CV ? 1 : 2;
}

/* True when op2 was the left operand in the source. Only meaningful for an
 * opcode whose operand order the compiler or the VM may have changed. */
static bool tyhp_op2_was_left(tyhp_walk *w, const zend_op *op)
{
	if (tyhp_is_computed(op->op1_type) && tyhp_is_computed(op->op2_type)) {
		uint32_t first = tyhp_slot_of(op, op->op1.var);
		uint32_t second = tyhp_slot_of(op, op->op2.var);

		if (first >= w->slot_count || second >= w->slot_count) {
			return 0;
		}
		return w->slot_seq[second] < w->slot_seq[first];
	}
	return tyhp_rank(op->op2_type) < tyhp_rank(op->op1_type);
}

static zend_result tyhp_binary(tyhp_walk *w, const zend_op *op, tyhp_order order, const char *symbol, const char *swapped_symbol)
{
	zval first, second, node;
	uint32_t slot;
	bool swap = 0;

	ZVAL_UNDEF(&first);
	ZVAL_UNDEF(&second);
	ZVAL_UNDEF(&node);
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &first) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op2, op->op2_type, &second) != SUCCESS) {
		zval_ptr_dtor(&first);
		return FAILURE;
	}
	if (order != TYHP_ORDER_SOURCE) {
		swap = tyhp_op2_was_left(w, op);
	}
	if (swap) {
		tyhp_node_binary(&node, swapped_symbol, &second, &first);
	} else {
		tyhp_node_binary(&node, symbol, &first, &second);
	}
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static const char *tyhp_cast_type(uint32_t type)
{
	switch (type) {
		case IS_LONG:
			return "int";
		case IS_DOUBLE:
			return "float";
		case IS_STRING:
			return "string";
		case IS_ARRAY:
			return "array";
		case IS_OBJECT:
			return "object";
		default:
			return NULL;
	}
}

static zend_result tyhp_unary(tyhp_walk *w, const zend_op *op, const char *symbol)
{
	zval expr, node;
	uint32_t slot;

	ZVAL_UNDEF(&expr);
	ZVAL_UNDEF(&node);
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &expr) != SUCCESS) {
		return FAILURE;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "unary");
	add_assoc_string(&node, "op", symbol);
	add_assoc_zval(&node, "expr", &expr);
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_cast(tyhp_walk *w, const zend_op *op, const char *type_name)
{
	zval expr, node;
	uint32_t slot;

	ZVAL_UNDEF(&expr);
	ZVAL_UNDEF(&node);
	if (!type_name) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &expr) != SUCCESS) {
		return FAILURE;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "cast");
	add_assoc_string(&node, "type", type_name);
	add_assoc_zval(&node, "expr", &expr);
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_fetch_prop(tyhp_walk *w, const zend_op *op)
{
	zval object, node;
	zend_string *name;
	uint32_t slot;
	bool nullsafe;

	ZVAL_UNDEF(&object);
	ZVAL_UNDEF(&node);
	if (op->op2_type != IS_CONST) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_const_string(w, op, op->op2, op->op2_type, &name) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &object) != SUCCESS) {
		return FAILURE;
	}
	nullsafe = w->nullsafe_fetch;
	w->nullsafe_fetch = 0;
	array_init(&node);
	add_assoc_string(&node, "kind", nullsafe ? "nullsafe_prop" : "prop");
	add_assoc_zval(&node, "object", &object);
	add_assoc_str(&node, "name", zend_string_copy(name));
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_fetch_dim(tyhp_walk *w, const zend_op *op)
{
	zval object, index, node;
	uint32_t slot;

	ZVAL_UNDEF(&object);
	ZVAL_UNDEF(&index);
	ZVAL_UNDEF(&node);
	if (op->op2_type == IS_UNUSED) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &object) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op2, op->op2_type, &index) != SUCCESS) {
		zval_ptr_dtor(&object);
		return FAILURE;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "index");
	add_assoc_zval(&node, "object", &object);
	add_assoc_zval(&node, "index", &index);
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

/* The compiler lowers === and !== against true, false, and null to
 * TYPE_CHECK. === keeps the bit of that one type. !== keeps every other bit. */
static zend_result tyhp_type_check(tyhp_walk *w, const zend_op *op)
{
	zval flag, right, left, node;
	uint32_t slot;
	uint32_t mask = op->extended_value;
	bool negated = 0;

	ZVAL_UNDEF(&right);
	ZVAL_UNDEF(&left);
	ZVAL_UNDEF(&node);
	if (mask != MAY_BE_TRUE && mask != MAY_BE_FALSE && mask != MAY_BE_NULL) {
		mask = MAY_BE_ANY ^ mask;
		negated = 1;
	}
	if (mask == MAY_BE_TRUE) {
		ZVAL_TRUE(&flag);
	} else if (mask == MAY_BE_FALSE) {
		ZVAL_FALSE(&flag);
	} else if (mask == MAY_BE_NULL) {
		ZVAL_NULL(&flag);
	} else {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_node_const(w, &right, &flag) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &left) != SUCCESS) {
		zval_ptr_dtor(&right);
		return FAILURE;
	}
	tyhp_node_binary(&node, negated ? "!==" : "===", &left, &right);
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_class_const(tyhp_walk *w, const zend_op *op)
{
	zval *class_name, *lc_name, *const_name, *zv;
	zend_class_entry *ce;
	zend_class_constant *constant;
	zval node;
	uint32_t slot;

	ZVAL_UNDEF(&node);
	if (op->op1_type != IS_CONST || op->op2_type != IS_CONST) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	class_name = RT_CONSTANT(op, op->op1);
	lc_name = class_name + 1;
	const_name = RT_CONSTANT(op, op->op2);
	if (Z_TYPE_P(class_name) != IS_STRING || Z_TYPE_P(lc_name) != IS_STRING || Z_TYPE_P(const_name) != IS_STRING) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	ce = zend_hash_find_ptr(EG(class_table), Z_STR_P(lc_name));
	if (!ce) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	zv = zend_hash_find(&ce->constants_table, Z_STR_P(const_name));
	if (!zv || Z_TYPE_P(zv) != IS_PTR) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	constant = Z_PTR_P(zv);
	if (Z_TYPE(constant->value) == IS_CONSTANT_AST || Z_TYPE(constant->value) == IS_UNDEF) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_node_const(w, &node, &constant->value) != SUCCESS) {
		return FAILURE;
	}
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_instanceof(tyhp_walk *w, const zend_op *op)
{
	zval expr, class_node, node;
	zend_string *class_name = NULL;
	uint32_t slot;
	bool dynamic = 0;

	ZVAL_UNDEF(&expr);
	ZVAL_UNDEF(&class_node);
	ZVAL_UNDEF(&node);
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &expr) != SUCCESS) {
		return FAILURE;
	}
	if (op->op2_type == IS_CONST) {
		if (tyhp_const_string(w, op, op->op2, op->op2_type, &class_name) != SUCCESS) {
			zval_ptr_dtor(&expr);
			return FAILURE;
		}
	} else if (op->op2_type == IS_TMP_VAR || op->op2_type == IS_VAR) {
		if (tyhp_read(w, op, op->op2, op->op2_type, &class_node) != SUCCESS) {
			zval_ptr_dtor(&expr);
			return FAILURE;
		}
		dynamic = 1;
	} else {
		zval_ptr_dtor(&expr);
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "instanceof");
	add_assoc_zval(&node, "expr", &expr);
	if (dynamic) {
		add_assoc_zval(&node, "class", &class_node);
	} else {
		add_assoc_str(&node, "class", zend_string_copy(class_name));
	}
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_fetch_class(tyhp_walk *w, const zend_op *op)
{
	zval value;
	uint32_t slot;

	ZVAL_UNDEF(&value);
	/* A class name literal is the INSTANCEOF operand itself. FETCH_CLASS of a
	 * CV is $u instanceof $name, which is not on the whitelist. A TMP/VAR is
	 * the expression compiled for a dynamic class (FETCH_OBJ_R then FETCH_CLASS). */
	if (op->op1_type != IS_TMP_VAR && op->op1_type != IS_VAR) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &value) != SUCCESS) {
		return FAILURE;
	}
	tyhp_store(w, slot, &value);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static HashTable *tyhp_live_statics(const zend_op_array *op_array)
{
	HashTable *ht;

	if (!op_array->static_variables) {
		return NULL;
	}
	ht = ZEND_MAP_PTR_GET(op_array->static_variables_ptr);
	if (!ht) {
		ht = op_array->static_variables;
	}
	return ht;
}

/* The value a closure was created with, as an owned copy. The root closure
 * reads its own static variables. A nested closure reads what the enclosing
 * closure's BIND_LEXICAL would hand it, which declare_lambda collected. */
static zend_result tyhp_capture_value(tyhp_walk *w, zend_string *name, zval *dst)
{
	zval *value;

	ZVAL_UNDEF(dst);
	if (!name) {
		return FAILURE;
	}
	if (w->outer) {
		value = zend_hash_find(w->outer, name);
	} else {
		HashTable *ht = tyhp_live_statics(w->op_array);
		value = ht ? zend_hash_find(ht, name) : NULL;
	}
	if (!value) {
		return FAILURE;
	}
	return tyhp_copy_value(dst, value);
}

static zend_result tyhp_bind_static(tyhp_walk *w, const zend_op *op)
{
	uint32_t cv;
	zend_string *name;
	zval value, node;

	ZVAL_UNDEF(&value);
	ZVAL_UNDEF(&node);
	if ((op->extended_value & ZEND_BIND_IMPLICIT) == 0 || (op->extended_value & (ZEND_BIND_EXPLICIT | ZEND_BIND_REF)) != 0) {
		tyhp_fail(w, TYHP_MSG_USE);
		return FAILURE;
	}
	if (op->op1_type != IS_CV) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	cv = tyhp_slot_of(op, op->op1.var);
	if (cv >= w->cv_count || !w->op_array->vars) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	name = w->op_array->vars[cv];
	if (tyhp_capture_value(w, name, &value) != SUCCESS) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "capture");
	add_assoc_str(&node, "name", zend_string_copy(name));
	add_assoc_zval(&node, "value", &value);
	tyhp_store(w, cv, &node);
	if (w->capture_count < w->cv_count) {
		w->capture_order[w->capture_count++] = cv;
	}
	w->cv_capture[cv] = 1;
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_recv(tyhp_walk *w, const zend_op *op)
{
	uint32_t cv;
	zend_string *name;
	zval node;

	ZVAL_UNDEF(&node);
	if (op->opcode != ZEND_RECV) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (op->result_type != IS_CV) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	cv = tyhp_slot_of(op, op->result.var);
	if (cv >= w->cv_count || !w->op_array->vars || w->param_count >= w->cv_count) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	name = w->op_array->vars[cv];
	array_init(&node);
	add_assoc_string(&node, "kind", "param");
	add_assoc_str(&node, "name", zend_string_copy(name));
	tyhp_store(w, cv, &node);
	w->param_names[w->param_count++] = name;
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_qm_assign(tyhp_walk *w, const zend_op *op)
{
	zval value;
	uint32_t slot;

	ZVAL_UNDEF(&value);
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &value) != SUCCESS) {
		return FAILURE;
	}
	tyhp_store(w, slot, &value);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

/* Appends one element to the array node. Takes ownership of key and value. */
static zend_result tyhp_array_element(tyhp_walk *w, zval *array_node, zval *key, zval *value)
{
	zval *elements, element;

	if (!tyhp_is_kind(array_node, "array")) {
		if (key) {
			zval_ptr_dtor(key);
		}
		zval_ptr_dtor(value);
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	SEPARATE_ARRAY(array_node);
	elements = zend_hash_str_find(Z_ARRVAL_P(array_node), "elements", sizeof("elements") - 1);
	if (!elements || Z_TYPE_P(elements) != IS_ARRAY) {
		if (key) {
			zval_ptr_dtor(key);
		}
		zval_ptr_dtor(value);
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	SEPARATE_ARRAY(elements);
	array_init(&element);
	if (key) {
		add_assoc_zval(&element, "key", key);
	} else {
		add_assoc_null(&element, "key");
	}
	add_assoc_zval(&element, "value", value);
	if (zend_hash_next_index_insert(Z_ARRVAL_P(elements), &element) == NULL) {
		zval_ptr_dtor(&element);
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	return SUCCESS;
}

static zend_result tyhp_init_array(tyhp_walk *w, const zend_op *op)
{
	zval node, elements, value, key;
	uint32_t slot;
	bool have_value;

	ZVAL_UNDEF(&node);
	ZVAL_UNDEF(&value);
	ZVAL_UNDEF(&key);
	if (op->extended_value & ZEND_ARRAY_ELEMENT_REF) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	have_value = op->op1_type != IS_UNUSED;
	if (have_value && tyhp_read(w, op, op->op1, op->op1_type, &value) != SUCCESS) {
		return FAILURE;
	}
	if (have_value && op->op2_type != IS_UNUSED && tyhp_read(w, op, op->op2, op->op2_type, &key) != SUCCESS) {
		zval_ptr_dtor(&value);
		return FAILURE;
	}
	array_init(&elements);
	array_init(&node);
	add_assoc_string(&node, "kind", "array");
	add_assoc_zval(&node, "elements", &elements);
	if (have_value) {
		zval *key_ptr = op->op2_type == IS_UNUSED ? NULL : &key;

		if (tyhp_array_element(w, &node, key_ptr, &value) != SUCCESS) {
			zval_ptr_dtor(&node);
			return FAILURE;
		}
	}
	tyhp_store(w, slot, &node);
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_add_array_element(tyhp_walk *w, const zend_op *op)
{
	zval value, key;
	uint32_t slot;

	ZVAL_UNDEF(&value);
	ZVAL_UNDEF(&key);
	if (op->extended_value & ZEND_ARRAY_ELEMENT_REF) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &value) != SUCCESS) {
		return FAILURE;
	}
	if (op->op2_type != IS_UNUSED && tyhp_read(w, op, op->op2, op->op2_type, &key) != SUCCESS) {
		zval_ptr_dtor(&value);
		return FAILURE;
	}
	if (tyhp_array_element(w, &w->slots[slot], op->op2_type == IS_UNUSED ? NULL : &key, &value) != SUCCESS) {
		return FAILURE;
	}
	w->ip++;
	return w->failed ? FAILURE : SUCCESS;
}

static bool tyhp_is_send(uint8_t opcode)
{
	/* SEND_VAR_NO_REF(_EX) carries the result of a call that is itself an
	 * argument: $u->m($u->n()). */
	return opcode == ZEND_SEND_VAL
		|| opcode == ZEND_SEND_VAL_EX
		|| opcode == ZEND_SEND_VAR
		|| opcode == ZEND_SEND_VAR_EX
		|| opcode == ZEND_SEND_VAR_NO_REF
		|| opcode == ZEND_SEND_VAR_NO_REF_EX
		|| opcode == ZEND_SEND_FUNC_ARG;
}

static bool tyhp_is_call_end(uint8_t opcode)
{
	return opcode == ZEND_DO_FCALL
		|| opcode == ZEND_DO_ICALL
		|| opcode == ZEND_DO_UCALL
		|| opcode == ZEND_DO_FCALL_BY_NAME;
}

static zend_result tyhp_parse_call(tyhp_walk *w, const char *kind)
{
	const zend_op *op;
	zval object, args, node;
	zend_string *method_name = NULL;
	zend_string *class_name = NULL;
	uint32_t new_slot = UINT32_MAX;
	bool have_object = 0;
	uint32_t guard;

	ZVAL_UNDEF(&object);
	ZVAL_UNDEF(&args);
	ZVAL_UNDEF(&node);
	if (w->nest >= TYHP_MAX_NEST) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	w->nest++;
	op = &w->ops[w->ip];
	if (op->opcode == ZEND_NEW) {
		if (tyhp_const_string(w, op, op->op1, op->op1_type, &class_name) != SUCCESS) {
			goto fail;
		}
		if (op->result_type != IS_VAR && op->result_type != IS_TMP_VAR) {
			tyhp_fail_op(w, op);
			goto fail;
		}
		new_slot = tyhp_slot_of(op, op->result.var);
	} else if (op->opcode == ZEND_INIT_METHOD_CALL) {
		if (tyhp_read(w, op, op->op1, op->op1_type, &object) != SUCCESS) {
			goto fail;
		}
		have_object = 1;
		if (tyhp_const_string(w, op, op->op2, op->op2_type, &method_name) != SUCCESS) {
			goto fail;
		}
	} else if (op->opcode == ZEND_INIT_STATIC_METHOD_CALL) {
		if (tyhp_const_string(w, op, op->op1, op->op1_type, &class_name) != SUCCESS) {
			goto fail;
		}
		if (tyhp_const_string(w, op, op->op2, op->op2_type, &method_name) != SUCCESS) {
			goto fail;
		}
	} else {
		tyhp_fail_op(w, op);
		goto fail;
	}
	w->ip++;
	array_init(&args);
	guard = w->count + 1;
	while (w->ip < w->count && !w->failed) {
		const zend_op *cur;
		uint32_t before;

		if (guard-- == 0) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
		cur = &w->ops[w->ip];
		if (tyhp_is_call_end(cur->opcode)) {
			uint32_t dest;

			array_init(&node);
			add_assoc_string(&node, "kind", kind);
			if (have_object) {
				add_assoc_zval(&node, "object", &object);
				have_object = 0;
				add_assoc_str(&node, "name", zend_string_copy(method_name));
			} else if (method_name) {
				add_assoc_str(&node, "class", zend_string_copy(class_name));
				add_assoc_str(&node, "name", zend_string_copy(method_name));
			} else {
				add_assoc_str(&node, "class", zend_string_copy(class_name));
			}
			add_assoc_zval(&node, "args", &args);
			ZVAL_UNDEF(&args);
			if (new_slot != UINT32_MAX) {
				dest = new_slot;
			} else if (cur->result_type == IS_VAR || cur->result_type == IS_TMP_VAR) {
				dest = tyhp_slot_of(cur, cur->result.var);
			} else {
				tyhp_fail_op(w, cur);
				goto fail;
			}
			w->ip++;
			tyhp_store(w, dest, &node);
			w->nest--;
			return w->failed ? FAILURE : SUCCESS;
		}
		if (cur->opcode == ZEND_CHECK_FUNC_ARG) {
			if (cur->op2_type == IS_CONST) {
				tyhp_fail_op(w, cur);
				goto fail;
			}
			w->ip++;
			continue;
		}
		if (tyhp_is_send(cur->opcode)) {
			zval arg;

			if (cur->op2_type == IS_CONST) {
				tyhp_fail_op(w, cur);
				goto fail;
			}
			if (tyhp_read(w, cur, cur->op1, cur->op1_type, &arg) != SUCCESS) {
				goto fail;
			}
			add_next_index_zval(&args, &arg);
			w->ip++;
			continue;
		}
		if (cur->opcode == ZEND_CHECK_UNDEF_ARGS || cur->opcode == ZEND_SEND_UNPACK || cur->opcode == ZEND_SEND_REF || cur->opcode == ZEND_CALLABLE_CONVERT) {
			tyhp_fail_op(w, cur);
			goto fail;
		}
		before = w->ip;
		if (tyhp_parse_one(w) != SUCCESS) {
			goto fail;
		}
		if (w->ip == before) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
	}
	tyhp_fail(w, TYHP_MSG_BODY);
fail:
	if (have_object) {
		zval_ptr_dtor(&object);
	}
	zval_ptr_dtor(&args);
	zval_ptr_dtor(&node);
	if (w->nest > 0) {
		w->nest--;
	}
	if (!w->failed) {
		tyhp_fail(w, TYHP_MSG_BODY);
	}
	return FAILURE;
}

static zend_result tyhp_parse_branch(tyhp_walk *w, uint32_t limit, tyhp_branch *out);

static zend_result tyhp_parse_ternary(tyhp_walk *w, zval *out, bool *exits)
{
	const zend_op *op;
	zval cond, node;
	tyhp_branch then_branch, else_branch;
	uint32_t else_ip, else_limit;
	int32_t slot;

	ZVAL_UNDEF(out);
	ZVAL_UNDEF(&cond);
	ZVAL_UNDEF(&node);
	*exits = 0;
	memset(&then_branch, 0, sizeof(then_branch));
	memset(&else_branch, 0, sizeof(else_branch));
	ZVAL_UNDEF(&then_branch.value);
	ZVAL_UNDEF(&else_branch.value);
	then_branch.qm_slot = -1;
	else_branch.qm_slot = -1;
	then_branch.join = UINT32_MAX;
	else_branch.join = UINT32_MAX;
	if (w->nest >= TYHP_MAX_NEST) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	w->nest++;
	op = &w->ops[w->ip];
	else_ip = tyhp_target(w, op, op->op2);
	if (else_ip == UINT32_MAX || else_ip > w->count) {
		tyhp_fail_op(w, op);
		goto fail;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &cond) != SUCCESS) {
		goto fail;
	}
	w->ip++;
	if (tyhp_parse_branch(w, else_ip, &then_branch) != SUCCESS) {
		goto fail;
	}
	if (!then_branch.exits && w->ip != else_ip) {
		tyhp_fail(w, TYHP_MSG_BODY);
		goto fail;
	}
	if (then_branch.exits) {
		w->ip = else_ip;
		else_limit = w->count;
	} else {
		if (then_branch.join == UINT32_MAX || then_branch.join > w->count) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
		else_limit = then_branch.join;
	}
	if (tyhp_parse_branch(w, else_limit, &else_branch) != SUCCESS) {
		goto fail;
	}
	if (!then_branch.has_value || !else_branch.has_value || then_branch.exits != else_branch.exits) {
		tyhp_fail(w, TYHP_MSG_BODY);
		goto fail;
	}
	if (!else_branch.exits && w->ip != else_limit) {
		tyhp_fail(w, TYHP_MSG_BODY);
		goto fail;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "ternary");
	add_assoc_zval(&node, "cond", &cond);
	ZVAL_UNDEF(&cond);
	add_assoc_zval(&node, "then", &then_branch.value);
	then_branch.has_value = 0;
	add_assoc_zval(&node, "else", &else_branch.value);
	else_branch.has_value = 0;
	if (then_branch.exits && else_branch.exits) {
		ZVAL_COPY_VALUE(out, &node);
		*exits = 1;
		w->nest--;
		return SUCCESS;
	}
	/* Both arms must write the one slot the join reads. */
	slot = then_branch.qm_slot;
	if (slot < 0 || slot != else_branch.qm_slot) {
		tyhp_fail(w, TYHP_MSG_BODY);
		goto fail;
	}
	tyhp_store(w, (uint32_t) slot, &node);
	w->nest--;
	return w->failed ? FAILURE : SUCCESS;
fail:
	zval_ptr_dtor(&cond);
	zval_ptr_dtor(&node);
	if (then_branch.has_value) {
		zval_ptr_dtor(&then_branch.value);
	}
	if (else_branch.has_value) {
		zval_ptr_dtor(&else_branch.value);
	}
	if (w->nest > 0) {
		w->nest--;
	}
	if (!w->failed) {
		tyhp_fail(w, TYHP_MSG_BODY);
	}
	return FAILURE;
}

/* After the optimizer an arm no longer ends with QM_ASSIGN: the arm's last
 * computation writes the slot the join reads. */
static bool tyhp_branch_adopt_last(tyhp_walk *w, uint32_t start_seq, tyhp_branch *out)
{
	if (out->has_value || w->seq == start_seq || w->last_slot >= w->slot_count) {
		return 0;
	}
	if (Z_TYPE(w->slots[w->last_slot]) == IS_UNDEF) {
		return 0;
	}
	ZVAL_COPY(&out->value, &w->slots[w->last_slot]);
	out->has_value = 1;
	out->qm_slot = (int32_t) w->last_slot;
	return 1;
}

static zend_result tyhp_parse_branch(tyhp_walk *w, uint32_t limit, tyhp_branch *out)
{
	uint32_t guard = w->count + 1;
	uint32_t start_seq = w->seq;

	memset(out, 0, sizeof(*out));
	ZVAL_UNDEF(&out->value);
	out->qm_slot = -1;
	out->join = UINT32_MAX;
	while (w->ip < limit && w->ip < w->count && !w->failed) {
		const zend_op *op;
		uint32_t before;

		if (guard-- == 0) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
		op = &w->ops[w->ip];
		if (op->opcode == ZEND_RETURN) {
			if (tyhp_read_return(w, op, &out->value) != SUCCESS) {
				return FAILURE;
			}
			out->has_value = 1;
			out->exits = 1;
			w->ip++;
			return SUCCESS;
		}
		if (op->opcode == ZEND_QM_ASSIGN) {
			uint32_t slot;
			zval copy;

			if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
				return FAILURE;
			}
			if (tyhp_read(w, op, op->op1, op->op1_type, &out->value) != SUCCESS) {
				return FAILURE;
			}
			ZVAL_COPY(&copy, &out->value);
			tyhp_store(w, slot, &copy);
			out->has_value = 1;
			out->qm_slot = (int32_t) slot;
			w->ip++;
			if (w->ip < w->count && w->ops[w->ip].opcode == ZEND_JMP) {
				out->join = tyhp_target(w, &w->ops[w->ip], w->ops[w->ip].op1);
				w->ip++;
			}
			return w->failed ? FAILURE : SUCCESS;
		}
		if (op->opcode == ZEND_JMP) {
			out->join = tyhp_target(w, op, op->op1);
			w->ip++;
			if (!out->has_value && !tyhp_branch_adopt_last(w, start_seq, out)) {
				tyhp_fail(w, TYHP_MSG_BODY);
				return FAILURE;
			}
			return SUCCESS;
		}
		if (op->opcode == ZEND_JMPZ) {
			zval nested;
			bool exits = 0;

			if (tyhp_parse_ternary(w, &nested, &exits) != SUCCESS) {
				return FAILURE;
			}
			if (exits) {
				ZVAL_COPY_VALUE(&out->value, &nested);
				out->has_value = 1;
				out->exits = 1;
				return SUCCESS;
			}
			if (w->ip > limit) {
				tyhp_fail(w, TYHP_MSG_BODY);
				return FAILURE;
			}
			continue;
		}
		before = w->ip;
		if (tyhp_parse_one(w) != SUCCESS) {
			return FAILURE;
		}
		if (w->ip == before || w->ip > limit) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
	}
	if (!out->has_value && !tyhp_branch_adopt_last(w, start_seq, out)) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_parse_logic(tyhp_walk *w, const char *symbol)
{
	const zend_op *op;
	zval left, right, node;
	uint32_t end, result_slot, guard;
	bool right_set = 0;

	ZVAL_UNDEF(&left);
	ZVAL_UNDEF(&right);
	ZVAL_UNDEF(&node);
	op = &w->ops[w->ip];
	end = tyhp_target(w, op, op->op2);
	if (end == UINT32_MAX || end > w->count) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &result_slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &left) != SUCCESS) {
		return FAILURE;
	}
	w->ip++;
	guard = w->count + 1;
	while (w->ip < end && !w->failed) {
		uint32_t before;

		if (guard-- == 0) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
		if (w->ops[w->ip].opcode == ZEND_BOOL && w->ip + 1 == end) {
			if (tyhp_read(w, &w->ops[w->ip], w->ops[w->ip].op1, w->ops[w->ip].op1_type, &right) != SUCCESS) {
				goto fail;
			}
			w->ip = end;
			right_set = 1;
			break;
		}
		before = w->ip;
		if (tyhp_parse_one(w) != SUCCESS) {
			goto fail;
		}
		if (w->ip == before) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
	}
	if (w->failed || w->ip != end) {
		tyhp_fail(w, TYHP_MSG_BODY);
		goto fail;
	}
	if (!right_set) {
		if (Z_TYPE(w->slots[result_slot]) == IS_UNDEF) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
		ZVAL_COPY(&right, &w->slots[result_slot]);
	}
	tyhp_node_binary(&node, symbol, &left, &right);
	tyhp_store(w, result_slot, &node);
	return w->failed ? FAILURE : SUCCESS;
fail:
	zval_ptr_dtor(&left);
	zval_ptr_dtor(&right);
	return FAILURE;
}

static zend_result tyhp_parse_short(tyhp_walk *w, bool elvis)
{
	const zend_op *op;
	zval left, right, node;
	uint32_t end, result_slot, guard;

	ZVAL_UNDEF(&left);
	ZVAL_UNDEF(&right);
	ZVAL_UNDEF(&node);
	op = &w->ops[w->ip];
	end = tyhp_target(w, op, op->op2);
	if (end == UINT32_MAX || end > w->count) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &result_slot) != SUCCESS) {
		return FAILURE;
	}
	if (tyhp_read(w, op, op->op1, op->op1_type, &left) != SUCCESS) {
		return FAILURE;
	}
	w->ip++;
	guard = w->count + 1;
	while (w->ip < end && !w->failed) {
		uint32_t before = w->ip;

		if (guard-- == 0) {
			tyhp_fail(w, TYHP_MSG_BODY);
			zval_ptr_dtor(&left);
			return FAILURE;
		}
		if (tyhp_parse_one(w) != SUCCESS) {
			zval_ptr_dtor(&left);
			return FAILURE;
		}
		if (w->ip == before) {
			zval_ptr_dtor(&left);
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
	}
	if (w->failed || w->ip != end || Z_TYPE(w->slots[result_slot]) == IS_UNDEF) {
		zval_ptr_dtor(&left);
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	ZVAL_COPY(&right, &w->slots[result_slot]);
	if (elvis) {
		array_init(&node);
		add_assoc_string(&node, "kind", "ternary");
		add_assoc_zval(&node, "cond", &left);
		add_assoc_null(&node, "then");
		add_assoc_zval(&node, "else", &right);
	} else {
		array_init(&node);
		add_assoc_string(&node, "kind", "coalesce");
		add_assoc_zval(&node, "left", &left);
		add_assoc_zval(&node, "right", &right);
	}
	tyhp_store(w, result_slot, &node);
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_parse_jmp_null(tyhp_walk *w)
{
	const zend_op *op;
	uint32_t end, result_slot, guard;

	op = &w->ops[w->ip];
	end = tyhp_target(w, op, op->op2);
	if (end == UINT32_MAX || end > w->count) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &result_slot) != SUCCESS) {
		return FAILURE;
	}
	w->ip++;
	if (w->ip < w->count && w->ops[w->ip].opcode == ZEND_INIT_METHOD_CALL) {
		if (tyhp_parse_call(w, "nullsafe_call") != SUCCESS) {
			return FAILURE;
		}
		if (w->ip != end || Z_TYPE(w->slots[result_slot]) == IS_UNDEF) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
		return SUCCESS;
	}
	w->nullsafe_fetch = 1;
	guard = w->count + 1;
	while (w->ip < end && !w->failed) {
		uint32_t before = w->ip;

		if (guard-- == 0) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
		if (tyhp_parse_one(w) != SUCCESS) {
			return FAILURE;
		}
		if (w->ip == before) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
	}
	w->nullsafe_fetch = 0;
	if (w->failed) {
		return FAILURE;
	}
	if (w->ip != end || Z_TYPE(w->slots[result_slot]) == IS_UNDEF) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	return SUCCESS;
}

/* Collects what the BIND_LEXICAL ops after a DECLARE_LAMBDA_FUNCTION bind into
 * the new closure, and moves ip past them. Returns NULL after tyhp_fail. */
static HashTable *tyhp_lambda_bindings(tyhp_walk *w, const zend_op *declare)
{
	HashTable *outer = zend_new_array(4);
	uint32_t next = w->ip + 1;

	while (next < w->count) {
		const zend_op *bind = &w->ops[next];
		uint32_t cv;
		zend_string *name;
		zval *source, value;

		if (bind->opcode != ZEND_BIND_LEXICAL || bind->op1_type != declare->result_type || bind->op1.var != declare->result.var) {
			break;
		}
		if ((bind->extended_value & ZEND_BIND_IMPLICIT) == 0 || (bind->extended_value & (ZEND_BIND_EXPLICIT | ZEND_BIND_REF)) != 0) {
			tyhp_fail(w, TYHP_MSG_USE);
			goto fail;
		}
		if (bind->op2_type != IS_CV) {
			tyhp_fail_op(w, bind);
			goto fail;
		}
		cv = tyhp_slot_of(bind, bind->op2.var);
		if (cv >= w->cv_count || !w->op_array->vars) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
		name = w->op_array->vars[cv];
		source = &w->slots[cv];
		if (Z_TYPE_P(source) == IS_UNDEF) {
			tyhp_fail(w, TYHP_MSG_BODY);
			goto fail;
		}
		ZVAL_NULL(&value);
		if (tyhp_is_kind(source, "capture")) {
			zval *bound = zend_hash_str_find(Z_ARRVAL_P(source), "value", sizeof("value") - 1);

			if (bound) {
				ZVAL_COPY(&value, bound);
			}
		}
		zend_hash_update(outer, name, &value);
		w->cv_used[cv] = 1;
		next++;
	}
	w->ip = next;
	return outer;
fail:
	zend_array_destroy(outer);
	return NULL;
}

static zend_result tyhp_declare_lambda(tyhp_walk *w, const zend_op *op)
{
	uint32_t index, slot;
	zend_op_array *child;
	zval body, params, captures, node;
	HashTable *outer;
	zend_result status;

	ZVAL_UNDEF(&body);
	ZVAL_UNDEF(&params);
	ZVAL_UNDEF(&captures);
	ZVAL_UNDEF(&node);
	index = op->op2.num;
	if (!w->op_array->dynamic_func_defs || index >= w->op_array->num_dynamic_func_defs || !w->op_array->dynamic_func_defs[index]) {
		tyhp_fail_op(w, op);
		return FAILURE;
	}
	if (w->depth >= TYHP_MAX_LAMBDA_DEPTH) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	if (tyhp_result_slot(w, op, &slot) != SUCCESS) {
		return FAILURE;
	}
	child = w->op_array->dynamic_func_defs[index];
	outer = tyhp_lambda_bindings(w, op);
	if (!outer) {
		return FAILURE;
	}
	status = tyhp_walk_op_array(child, outer, w->depth + 1, &body, &params, &captures);
	zend_array_destroy(outer);
	if (status != SUCCESS) {
		w->failed = 1;
		return FAILURE;
	}
	array_init(&node);
	add_assoc_string(&node, "kind", "lambda");
	add_assoc_zval(&node, "params", &params);
	add_assoc_zval(&node, "captures", &captures);
	add_assoc_zval(&node, "body", &body);
	tyhp_store(w, slot, &node);
	return w->failed ? FAILURE : SUCCESS;
}

static zend_result tyhp_parse_one(tyhp_walk *w)
{
	const zend_op *op;

	if (w->failed || EG(exception)) {
		w->failed = 1;
		return FAILURE;
	}
	if (w->ip >= w->count) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	if (++w->steps > TYHP_MAX_STEPS) {
		tyhp_fail(w, TYHP_MSG_BODY);
		return FAILURE;
	}
	op = &w->ops[w->ip];
	switch (op->opcode) {
		case ZEND_ADD:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "+", "+");
		case ZEND_SUB:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "-", "-");
		case ZEND_MUL:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "*", "*");
		case ZEND_DIV:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "/", "/");
		case ZEND_MOD:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "%", "%");
		case ZEND_POW:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "**", "**");
		case ZEND_CONCAT:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, ".", ".");
		case ZEND_SL:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "<<", "<<");
		case ZEND_SR:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, ">>", ">>");
		case ZEND_BW_OR:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "|", "|");
		case ZEND_BW_AND:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "&", "&");
		case ZEND_BW_XOR:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "^", "^");
		case ZEND_BOOL_XOR:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "xor", "xor");
		case ZEND_IS_SMALLER:
			return tyhp_binary(w, op, TYHP_ORDER_SMALLER, "<", ">");
		case ZEND_IS_SMALLER_OR_EQUAL:
			return tyhp_binary(w, op, TYHP_ORDER_SMALLER, "<=", ">=");
		case ZEND_IS_EQUAL:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "==", "==");
		case ZEND_IS_NOT_EQUAL:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "!=", "!=");
		case ZEND_IS_IDENTICAL:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "===", "===");
		case ZEND_IS_NOT_IDENTICAL:
			return tyhp_binary(w, op, TYHP_ORDER_COMMUTATIVE, "!==", "!==");
		case ZEND_SPACESHIP:
			return tyhp_binary(w, op, TYHP_ORDER_SOURCE, "<=>", "<=>");
		case ZEND_BW_NOT:
			return tyhp_unary(w, op, "~");
		case ZEND_BOOL_NOT:
			return tyhp_unary(w, op, "!");
		case ZEND_BOOL:
			return tyhp_cast(w, op, "bool");
		case ZEND_CAST:
			return tyhp_cast(w, op, tyhp_cast_type(op->extended_value));
		case ZEND_TYPE_CHECK:
			return tyhp_type_check(w, op);
		case ZEND_FETCH_OBJ_R:
		case ZEND_FETCH_OBJ_IS:
		case ZEND_FETCH_OBJ_FUNC_ARG:
			return tyhp_fetch_prop(w, op);
		case ZEND_FETCH_DIM_R:
			return tyhp_fetch_dim(w, op);
		case ZEND_INSTANCEOF:
			return tyhp_instanceof(w, op);
		case ZEND_FETCH_CLASS:
			return tyhp_fetch_class(w, op);
		case ZEND_FETCH_CLASS_CONSTANT:
			return tyhp_class_const(w, op);
		case ZEND_RECV:
			return tyhp_recv(w, op);
		case ZEND_BIND_STATIC:
			return tyhp_bind_static(w, op);
		case ZEND_QM_ASSIGN:
			return tyhp_qm_assign(w, op);
		case ZEND_INIT_ARRAY:
			return tyhp_init_array(w, op);
		case ZEND_ADD_ARRAY_ELEMENT:
			return tyhp_add_array_element(w, op);
		case ZEND_NEW:
			return tyhp_parse_call(w, "new");
		case ZEND_INIT_METHOD_CALL:
			return tyhp_parse_call(w, "call");
		case ZEND_INIT_STATIC_METHOD_CALL:
			return tyhp_parse_call(w, "static_call");
		case ZEND_DECLARE_LAMBDA_FUNCTION:
			return tyhp_declare_lambda(w, op);
		case ZEND_JMPZ_EX:
			return tyhp_parse_logic(w, "&&");
		case ZEND_JMPNZ_EX:
			return tyhp_parse_logic(w, "||");
		case ZEND_COALESCE:
			return tyhp_parse_short(w, 0);
		case ZEND_JMP_SET:
			return tyhp_parse_short(w, 1);
		case ZEND_JMP_NULL:
			return tyhp_parse_jmp_null(w);
		case ZEND_JMPZ:
			{
				zval nested;
				bool exits = 0;

				if (tyhp_parse_ternary(w, &nested, &exits) != SUCCESS) {
					return FAILURE;
				}
				if (exits) {
					if (Z_TYPE(w->result) != IS_UNDEF) {
						zval_ptr_dtor(&w->result);
					}
					ZVAL_COPY_VALUE(&w->result, &nested);
					w->has_result = 1;
				}
				return w->failed ? FAILURE : SUCCESS;
			}
		default:
			tyhp_fail_op(w, op);
			return FAILURE;
	}
}

static void tyhp_walk_destroy(tyhp_walk *w)
{
	uint32_t i;

	if (w->slots) {
		for (i = 0; i < w->slot_count; i++) {
			zval_ptr_dtor(&w->slots[i]);
		}
		efree(w->slots);
		w->slots = NULL;
	}
	if (w->slot_seq) {
		efree(w->slot_seq);
		w->slot_seq = NULL;
	}
	zval_ptr_dtor(&w->result);
	ZVAL_UNDEF(&w->result);
	if (w->cv_used) {
		efree(w->cv_used);
		w->cv_used = NULL;
	}
	if (w->cv_capture) {
		efree(w->cv_capture);
		w->cv_capture = NULL;
	}
	if (w->capture_order) {
		efree(w->capture_order);
		w->capture_order = NULL;
	}
	if (w->param_names) {
		efree(w->param_names);
		w->param_names = NULL;
	}
}

static bool tyhp_prop_chain(zval *node, bool at_root)
{
	zval *object;

	if (tyhp_is_kind(node, "param")) {
		return !at_root;
	}
	if (!tyhp_is_kind(node, "prop") && !tyhp_is_kind(node, "nullsafe_prop")) {
		return 0;
	}
	object = zend_hash_str_find(Z_ARRVAL_P(node), "object", sizeof("object") - 1);
	if (!object) {
		return 0;
	}
	return tyhp_prop_chain(object, 0);
}

static zend_result tyhp_export_meta(tyhp_walk *w, zval *params_out, zval *captures_out)
{
	uint32_t i;

	array_init(params_out);
	array_init(captures_out);
	for (i = 0; i < w->param_count; i++) {
		add_next_index_str(params_out, zend_string_copy(w->param_names[i]));
	}
	for (i = 0; i < w->capture_count; i++) {
		uint32_t cv = w->capture_order[i];
		zval copy;

		if (cv >= w->cv_count || !w->cv_used[cv] || !w->cv_capture[cv]) {
			continue;
		}
		if (Z_TYPE(w->slots[cv]) == IS_UNDEF) {
			tyhp_fail(w, TYHP_MSG_BODY);
			return FAILURE;
		}
		ZVAL_COPY(&copy, &w->slots[cv]);
		add_next_index_zval(captures_out, &copy);
	}
	return SUCCESS;
}

static zend_result tyhp_walk_op_array(const zend_op_array *op_array, HashTable *outer, uint32_t depth, zval *body_out, zval *params_out, zval *captures_out)
{
	tyhp_walk w;
	uint32_t guard;
	int last_var;

	ZVAL_UNDEF(body_out);
	if (params_out) {
		ZVAL_UNDEF(params_out);
	}
	if (captures_out) {
		ZVAL_UNDEF(captures_out);
	}
	memset(&w, 0, sizeof(w));
	ZVAL_UNDEF(&w.result);
	w.op_array = op_array;
	w.outer = outer;
	w.depth = depth;
	if (!op_array || !op_array->opcodes) {
		tyhp_fail(&w, TYHP_MSG_BODY);
		tyhp_walk_destroy(&w);
		return FAILURE;
	}
	last_var = op_array->last_var;
	if (last_var < 0) {
		tyhp_fail(&w, TYHP_MSG_BODY);
		tyhp_walk_destroy(&w);
		return FAILURE;
	}
	w.ops = op_array->opcodes;
	w.count = op_array->last;
	w.cv_count = (uint32_t) last_var;
	w.slot_count = w.cv_count + op_array->T;
	if (w.slot_count > 0) {
		w.slots = ecalloc(w.slot_count, sizeof(zval));
		w.slot_seq = ecalloc(w.slot_count, sizeof(uint32_t));
	}
	if (w.cv_count > 0) {
		w.cv_used = ecalloc(w.cv_count, 1);
		w.cv_capture = ecalloc(w.cv_count, 1);
		w.capture_order = ecalloc(w.cv_count, sizeof(uint32_t));
		w.param_names = ecalloc(w.cv_count, sizeof(zend_string *));
	}
	guard = w.count + 1;
	while (w.ip < w.count && !w.failed && !w.has_result) {
		const zend_op *op;
		uint32_t before;

		if (guard-- == 0) {
			tyhp_fail(&w, TYHP_MSG_BODY);
			break;
		}
		op = &w.ops[w.ip];
		if (op->opcode == ZEND_RETURN) {
			zval value;

			if (tyhp_read_return(&w, op, &value) != SUCCESS) {
				break;
			}
			ZVAL_COPY_VALUE(&w.result, &value);
			w.has_result = 1;
			w.ip++;
			break;
		}
		before = w.ip;
		if (tyhp_parse_one(&w) != SUCCESS) {
			break;
		}
		if (w.ip == before) {
			tyhp_fail(&w, TYHP_MSG_BODY);
			break;
		}
	}
	if (!w.failed && !w.has_result) {
		tyhp_fail(&w, TYHP_MSG_BODY);
	}
	if (!w.failed && params_out && captures_out) {
		if (tyhp_export_meta(&w, params_out, captures_out) != SUCCESS) {
			zval_ptr_dtor(params_out);
			zval_ptr_dtor(captures_out);
			ZVAL_UNDEF(params_out);
			ZVAL_UNDEF(captures_out);
		}
	}
	if (w.failed || Z_TYPE(w.result) == IS_UNDEF) {
		tyhp_walk_destroy(&w);
		if (params_out) {
			zval_ptr_dtor(params_out);
			ZVAL_UNDEF(params_out);
		}
		if (captures_out) {
			zval_ptr_dtor(captures_out);
			ZVAL_UNDEF(captures_out);
		}
		return FAILURE;
	}
	ZVAL_COPY_VALUE(body_out, &w.result);
	ZVAL_UNDEF(&w.result);
	tyhp_walk_destroy(&w);
	return SUCCESS;
}

PHP_METHOD(Ext, closureTree)
{
	zval *fn;
	zend_string *mode;
	const zend_function *func;
	zval body;
	bool property_path;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_OBJECT_OF_CLASS(fn, zend_ce_closure)
		Z_PARAM_STR(mode)
	ZEND_PARSE_PARAMETERS_END();

	if (zend_string_equals_literal(mode, "expression")) {
		property_path = 0;
	} else if (zend_string_equals_literal(mode, "property-path")) {
		property_path = 1;
	} else {
		zend_throw_exception(tyhp_iae_ce(), "closureTree mode must be \"expression\" or \"property-path\"", 0);
		RETURN_THROWS();
	}

	func = zend_get_closure_method_def(Z_OBJ_P(fn));
	if (!func || func->type != ZEND_USER_FUNCTION) {
		zend_throw_exception(tyhp_iae_ce(), TYHP_MSG_BODY, 0);
		RETURN_THROWS();
	}
	if (tyhp_walk_op_array(&func->op_array, NULL, 0, &body, NULL, NULL) != SUCCESS) {
		RETURN_THROWS();
	}
	if (property_path && !tyhp_prop_chain(&body, 1)) {
		zval_ptr_dtor(&body);
		zend_throw_exception(tyhp_iae_ce(), "closureTree property-path must be a chain of property fetches", 0);
		RETURN_THROWS();
	}
	ZVAL_COPY_VALUE(return_value, &body);
}
