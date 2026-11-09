--TEST--
typeParse returns a graph or null
--EXTENSIONS--
tyhp
--FILE--
<?php

function scalar(string $name, bool $nullable = false): array
{
    return ['kind' => 'scalar', 'name' => $name, 'nullable' => $nullable];
}

function name_of(string $name, bool $nullable = false, array $args = []): array
{
    return ['kind' => 'unresolved', 'form' => 'name', 'name' => $name, 'nullable' => $nullable, 'args' => $args];
}

function shape(string $form, string $name, bool $nullable = false, array $args = []): array
{
    return ['kind' => 'unresolved', 'form' => $form, 'name' => $name, 'nullable' => $nullable, 'args' => $args];
}

function arg(string $name, array $type): array
{
    return ['name' => $name, 'type' => $type];
}

function generic(string $name, array $args, bool $nullable = false): array
{
    return ['kind' => 'generic', 'name' => $name, 'nullable' => $nullable, 'args' => $args];
}

function union(array $members, bool $nullable = false): array
{
    return ['kind' => 'union', 'nullable' => $nullable, 'members' => $members];
}

function intersection(array $members, bool $nullable = false): array
{
    return ['kind' => 'intersection', 'nullable' => $nullable, 'members' => $members];
}

function expect(string $label, string $spelling, mixed $expected): void
{
    $got = \Tyhp\Ext::typeParse($spelling);
    if ($got === $expected) {
        echo $label, " ok\n";
        return;
    }
    echo $label, " FAIL\n";
    \var_export($got);
    echo "\n";
}

$parse = new \ReflectionMethod('Tyhp\\Ext', 'typeParse');
$parseReturn = $parse->getReturnType();
echo $parse->isStatic()
    && $parseReturn instanceof \ReflectionNamedType
    && $parseReturn->getName() === 'array'
    && $parseReturn->allowsNull()
    && \method_exists('Tyhp\\Ext', 'typeIs')
    ? "sig ok\n"
    : "sig FAIL\n";

$int = scalar('int');
$string = scalar('string');
$nullableInt = scalar('int', true);

expect('int', 'int', $int);
expect('INT', 'INT', $int);
expect('?string', '?string', scalar('string', true));
expect('int|null', 'int|null', $nullableInt);
expect('?int', '?int', $nullableInt);
expect('whitespace', ' int | null ', $nullableInt);
expect('string|int', 'string|int', union([$string, $int]));
expect('int|string|null', 'int|string|null', union([$int, $string], true));
expect('A&B', 'A&B', intersection([name_of('A'), name_of('B')]));
expect('?A&B', '?A&B', intersection([name_of('A', true), name_of('B')]));
expect('?(A&B)', '?(A&B)', intersection([name_of('A'), name_of('B')], true));
expect('array<int>', 'array<int>', generic('array', [arg('TValue', $int)]));
expect('array<int, string>', 'array<int, string>', generic('array', [arg('TKey', $int), arg('TValue', $string)]));
expect('iterable<int>', 'iterable<int>', generic('iterable', [arg('TValue', $int)]));
expect('iterable<int, string>', 'iterable<int, string>', generic('iterable', [arg('TKey', $int), arg('TValue', $string)]));
expect('array', 'array', scalar('array'));
expect('Foo', 'Foo', name_of('Foo'));
expect('\\Foo', '\\Foo', name_of('Foo'));
expect('\\\\Foo', '\\\\Foo', name_of('Foo'));
expect('Foo<int, string>', 'Foo<int, string>', name_of('Foo', false, [arg('T0', $int), arg('T1', $string)]));
expect('struct Address', 'struct Address', shape('struct', 'Address'));
expect('struct Address<int>', 'struct Address<int>', shape('struct', 'Address', false, [$int]));
expect('object Point', 'object Point', shape('object', 'Point'));
expect('callable Handler', 'callable Handler', shape('callable', 'Handler'));
expect('decimal', 'decimal', ['kind' => 'class', 'name' => 'Tyhp\\Decimal', 'nullable' => false]);
expect('true', 'true', scalar('bool'));
expect('false', 'false', scalar('bool'));
expect('null', 'null', scalar('null', true));
expect('?null', '?null', scalar('null', true));
expect('null|null', 'null|null', scalar('null', true));
expect('mixed', 'mixed', scalar('mixed'));
expect('?mixed', '?mixed', scalar('mixed'));
expect('mixed|null', 'mixed|null', scalar('mixed'));
expect('\\INT', '\\INT', $int);
expect('object', 'object', scalar('object'));
expect('callable', 'callable', scalar('callable'));
expect('struct alone', 'struct', name_of('struct'));
expect('?iterable', '?iterable', scalar('iterable', true));
expect('?int|string', '?int|string', union([$int, $string], true));
expect('(A|B)|C', '(A|B)|C', union([name_of('A'), name_of('B'), name_of('C')]));
expect('(A&B)&C', '(A&B)&C', intersection([name_of('A'), name_of('B'), name_of('C')]));
expect('array<?int>', 'array<?int>', generic('array', [arg('TValue', $nullableInt)]));
expect('array<int>|null', 'array<int>|null', generic('array', [arg('TValue', $int)], true));
expect('?Foo<int>', '?Foo<int>', name_of('Foo', true, [arg('T0', $int)]));
expect('array empty args', 'array<>', null);
expect('Foo empty args', 'Foo<>', null);
expect('true args', 'true<int>', null);
expect('decimal args', 'decimal<int>', null);
expect('empty', '', null);
expect('whitespace only', " \t", null);
expect('int|', 'int|', null);
expect('array three', 'array<int,string,bool>', null);
expect('int args', 'int<string>', null);
expect('missing gt', 'array<int', null);
expect('paren empty', '()', null);
expect('bad token', 'int@', null);
expect('trailing', 'int int', null);

$sameLeft = \Tyhp\Ext::typeParse('int|null');
$sameRight = \Tyhp\Ext::typeParse('?int');
echo $sameLeft === $sameRight ? "same nullable ok\n" : "same nullable FAIL\n";

$foo = \Tyhp\Ext::typeParse('Foo');
$fooSlash = \Tyhp\Ext::typeParse('\\Foo');
echo $foo === $fooSlash ? "slash ok\n" : "slash FAIL\n";

$depth = 'int';
for ($i = 0; $i < 128; $i++) {
    $depth = 'Foo<' . $depth . '>';
}
echo \is_array(\Tyhp\Ext::typeParse($depth)) ? "depth 128 ok\n" : "depth 128 FAIL\n";
$depth = 'Foo<' . $depth . '>';
echo \Tyhp\Ext::typeParse($depth) === null ? "depth 129 ok\n" : "depth 129 FAIL\n";
?>
--EXPECT--
sig ok
int ok
INT ok
?string ok
int|null ok
?int ok
whitespace ok
string|int ok
int|string|null ok
A&B ok
?A&B ok
?(A&B) ok
array<int> ok
array<int, string> ok
iterable<int> ok
iterable<int, string> ok
array ok
Foo ok
\Foo ok
\\Foo ok
Foo<int, string> ok
struct Address ok
struct Address<int> ok
object Point ok
callable Handler ok
decimal ok
true ok
false ok
null ok
?null ok
null|null ok
mixed ok
?mixed ok
mixed|null ok
\INT ok
object ok
callable ok
struct alone ok
?iterable ok
?int|string ok
(A|B)|C ok
(A&B)&C ok
array<?int> ok
array<int>|null ok
?Foo<int> ok
array empty args ok
Foo empty args ok
true args ok
decimal args ok
empty ok
whitespace only ok
int| ok
array three ok
int args ok
missing gt ok
paren empty ok
bad token ok
trailing ok
same nullable ok
slash ok
depth 128 ok
depth 129 ok
