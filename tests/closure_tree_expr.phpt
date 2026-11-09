--TEST--
closureTree expression shapes
--EXTENSIONS--
tyhp
--INI--
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/closure_tree.inc';

$chain = ct(fn ($u) => $u->a->b);
$object = ct_prop($chain, 'b', false, 'chain b');
$object = ct_prop($object, 'a', false, 'chain a');
ct_param($object, 'u', 'chain u');

$nullsafe = ct(fn ($u) => $u?->a?->b);
$object = ct_prop($nullsafe, 'b', true, 'ns b');
$object = ct_prop($object, 'a', true, 'ns a');
ct_param($object, 'u', 'ns u');

$index = ct(fn ($u) => $u->a['k']);
ct_kind($index, 'index', 'index');
$object = ct_prop($index['object'], 'a', false, 'index object');
ct_param($object, 'u', 'index param');
ct_const($index['index'], 'k', 'index key');

ct_cmp('>', fn ($u) => $u->a > 1);
ct_cmp('>=', fn ($u) => $u->a >= 1);
ct_cmp('<', fn ($u) => $u->a < 1);
ct_cmp('<=', fn ($u) => $u->a <= 1);
ct_cmp('==', fn ($u) => $u->a == 1);
ct_cmp('!=', fn ($u) => $u->a != 1);
ct_cmp('===', fn ($u) => $u->a === 1);
ct_cmp('!==', fn ($u) => $u->a !== 1);
ct_cmp('<=>', fn ($u) => $u->a <=> 1);
ct_cmp('!=', fn ($u) => $u->a <> 1);

ct_two_props('binary', '&&', fn ($u) => $u->a && $u->b);
ct_two_props('binary', '||', fn ($u) => $u->a || $u->b);
ct_two_props('binary', '&&', fn ($u) => $u->a and $u->b);
ct_two_props('binary', '||', fn ($u) => $u->a or $u->b);
ct_two_props('binary', 'xor', fn ($u) => $u->a xor $u->b);

ct_two_props('coalesce', '??', fn ($u) => $u->a ?? $u->b);

$elvis = ct(fn ($u) => $u->a ?: $u->b);
ct_kind($elvis, 'ternary', 'elvis');
if (!\array_key_exists('then', $elvis) || $elvis['then'] !== null) {
    ct_fail('elvis then ' . \var_export($elvis['then'] ?? 'MISSING', true));
}
$object = ct_prop($elvis['cond'], 'a', false, 'elvis cond');
ct_param($object, 'u', 'elvis cond param');
$object = ct_prop($elvis['else'], 'b', false, 'elvis else');
ct_param($object, 'u', 'elvis else param');

ct_ternary(ct(fn ($u) => $u->a ? $u->b : $u->c), 'a', 'b', 'c', 'ternary');

// A ternary used as an operand continues into the surrounding expression.
// The optimizer drops the QM_ASSIGN that closes each arm.
$plus = ct_binary(ct(fn ($u) => ($u->a ? $u->b : $u->c) + 1), '+', 'tern plus');
ct_ternary($plus['left'], 'a', 'b', 'c', 'tern plus left');
ct_const($plus['right'], 1, 'tern plus right');

$plus = ct_binary(ct(fn ($u) => 1 + ($u->a ? $u->b : $u->c)), '+', 'plus tern');
ct_const($plus['left'], 1, 'plus tern left');
ct_ternary($plus['right'], 'a', 'b', 'c', 'plus tern right');

$outer = ct(fn ($u) => ($u->a ? $u->b : $u->c) ? $u->d : $u->e);
ct_kind($outer, 'ternary', 'tern cond');
ct_ternary($outer['cond'], 'a', 'b', 'c', 'tern cond cond');
ct_u_prop($outer['then'], 'd', 'tern cond then');
ct_u_prop($outer['else'], 'e', 'tern cond else');

foreach ([
    '+' => fn ($u) => $u->a + $u->b,
    '-' => fn ($u) => $u->a - $u->b,
    '*' => fn ($u) => $u->a * $u->b,
    '/' => fn ($u) => $u->a / $u->b,
    '%' => fn ($u) => $u->a % $u->b,
    '**' => fn ($u) => $u->a ** $u->b,
    '.' => fn ($u) => $u->a . $u->b,
    '&' => fn ($u) => $u->a & $u->b,
    '|' => fn ($u) => $u->a | $u->b,
    '^' => fn ($u) => $u->a ^ $u->b,
    '<<' => fn ($u) => $u->a << $u->b,
    '>>' => fn ($u) => $u->a >> $u->b,
] as $op => $fn) {
    ct_two_props('binary', $op, $fn);
}

ct_unary('!', fn ($u) => !$u->a);
ct_unary('~', fn ($u) => ~$u->a);

ct_cast('int', fn ($u) => (int) $u->a);
ct_cast('string', fn ($u) => (string) $u->a);
ct_cast('float', fn ($u) => (float) $u->a);
ct_cast('bool', fn ($u) => (bool) $u->a);
ct_cast('array', fn ($u) => (array) $u->a);
ct_cast('object', fn ($u) => (object) $u->a);

// === and !== against true, false, and null are TYPE_CHECK. !== is the
// inverse mask.
foreach ([
    ['===', true, fn ($u) => $u->a === true],
    ['===', false, fn ($u) => $u->a === false],
    ['===', null, fn ($u) => $u->a === null],
    ['!==', true, fn ($u) => $u->a !== true],
    ['!==', false, fn ($u) => $u->a !== false],
    ['!==', null, fn ($u) => $u->a !== null],
    ['===', null, fn ($u) => null === $u->a],
    ['!==', null, fn ($u) => null !== $u->a],
    ['===', null, fn ($u) => \is_null($u->a)],
] as [$op, $literal, $fn]) {
    $check = ct_binary(ct($fn), $op, $op . ' ' . \var_export($literal, true));
    ct_left_prop($check, $op . ' ' . \var_export($literal, true));
    ct_const($check['right'], $literal, $op . ' ' . \var_export($literal, true) . ' right');
}

$fold = ct_binary(ct(fn ($u) => $u->a > 1 + 2), '>', 'fold');
ct_left_prop($fold, 'fold');
ct_const($fold['right'], 3, 'fold right');

$negative = ct_binary(ct(fn ($u) => -$u->a), '*', 'unary minus');
ct_left_prop($negative, 'unary minus');
ct_const($negative['right'], -1, 'unary minus right');

$onlyNull = ct(fn ($u) => null);
ct_const($onlyNull, null, 'bare null');

$concat = ct_binary(ct(fn ($u) => $u->a . $u->b), '.', 'concat');
ct_left_prop($concat, 'concat');
ct_right_prop($concat, 'b', 'concat');

echo "ok\n";
--EXPECT--
ok
