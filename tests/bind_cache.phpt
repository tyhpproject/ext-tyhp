--TEST--
bindGet and bindPut intern normalized closures
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/tyhp_type.inc';

interface BindCacheIface {}

enum BindCacheEnum {}

trait BindCacheTrait {}

final class BindCacheToType
{
    public function toType(): \Tyhp\Type
    {
        return \Tyhp\Type::int();
    }
}

final class BindCacheThrowingToType
{
    public function toType(): \Tyhp\Type
    {
        throw new \RuntimeException('toType failed');
    }
}

final class BindCacheWrongToType
{
    public function toType(): int
    {
        return 5;
    }
}

class BindCacheService
{
    public function inst(): int
    {
        return 1;
    }

    public static function stat(): int
    {
        return 2;
    }
}

function show(string $label, bool $ok): void
{
    echo $label, $ok ? " ok\n" : " FAIL\n";
}

$get = new \ReflectionMethod('Tyhp\\Ext', 'bindGet');
$put = new \ReflectionMethod('Tyhp\\Ext', 'bindPut');
$getReturn = $get->getReturnType();
$putReturn = $put->getReturnType();
show(
    'sig',
    (new \ReflectionClass('Tyhp\\Ext'))->isFinal()
        && $get->isStatic()
        && $put->isStatic()
        && $getReturn instanceof \ReflectionNamedType
        && $getReturn->getName() === 'Closure'
        && $getReturn->allowsNull()
        && $putReturn instanceof \ReflectionNamedType
        && $putReturn->getName() === 'void'
        && !$putReturn->allowsNull()
        && \method_exists('Tyhp\\Ext', 'typeIs')
);

$a = static function (): int {
    return 1;
};
$b = static function (): int {
    return 2;
};
$c = static function (): int {
    return 3;
};

show('int singleton', \Tyhp\Type::int() === \Tyhp\Type::int());
show('builtin singleton', \Tyhp\BuiltIn::int->toType() === \Tyhp\Type::int());
show('class singleton', \Tyhp\Type::fromClassName('stdClass') === \Tyhp\Type::fromClassName('stdClass'));

\Tyhp\Ext::bindPut('strlen', [], $a);
show('put get', \Tyhp\Ext::bindGet('strlen', []) === $a);

\Tyhp\Ext::bindPut('strlen', [], $b);
show('replace', \Tyhp\Ext::bindGet('strlen', []) === $b);
show('replace drops first', \Tyhp\Ext::bindGet('strlen', []) !== $a);

\Tyhp\Ext::bindPut('ord', [], $a);
show('other target', \Tyhp\Ext::bindGet('ord', []) === $a);
show('targets differ', \Tyhp\Ext::bindGet('strlen', []) === $b && \Tyhp\Ext::bindGet('ord', []) !== $b);

\Tyhp\Ext::bindPut([\BindCacheService::class, 'stat'], [], $c);
show('static array', \Tyhp\Ext::bindGet([\BindCacheService::class, 'stat'], []) === $c);
show('string method differs', \Tyhp\Ext::bindGet('BindCacheService::stat', []) === null);

$fn = static function (): int {
    return 4;
};
$otherFn = static function (): int {
    return 5;
};
\Tyhp\Ext::bindPut($fn, [], $a);
show('closure target', \Tyhp\Ext::bindGet($fn, []) === $a);
show('closure identity', \Tyhp\Ext::bindGet($otherFn, []) === null);

$service = new \BindCacheService();
\Tyhp\Ext::bindPut([$service, 'inst'], [], $a);
show('instance array', \Tyhp\Ext::bindGet([$service, 'inst'], []) === null);

$fcc = $service->inst(...);
\Tyhp\Ext::bindPut($fcc, [], $a);
show('instance closure', \Tyhp\Ext::bindGet($fcc, []) === null);

$withThis = function (): int {
    return 1;
};
$withThis = $withThis->bindTo($service);
show('bound this', $withThis instanceof \Closure && \Tyhp\Ext::bindGet($withThis, []) === null);
\Tyhp\Ext::bindPut($withThis, [], $a);
show('bound this put', \Tyhp\Ext::bindGet($withThis, []) === null);

$cleared = function (): int {
    return 1;
};
$cleared = $cleared->bindTo(null);
\Tyhp\Ext::bindPut($cleared, [], $b);
show('null this', $cleared instanceof \Closure && \Tyhp\Ext::bindGet($cleared, []) === $b);

$int = \Tyhp\Type::int();
\Tyhp\Ext::bindPut('box', [$int], $a);
show('builtin shares', \Tyhp\Ext::bindGet('box', [\Tyhp\BuiltIn::int]) === $a);
show('totype shares', \Tyhp\Ext::bindGet('box', [new \BindCacheToType()]) === $a);
show('int shares', \Tyhp\Ext::bindGet('box', [\Tyhp\Type::int()]) === $a);

$s1 = \Tyhp\Type::scalar('string');
$s2 = \Tyhp\Type::scalar('string');
show('scalars differ', $s1 !== $s2);
\Tyhp\Ext::bindPut('strings', [$s1], $a);
show('scalar handle', \Tyhp\Ext::bindGet('strings', [$s2]) === null);
show('scalar same', \Tyhp\Ext::bindGet('strings', [$s1]) === $a);

$n1 = new \Tyhp\NamedType('T', $int);
$n2 = new \Tyhp\NamedType('T', $int);
\Tyhp\Ext::bindPut('named', [$n1], $c);
show('named distinct', \Tyhp\Ext::bindGet('named', [$n2]) === null);
show('named same', \Tyhp\Ext::bindGet('named', [$n1]) === $c);

\Tyhp\Ext::bindPut('infer', [null], $a);
show('null arg', \Tyhp\Ext::bindGet('infer', [null]) === $a);
show('null list differs', \Tyhp\Ext::bindGet('infer', []) === null);
show('two nulls differ', \Tyhp\Ext::bindGet('infer', [null, null]) === null);

\Tyhp\Ext::bindPut('cls', [\stdClass::class], $b);
show('class string', \Tyhp\Ext::bindGet('cls', [\Tyhp\Type::fromClassName(\stdClass::class)]) === $b);
show('class string again', \Tyhp\Ext::bindGet('cls', ['stdClass']) === $b);

\Tyhp\Ext::bindPut('iface', [\BindCacheIface::class], $a);
show('interface', \Tyhp\Ext::bindGet('iface', [\Tyhp\Type::fromClassName(\BindCacheIface::class)]) === $a);

\Tyhp\Ext::bindPut('enum', [\BindCacheEnum::class], $c);
show('enum', \Tyhp\Ext::bindGet('enum', [\Tyhp\Type::fromClassName(\BindCacheEnum::class)]) === $c);

\Tyhp\Ext::bindPut('trait', [\BindCacheTrait::class], $a);
show('trait', \Tyhp\Ext::bindGet('trait', [\BindCacheTrait::class]) === null);

\Tyhp\Ext::bindPut('bad object', [new \stdClass()], $a);
show('stdclass arg', \Tyhp\Ext::bindGet('bad object', [new \stdClass()]) === null);

\Tyhp\Ext::bindPut('bad string', ['not-a-class'], $a);
show('unknown string', \Tyhp\Ext::bindGet('bad string', ['not-a-class']) === null);

show('miss', \Tyhp\Ext::bindGet('missing', []) === null);

$classRef = 'stdClass';
show('reference arg', \Tyhp\Ext::bindGet('cls', [&$classRef]) === $b);

$weakClosure = (static function (): \WeakReference {
    $held = static function (): int {
        return 9;
    };
    \Tyhp\Ext::bindPut('held', [], $held);

    return \WeakReference::create($held);
})();
\gc_collect_cycles();
show('map holds closure', $weakClosure->get() instanceof \Closure);
show('map returns held closure', \Tyhp\Ext::bindGet('held', []) === $weakClosure->get());

$replaceOld = (static function (): \WeakReference {
    $old = static function (): int {
        return 10;
    };
    \Tyhp\Ext::bindPut('swap', [], $old);

    return \WeakReference::create($old);
})();
\Tyhp\Ext::bindPut('swap', [], $c);
\gc_collect_cycles();
show('replace releases old closure', $replaceOld->get() === null);

$heldType = new \Tyhp\Type('scalar', 'held');
$heldId = \spl_object_id($heldType);
$weakType = \WeakReference::create($heldType);
\Tyhp\Ext::bindPut('held-type', [$heldType], $a);
unset($heldType);
\gc_collect_cycles();
show('map holds type', $weakType->get() instanceof \Tyhp\Type);
$reused = false;
$fresh = [];
for ($i = 0; $i < 64; $i++) {
    $fresh[] = new \stdClass();
    if (\spl_object_id($fresh[$i]) === $heldId) {
        $reused = true;
    }
}
show('type handle not reused', $reused === false);
show('type slot still matches', \Tyhp\Ext::bindGet('held-type', [$weakType->get()]) === $a);

\Tyhp\Ext::bindPut('thrower', [new \BindCacheThrowingToType()], $a);
show('toType throws', \Tyhp\Ext::bindGet('thrower', [new \BindCacheThrowingToType()]) === null);
\Tyhp\Ext::bindPut('wrong', [new \BindCacheWrongToType()], $a);
show('toType wrong', \Tyhp\Ext::bindGet('wrong', [new \BindCacheWrongToType()]) === null);

\spl_autoload_register(static function (string $class): void {
    if ($class === 'BindCacheAutoloadThrows') {
        throw new \RuntimeException('autoload failed');
    }
});
\Tyhp\Ext::bindPut('autoload', ['BindCacheAutoloadThrows'], $a);
show('autoload throws', \Tyhp\Ext::bindGet('autoload', ['BindCacheAutoloadThrows']) === null);
$mutated = 'BindCache' . \implode('', ['Mut', 'ated']);
\spl_autoload_register(static function (string $class) use (&$mutated): void {
    if ($class === 'BindCacheMutated') {
        \class_alias(\BindCacheService::class, 'BindCacheMutated');
        $mutated = null;
    }
});
\Tyhp\Ext::bindPut('mutated', [&$mutated], $a);
show('autoload rewrites ref', \Tyhp\Ext::bindGet('mutated', ['BindCacheMutated']) === $a);

$threw = false;
try {
    \Tyhp\Ext::bindPut('nope', [new \stdClass()], $a);
    \Tyhp\Ext::bindPut('nope', ['not-a-class'], $a);
    \Tyhp\Ext::bindGet('nope', [new \stdClass()]);
    \Tyhp\Ext::bindGet([$service, 'inst'], [null]);
} catch (\Throwable $e) {
    $threw = true;
}
show('no throw', $threw === false);
?>
--EXPECT--
sig ok
int singleton ok
builtin singleton ok
class singleton ok
put get ok
replace ok
replace drops first ok
other target ok
targets differ ok
static array ok
string method differs ok
closure target ok
closure identity ok
instance array ok
instance closure ok
bound this ok
bound this put ok
null this ok
builtin shares ok
totype shares ok
int shares ok
scalars differ ok
scalar handle ok
scalar same ok
named distinct ok
named same ok
null arg ok
null list differs ok
two nulls differ ok
class string ok
class string again ok
interface ok
enum ok
trait ok
stdclass arg ok
unknown string ok
miss ok
reference arg ok
map holds closure ok
map returns held closure ok
replace releases old closure ok
map holds type ok
type handle not reused ok
type slot still matches ok
toType throws ok
toType wrong ok
autoload throws ok
autoload rewrites ref ok
no throw ok
