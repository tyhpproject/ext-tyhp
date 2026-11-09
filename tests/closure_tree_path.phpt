--TEST--
closureTree property-path mode
--EXTENSIONS--
tyhp
--INI--
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/closure_tree.inc';

$chain = ct(fn ($u) => $u->a->b, 'property-path');
$object = ct_prop($chain, 'b', false, 'path b');
$object = ct_prop($object, 'a', false, 'path a');
ct_param($object, 'u', 'path u');

$single = ct(fn ($u) => $u->a, 'property-path');
ct_param(ct_prop($single, 'a', false, 'path single'), 'u', 'path single u');

$nullsafe = ct(fn ($u) => $u?->a->b, 'property-path');
$object = ct_prop($nullsafe, 'b', false, 'path ns b');
$object = ct_prop($object, 'a', true, 'path ns a');
ct_param($object, 'u', 'path ns u');

$nullsafeAll = ct(fn ($u) => $u?->a?->b, 'property-path');
$object = ct_prop($nullsafeAll, 'b', true, 'path ns all b');
$object = ct_prop($object, 'a', true, 'path ns all a');
ct_param($object, 'u', 'path ns all u');

// Anything that is not a chain of property fetches from the parameter.
$captured = new \stdClass();
$list = ['k' => 1];
ct_throw(fn () => ct(fn ($u) => $u, 'property-path'), 'bare parameter');
ct_throw(fn () => ct(fn ($u) => $captured->a, 'property-path'), 'captured object');
ct_throw(fn () => ct(fn ($u) => $u->a['k'], 'property-path'), 'index path');
ct_throw(fn () => ct(fn ($u) => $list['k'], 'property-path'), 'captured index');
ct_throw(fn () => ct(fn ($u) => $u->a + 1, 'property-path'), 'binary path');
ct_throw(fn () => ct(fn ($u) => $u->a->m(), 'property-path'), 'method call');
ct_throw(fn () => ct(fn ($u) => 1, 'property-path'), 'constant');
ct_throw(fn () => ct(fn ($u) => null, 'property-path'), 'null');
ct_throw(fn () => ct(fn ($u) => $u->a ?? 1, 'property-path'), 'coalesce');
ct_throw(fn () => ct(fn ($u) => !$u->a, 'property-path'), 'unary');
ct_throw(fn () => ct(fn ($u) => fn ($p) => $p->a, 'property-path'), 'lambda');

echo "ok\n";
--EXPECT--
ok
