--TEST--
closureTree rejects opcodes outside the expression whitelist
--EXTENSIONS--
tyhp
--INI--
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/closure_tree.inc';

ct_throw(fn () => ct(fn ($u) => $u->a = 1), 'assign');
ct_throw(fn () => ct(fn ($u) => ++$u->a), 'inc');
ct_throw(fn () => ct(fn ($u) => throw new \Exception('x')), 'throw');
ct_throw(fn () => ct(fn ($u) => match ($u->a) { 1 => 2, default => 3 }), 'match');
ct_throw(fn () => ct(fn ($u) => [...$u->a]), 'unpack');
ct_throw(fn () => ct(fn ($u) => \strlen($u->a)), 'strlen');
ct_throw(fn () => ct(fn ($u) => \trim($u->a)), 'trim');
$previous = \error_reporting(E_ALL & ~E_DEPRECATED);
$backtick = eval('return fn ($u) => `$u->a`;');
\error_reporting($previous);
ct_throw(fn () => ct($backtick), 'backtick');
$name = 'a';
ct_throw(fn () => ct(fn ($u) => $u->$name), 'dynamic member');
ct_throw(fn () => ct(fn ($u) => isset($u->a)), 'isset');
ct_throw(fn () => ct(fn ($u) => $u::class), 'class name');
ct_throw(fn () => ct(fn ($u) => include $u->a), 'include');

// A nested closure with a statement body is not an arrow function. Its only
// RETURN is the implicit one, so it is rejected at every depth.
ct_throw(fn () => ct(fn ($u) => function () {}), 'nested function');
ct_throw(fn () => ct(fn ($u) => function ($p) {}), 'nested function with parameter');
ct_throw(fn () => ct(fn ($u) => fn ($p) => function () {}), 'doubly nested function');
ct_throw(fn () => ct(fn ($u) => static function () {}), 'nested static function');

// An explicit use binding is not an implicit arrow capture.
$n = 5;
ct_throw(fn () => ct(fn ($u) => function () use ($n) { return $n; }), 'nested function use');
ct_throw(fn () => ct(fn ($u) => function () use (&$n) { return $n; }), 'nested function use by reference');
ct_throw(fn () => ct(function ($u) use ($n) { return $u->a; }), 'root function use');
ct_throw(fn () => ct(function ($u) use (&$n) { return $u->a; }), 'root function use by reference');

// A root closure with no returned expression.
ct_throw(fn () => ct(function ($u) {}), 'root function without return');
ct_throw(fn () => ct(function () {}), 'root function without parameter');

// The mode is matched exactly.
foreach (['nope', 'Expression', ' expression', 'expression ', 'property_path', 'PROPERTY-PATH', 'property-path ', 'expr', '', "expression\0", "expression\0x"] as $mode) {
    ct_throw(fn () => ct(fn ($u) => $u->a, $mode), 'mode ' . \var_export($mode, true));
}

// Failures found after part of the tree has been built free what was built.
ct_throw(fn () => ct(fn ($u) => [$u->a, $u->b, ++$u->c]), 'array element');
ct_throw(fn () => ct(fn ($u) => $u->m([$u->a], $u->b = 1)), 'call argument');
ct_throw(fn () => ct(fn ($u) => new \stdClass($u->a, $u->b = 1)), 'new argument');
ct_throw(fn () => ct(fn ($u) => fn ($p) => $p->a = 1), 'nested body');
ct_throw(fn () => ct(fn ($u) => fn ($p) => fn ($q) => [$u->a, $q->b = 1]), 'deeply nested body');
ct_throw(fn () => ct(fn ($u) => $u->a ? $u->b : ++$u->c), 'ternary arm');
ct_throw(fn () => ct(fn ($u) => $u->a && ++$u->c), 'logical right');
ct_throw(fn () => ct(fn ($u) => $u->a ?? ++$u->c), 'coalesce right');
ct_throw(fn () => ct(fn ($u) => $u?->a(++$u->c)), 'nullsafe call argument');

// The failures above must leave the extension usable.
$ok = ct(fn ($u) => $u->a);
ct_u_prop($ok, 'a', 'after failures');

echo "ok\n";
--EXPECT--
ok
