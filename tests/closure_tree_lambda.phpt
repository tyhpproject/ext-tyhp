--TEST--
closureTree nested arrow functions and captures
--EXTENSIONS--
tyhp
--INI--
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/closure_tree.inc';

function ct_capture(array $node, string $name, mixed $value, string $path): void
{
    ct_kind($node, 'capture', $path);
    if (($node['name'] ?? null) !== $name) {
        ct_fail($path . ' capture name ' . \var_export($node['name'] ?? null, true));
    }
    if (!\array_key_exists('value', $node) || $node['value'] !== $value) {
        ct_fail($path . ' capture value ' . \var_export($node['value'] ?? 'MISSING', true) . ' !== ' . \var_export($value, true));
    }
}

/** Checks the lambda node's shape and returns it. */
function ct_lambda(array $node, array $params, int $captures, string $path): array
{
    ct_kind($node, 'lambda', $path);
    if ($node['params'] !== $params) {
        ct_fail($path . ' params ' . \var_export($node['params'], true));
    }
    if (\count($node['captures']) !== $captures) {
        ct_fail($path . ' capture count ' . \count($node['captures']));
    }
    return $node;
}

$n = 5;

// A nested arrow function with a real body, and one that reads the outer
// parameter. The outer parameter has no value at inspection time, so its
// capture carries null and the name identifies it.
$lambda = ct_lambda(ct(fn ($u) => fn ($p) => $p->a - $u->b), ['p'], 1, 'outer param');
ct_capture($lambda['captures'][0], 'u', null, 'outer param capture');
$body = ct_binary($lambda['body'], '-', 'outer param body');
ct_param(ct_prop($body['left'], 'a', false, 'outer param left'), 'p', 'outer param left object');
ct_capture(ct_prop($body['right'], 'b', false, 'outer param right'), 'u', null, 'outer param right object');

// A nested arrow function that captures an outer capture carries its value.
$lambda = ct_lambda(ct(fn ($u) => fn ($p) => $p->a - $n), ['p'], 1, 'outer capture');
ct_capture($lambda['captures'][0], 'n', 5, 'outer capture capture');
$body = ct_binary($lambda['body'], '-', 'outer capture body');
ct_param(ct_prop($body['left'], 'a', false, 'outer capture left'), 'p', 'outer capture left object');
ct_capture($body['right'], 'n', 5, 'outer capture right');

// An arrow function with no parameter and no capture.
$lambda = ct_lambda(ct(fn ($u) => fn () => $u->a), [], 1, 'no params');
$lambda = ct_lambda(ct(fn ($u) => fn ($p, $q) => $q->a), ['p', 'q'], 0, 'two params');
ct_param(ct_prop($lambda['body'], 'a', false, 'two params body'), 'q', 'two params body object');

// A body of null is a real arrow body: `function () {}` has the same opcodes
// except for its implicit return, which the walker tells apart.
$lambda = ct_lambda(ct(fn ($u) => fn () => null), [], 0, 'null body');
ct_const($lambda['body'], null, 'null body value');
$lambda = ct_lambda(ct(fn ($u) => fn ($p) => null), ['p'], 0, 'null body param');
ct_const($lambda['body'], null, 'null body param value');

// static arrow functions are accepted.
$lambda = ct_lambda(ct(fn ($u) => static fn ($p) => $p->a), ['p'], 0, 'static');
ct_param(ct_prop($lambda['body'], 'a', false, 'static body'), 'p', 'static body object');
$lambda = ct_lambda(ct(fn ($u) => static fn ($p) => $p->a - $n), ['p'], 1, 'static capture');
ct_capture($lambda['captures'][0], 'n', 5, 'static capture capture');

// Nesting two levels deep passes the capture through the middle function.
$outer = ct_lambda(ct(fn ($u) => fn ($p) => fn ($q) => $q->a - $n), ['p'], 1, 'deep outer');
ct_capture($outer['captures'][0], 'n', 5, 'deep outer capture');
$inner = ct_lambda($outer['body'], ['q'], 1, 'deep inner');
ct_capture($inner['captures'][0], 'n', 5, 'deep inner capture');
$body = ct_binary($inner['body'], '-', 'deep body');
ct_capture($body['right'], 'n', 5, 'deep body capture');

// A lambda is an argument like any other value.
$call = ct(fn ($u) => $u->m(fn ($p) => $p->a));
ct_kind($call, 'call', 'lambda arg');
ct_lambda($call['args'][0], ['p'], 0, 'lambda arg lambda');

// Captured values are copies owned by the tree. Changing the source after the
// call, or changing the tree, must not leak into the closure or a later call.
$list = [1, 2];
$fn = fn ($u) => $u->a === $list;
$first = ct($fn);
$list[] = 3;
$first['right']['value'][] = 9;
$second = ct($fn);
ct_capture($second['right'], 'list', [1, 2], 'owned capture');

$copy = $second;
$copy['right']['value'][] = 7;
ct_capture($second['right'], 'list', [1, 2], 'owned copy');

function ct_make_tree(): array
{
    $object = new \stdClass();
    $object->x = ['y' => 1];
    return ct(fn ($u) => $u->a === $object);
}

$tree = ct_make_tree();
$value = $tree['right']['value'];
if (!($value instanceof \stdClass) || $value->x !== ['y' => 1]) {
    ct_fail('object capture after the closure was destroyed');
}

echo "ok\n";
--EXPECT--
ok
