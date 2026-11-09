--TEST--
closureTree reports operands in source order
--EXTENSIONS--
tyhp
--INI--
opcache.file_update_protection=0
--FILE--
<?php
require __DIR__ . '/closure_tree.inc';

// Both operands are computed, so the order they were computed in is the order
// they were written in. Each operator is checked with the operands written
// both ways round: a tree that swapped the sides would fail one of the two.
ct_sides('<', fn ($u) => $u->a < $u->b, 'a', 'b');
ct_sides('<', fn ($u) => $u->b < $u->a, 'b', 'a');
ct_sides('>', fn ($u) => $u->a > $u->b, 'a', 'b');
ct_sides('>', fn ($u) => $u->b > $u->a, 'b', 'a');
ct_sides('<=', fn ($u) => $u->a <= $u->b, 'a', 'b');
ct_sides('<=', fn ($u) => $u->b <= $u->a, 'b', 'a');
ct_sides('>=', fn ($u) => $u->a >= $u->b, 'a', 'b');
ct_sides('>=', fn ($u) => $u->b >= $u->a, 'b', 'a');
ct_sides('==', fn ($u) => $u->a == $u->b, 'a', 'b');
ct_sides('==', fn ($u) => $u->b == $u->a, 'b', 'a');
ct_sides('!=', fn ($u) => $u->a != $u->b, 'a', 'b');
ct_sides('!=', fn ($u) => $u->b != $u->a, 'b', 'a');
ct_sides('===', fn ($u) => $u->a === $u->b, 'a', 'b');
ct_sides('===', fn ($u) => $u->b === $u->a, 'b', 'a');
ct_sides('!==', fn ($u) => $u->a !== $u->b, 'a', 'b');
ct_sides('!==', fn ($u) => $u->b !== $u->a, 'b', 'a');
ct_sides('<=>', fn ($u) => $u->a <=> $u->b, 'a', 'b');
ct_sides('<=>', fn ($u) => $u->b <=> $u->a, 'b', 'a');
ct_sides('*', fn ($u) => $u->a * $u->b, 'a', 'b');
ct_sides('*', fn ($u) => $u->b * $u->a, 'b', 'a');
ct_sides('&', fn ($u) => $u->a & $u->b, 'a', 'b');
ct_sides('&', fn ($u) => $u->b & $u->a, 'b', 'a');
ct_sides('|', fn ($u) => $u->a | $u->b, 'a', 'b');
ct_sides('|', fn ($u) => $u->b | $u->a, 'b', 'a');
ct_sides('^', fn ($u) => $u->a ^ $u->b, 'a', 'b');
ct_sides('^', fn ($u) => $u->b ^ $u->a, 'b', 'a');
ct_sides('xor', fn ($u) => $u->a xor $u->b, 'a', 'b');
ct_sides('xor', fn ($u) => $u->b xor $u->a, 'b', 'a');
ct_sides('-', fn ($u) => $u->b - $u->a, 'b', 'a');
ct_sides('.', fn ($u) => $u->b . $u->a, 'b', 'a');

// A computed operand next to a computed value of another type (an addition's
// result is a TMP, a property fetch can be a VAR before the optimizer).
$left = ct_binary(ct(fn ($u) => $u->b + 1 === $u->a), '===', 'add left');
ct_binary($left['left'], '+', 'add left binary');
ct_u_prop($left['right'], 'a', 'add left right');
$right = ct_binary(ct(fn ($u) => $u->a === $u->b + 1), '===', 'add right');
ct_u_prop($right['left'], 'a', 'add right left');
ct_binary($right['right'], '+', 'add right binary');

// A call result is computed too.
$call = ct_binary(ct(fn ($u) => $u->m(1) < $u->a), '<', 'call left');
ct_kind($call['left'], 'call', 'call left call');
ct_u_prop($call['right'], 'a', 'call left prop');
$call = ct_binary(ct(fn ($u) => $u->a > $u->m(1)), '>', 'call right');
ct_u_prop($call['left'], 'a', 'call right prop');
ct_kind($call['right'], 'call', 'call right call');

// A literal or capture next to a computed value. The opcode does not record
// which side was written first, so the computed operand is the left one.
$n = 5;
foreach ([
    ['<', fn ($u) => $u->a < 1],
    ['>', fn ($u) => $u->a > 1],
    ['<=', fn ($u) => $u->a <= 1],
    ['>=', fn ($u) => $u->a >= 1],
    ['==', fn ($u) => $u->a == 1],
    ['==', fn ($u) => 1 == $u->a],
    ['!=', fn ($u) => 1 != $u->a],
    ['===', fn ($u) => 1 === $u->a],
    ['!==', fn ($u) => 1 !== $u->a],
    ['*', fn ($u) => 2 * $u->a],
    ['>', fn ($u) => 1 < $u->a],
    ['>=', fn ($u) => 1 <= $u->a],
] as [$op, $fn]) {
    $node = ct_binary(ct($fn), $op, $op . ' literal');
    ct_left_prop($node, $op . ' literal');
    $literal = ct_kind($node['right'], 'const', $op . ' literal right');
}
foreach ([
    ['<', fn ($u) => $u->a < $n],
    ['>', fn ($u) => $u->a > $n],
    ['<=', fn ($u) => $u->a <= $n],
    ['>=', fn ($u) => $u->a >= $n],
    ['==', fn ($u) => $u->a == $n],
    ['==', fn ($u) => $n == $u->a],
    ['!==', fn ($u) => $u->a !== $n],
    ['*', fn ($u) => $u->a * $n],
    ['*', fn ($u) => $n * $u->a],
] as [$op, $fn]) {
    $node = ct_binary(ct($fn), $op, $op . ' capture');
    ct_left_prop($node, $op . ' capture');
    $capture = ct_kind($node['right'], 'capture', $op . ' capture right');
    if ($capture['name'] !== 'n' || $capture['value'] !== 5) {
        ct_fail($op . ' capture value ' . \var_export($capture, true));
    }
}

echo "ok\n";
--EXPECT--
ok
