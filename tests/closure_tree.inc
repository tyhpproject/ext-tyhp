<?php

function ct(\Closure $fn, string $mode = 'expression'): array
{
    return \Tyhp\Ext::closureTree($fn, $mode);
}

function ct_fail(string $message): void
{
    echo $message, "\n";
    exit(1);
}

function ct_kind(array $node, string $kind, string $path): array
{
    if (($node['kind'] ?? null) !== $kind) {
        ct_fail($path . ' kind ' . \var_export($node['kind'] ?? null, true) . ' !== ' . $kind);
    }
    return $node;
}

function ct_param(array $node, string $name, string $path): void
{
    ct_kind($node, 'param', $path);
    if (($node['name'] ?? null) !== $name) {
        ct_fail($path . ' param name ' . \var_export($node['name'] ?? null, true));
    }
}

function ct_const(array $node, mixed $value, string $path): void
{
    ct_kind($node, 'const', $path);
    if (!\array_key_exists('value', $node) || $node['value'] !== $value) {
        ct_fail($path . ' const ' . \var_export($node['value'] ?? 'MISSING', true) . ' !== ' . \var_export($value, true));
    }
}

function ct_prop(array $node, string $name, bool $nullsafe, string $path): array
{
    $object = ct_kind($node, $nullsafe ? 'nullsafe_prop' : 'prop', $path);
    if (($node['name'] ?? null) !== $name) {
        ct_fail($path . ' prop name ' . \var_export($node['name'] ?? null, true));
    }
    if (!isset($object['object']) || !\is_array($object['object'])) {
        ct_fail($path . ' missing object');
    }
    return $object['object'];
}

/** A property of the parameter $u: $u->$name. */
function ct_u_prop(array $node, string $name, string $path): void
{
    ct_param(ct_prop($node, $name, false, $path), 'u', $path . ' param');
}

function ct_binary(array $node, string $op, string $path): array
{
    ct_kind($node, 'binary', $path);
    if (($node['op'] ?? null) !== $op) {
        ct_fail($path . ' op ' . \var_export($node['op'] ?? null, true));
    }
    return $node;
}

function ct_left_prop(array $node, string $path): void
{
    ct_u_prop($node['left'], 'a', $path . ' left');
}

function ct_right_prop(array $node, string $name, string $path): void
{
    ct_u_prop($node['right'], $name, $path . ' right');
}

/** $u->a OP 1, with the property on the left and the literal 1 on the right. */
function ct_cmp(string $op, \Closure $fn): void
{
    $node = ct_binary(ct($fn), $op, $op);
    ct_left_prop($node, $op);
    ct_const($node['right'], 1, $op . ' right');
}

/** $u->$left OP $u->$right: both operands computed, so source order is recoverable. */
function ct_sides(string $op, \Closure $fn, string $left, string $right): void
{
    $label = $op . ' ' . $left . ' ' . $right;
    $node = ct_binary(ct($fn), $op, $label);
    ct_u_prop($node['left'], $left, $label . ' left');
    ct_u_prop($node['right'], $right, $label . ' right');
}

function ct_two_props(string $kind, string $op, \Closure $fn): void
{
    $node = ct($fn);
    if ($kind === 'binary') {
        $node = ct_binary($node, $op, $op);
        ct_left_prop($node, $op);
        ct_right_prop($node, 'b', $op);
        return;
    }
    ct_kind($node, $kind, $op);
    ct_u_prop($node['left'], 'a', $op . ' left');
    ct_u_prop($node['right'], 'b', $op . ' right');
}

/** cond ? then : else, where each arm is $u->name. */
function ct_ternary(array $node, string $cond, string $then, string $else, string $path): void
{
    ct_kind($node, 'ternary', $path);
    ct_u_prop($node['cond'], $cond, $path . ' cond');
    ct_u_prop($node['then'], $then, $path . ' then');
    ct_u_prop($node['else'], $else, $path . ' else');
}

function ct_cast(string $type, \Closure $fn): void
{
    $node = ct_kind(ct($fn), 'cast', $type);
    if (($node['type'] ?? null) !== $type) {
        ct_fail($type . ' type ' . \var_export($node['type'] ?? null, true));
    }
    ct_u_prop($node['expr'], 'a', $type);
}

function ct_unary(string $op, \Closure $fn): void
{
    $node = ct_kind(ct($fn), 'unary', $op);
    if (($node['op'] ?? null) !== $op) {
        ct_fail($op . ' unary op ' . \var_export($node['op'] ?? null, true));
    }
    ct_u_prop($node['expr'], 'a', $op);
}

function ct_throw(callable $fn, string $label): void
{
    try {
        $fn();
    } catch (\InvalidArgumentException $e) {
        return;
    } catch (\Throwable $e) {
        ct_fail($label . ' threw ' . $e::class . ': ' . $e->getMessage());
    }
    ct_fail($label . ' did not throw');
}
