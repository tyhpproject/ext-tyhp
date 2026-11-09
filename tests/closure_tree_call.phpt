--TEST--
closureTree calls, arrays, lambdas, and captures
--EXTENSIONS--
tyhp
--INI--
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/closure_tree.inc';

class Foo
{
    public function __construct($a = null, $b = null)
    {
    }

    public static function s($a, $b = null)
    {
        return $a;
    }

    public static function e($a)
    {
    }

    public function m($a = null)
    {
        return $this;
    }
}

$instance = ct(fn ($u) => $u instanceof Foo);
ct_kind($instance, 'instanceof', 'instanceof');
ct_param($instance['expr'], 'u', 'instanceof expr');
if (($instance['class'] ?? null) !== 'Foo') {
    ct_fail('instanceof class ' . \var_export($instance['class'] ?? null, true));
}

$created = ct(fn ($u) => new Foo($u->a, 'x'));
ct_kind($created, 'new', 'new');
if (($created['class'] ?? null) !== 'Foo') {
    ct_fail('new class');
}
if (\count($created['args']) !== 2) {
    ct_fail('new argc');
}
$object = ct_prop($created['args'][0], 'a', false, 'new arg0');
ct_param($object, 'u', 'new arg0 param');
ct_const($created['args'][1], 'x', 'new arg1');

$call = ct(fn ($u) => $u->m($u->a, 'x'));
ct_kind($call, 'call', 'call');
ct_param($call['object'], 'u', 'call object');
if (($call['name'] ?? null) !== 'm') {
    ct_fail('call name');
}
$object = ct_prop($call['args'][0], 'a', false, 'call arg0');
ct_param($object, 'u', 'call arg0 param');
ct_const($call['args'][1], 'x', 'call arg1');

$nullsafeCall = ct(fn ($u) => $u?->m($u->a));
ct_kind($nullsafeCall, 'nullsafe_call', 'nullsafe call');
ct_param($nullsafeCall['object'], 'u', 'nullsafe call object');
if (($nullsafeCall['name'] ?? null) !== 'm') {
    ct_fail('nullsafe call name');
}
$object = ct_prop($nullsafeCall['args'][0], 'a', false, 'nullsafe call arg');
ct_param($object, 'u', 'nullsafe call arg param');

// Foo::s is a real static method that returns a value, so a call to it
// survives the optimizer.
$static = ct(fn ($u) => Foo::s($u->a, 'x'));
ct_kind($static, 'static_call', 'static');
if (($static['class'] ?? null) !== 'Foo' || ($static['name'] ?? null) !== 's') {
    ct_fail('static name ' . \var_export($static['class'] ?? null, true) . '::' . \var_export($static['name'] ?? null, true));
}
$object = ct_prop($static['args'][0], 'a', false, 'static arg0');
ct_param($object, 'u', 'static arg0 param');
ct_const($static['args'][1], 'x', 'static arg1');

// A call that is itself an argument is sent with SEND_VAR_NO_REF(_EX).
$nested = ct(fn ($u) => Foo::s(Foo::s($u->a)));
ct_kind($nested, 'static_call', 'nested static');
$inner = ct_kind($nested['args'][0], 'static_call', 'nested static inner');
ct_u_prop($inner['args'][0], 'a', 'nested static inner arg');

$nested = ct(fn ($u) => $u->m($u->m(1)));
ct_kind($nested, 'call', 'nested call');
$inner = ct_kind($nested['args'][0], 'call', 'nested call inner');
ct_const($inner['args'][0], 1, 'nested call inner arg');

$nested = ct(fn ($u) => new Foo(new Foo($u->a)));
ct_kind($nested, 'new', 'nested new');
$inner = ct_kind($nested['args'][0], 'new', 'nested new inner');
ct_u_prop($inner['args'][0], 'a', 'nested new inner arg');

// The optimizer removes a call to a method with no effect and leaves only the
// argument's FREE. The tree must then be rejected: it must never come back as
// a constant that silently drops the call.
foreach ([fn ($u) => Foo::e($u->a), fn ($u) => Foo::e($u->a) . 'x'] as $i => $fn) {
    try {
        $tree = ct($fn);
    } catch (\InvalidArgumentException $e) {
        continue;
    }
    $node = $tree['kind'] === 'binary' ? $tree['left'] : $tree;
    ct_kind($node, 'static_call', 'elided call ' . $i);
}

// Calls as arguments of operators, and arguments that are expressions.
$mixed = ct_binary(ct(fn ($u) => $u->m($u->a ? $u->b : $u->c) . 'x'), '.', 'call ternary');
$callNode = ct_kind($mixed['left'], 'call', 'call ternary call');
ct_ternary($callNode['args'][0], 'a', 'b', 'c', 'call ternary arg');

$array = ct(fn ($u) => [$u->a, 'k' => $u->b]);
ct_kind($array, 'array', 'array');
if (\count($array['elements']) !== 2) {
    ct_fail('array count');
}
if (!\array_key_exists('key', $array['elements'][0]) || $array['elements'][0]['key'] !== null) {
    ct_fail('packed key ' . \var_export($array['elements'][0]['key'] ?? 'MISSING', true));
}
$object = ct_prop($array['elements'][0]['value'], 'a', false, 'array 0');
ct_param($object, 'u', 'array 0 param');
ct_const($array['elements'][1]['key'], 'k', 'array key');
$object = ct_prop($array['elements'][1]['value'], 'b', false, 'array 1');
ct_param($object, 'u', 'array 1 param');

$nestedArray = ct(fn ($u) => [[$u->a], 'k' => [$u->b => 1]]);
ct_kind($nestedArray, 'array', 'nested array');
ct_kind($nestedArray['elements'][0]['value'], 'array', 'nested array 0');
ct_u_prop($nestedArray['elements'][0]['value']['elements'][0]['value'], 'a', 'nested array 0 0');
ct_u_prop($nestedArray['elements'][1]['value']['elements'][0]['key'], 'b', 'nested array key');

echo "ok\n";
--EXPECT--
ok
