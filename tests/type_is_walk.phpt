--TEST--
typeIs walks class, union, intersection, generic, alias, and lazy types
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/tyhp_type.inc';

class Animal {}
class Cat extends Animal {}
interface Speaks {}
interface Named {}
class Parrot extends Animal implements Speaks, Named {}
class Statue implements Named {}
enum Suit { case Hearts; }

class Box
{
    public ?object $__tyhpGeneric = null;
}

class GenericBag
{
    public function __construct(public object $type) {}

    public function objectType(): object
    {
        return $this->type;
    }
}

class HiddenType
{
    public function __construct(
        private string $kind,
        private ?string $name = null,
        private array $typeArgs = [],
        private bool $isNullable = false,
        private array $structFields = [],
        private array $structRequiredKeys = [],
        private ?object $forward = null,
        private ?object $lazyInner = null,
        private bool $resolving = false,
        private string $lazyOp = '',
        private ?string $lazyName = null,
        private bool $placeholder = false,
    ) {}
}

// Private properties declared on the parent, as Tyhp\NamedType reads them on Tyhp\Type.
class HiddenChild extends HiddenType {}

$animal = new Tyhp\Type('class', Animal::class);
$cat = new Tyhp\Type('class', Cat::class);
$speaks = new Tyhp\Type('class', Speaks::class);
$named = new Tyhp\Type('class', Named::class);
$suit = new Tyhp\Type('class', Suit::class);
$std = new Tyhp\Type('class', '\stdClass');
$both = new Tyhp\Type('intersection', typeArgs: [$speaks, $named]);
$union = new Tyhp\Type('union', typeArgs: [
    Tyhp\Type::scalar('int'),
    Tyhp\Type::scalar('string'),
]);
$list = new Tyhp\Type('generic', 'array', typeArgs: [
    new Tyhp\NamedType('TValue', Tyhp\Type::scalar('int')),
]);
$map = new Tyhp\Type('generic', 'array', typeArgs: [
    Tyhp\Type::scalar('string'),
    Tyhp\Type::scalar('int'),
]);
$iterable = new Tyhp\Type('generic', 'iterable', typeArgs: [
    Tyhp\Type::scalar('int'),
]);
$boxInt = new Tyhp\Type('generic', Box::class, typeArgs: [Tyhp\Type::scalar('int')]);
$boxString = new Tyhp\Type('generic', Box::class, typeArgs: [Tyhp\Type::scalar('string')]);
$alias = new Tyhp\Type('ref', 'UserId', forward: Tyhp\Type::scalar('int'));
$lazyNull = new Tyhp\Type('lazy', lazyInner: Tyhp\Type::scalar('string'), lazyOp: 'nullable');
$lazyName = new Tyhp\Type('lazy', lazyInner: Tyhp\Type::scalar('int'), lazyOp: 'name', lazyName: 'UserId');
$lazyReadOnly = new Tyhp\Type('lazy', lazyInner: Tyhp\Type::scalar('bool'), lazyOp: 'readonly');
$innerNullable = Tyhp\Type::scalar('string', isNullable: true);
$lazyStrip = new Tyhp\Type('lazy', lazyInner: $innerNullable, lazyOp: 'nonNullable');
$shape = new Tyhp\Type('callableShape', 'fn');
$hidden = new HiddenType('scalar', 'bool');
$hiddenChild = new HiddenChild('scalar', 'bool');
$namedString = new Tyhp\NamedType('TValue', Tyhp\Type::scalar('string'));
$namedNullable = new Tyhp\NamedType('TValue', Tyhp\Type::scalar('int', isNullable: true));
$cycleA = new Tyhp\Type('ref', 'A');
$cycleB = new Tyhp\Type('ref', 'B');
$cycleA->forward = $cycleB;
$cycleB->forward = $cycleA;
$self = Tyhp\Type::scalar('int');
$self->forward = $self;
$node = new Tyhp\Type('struct', structRequiredKeys: ['child']);
$node->structFields = ['child' => $node];
$loop = [];
$loop['child'] = &$loop;

$box = new Box();
$box->__tyhpGeneric = new GenericBag($boxInt);
$other = new Box();
$other->__tyhpGeneric = new GenericBag($boxString);
$plain = new Box();

\Tyhp\show('subclass', \Tyhp\Ext::typeIs($animal, new Cat()));
\Tyhp\show('parent', \Tyhp\Ext::typeIs($cat, new Animal()));
\Tyhp\show('interface', \Tyhp\Ext::typeIs($speaks, new Parrot()));
\Tyhp\show('interface fail', \Tyhp\Ext::typeIs($speaks, new Cat()));
\Tyhp\show('enum', \Tyhp\Ext::typeIs($suit, Suit::Hearts));
\Tyhp\show('leading slash', \Tyhp\Ext::typeIs($std, new stdClass()));
\Tyhp\show('intersection', \Tyhp\Ext::typeIs($both, new Parrot()));
\Tyhp\show('intersection partial', \Tyhp\Ext::typeIs($both, new Statue()));
\Tyhp\show('intersection value', \Tyhp\Ext::typeIs($both, 1));
\Tyhp\show('union int', \Tyhp\Ext::typeIs($union, 1));
\Tyhp\show('union string', \Tyhp\Ext::typeIs($union, 'a'));
\Tyhp\show('union fail', \Tyhp\Ext::typeIs($union, []));
\Tyhp\show('list', \Tyhp\Ext::typeIs($list, [1, 2]));
\Tyhp\show('list fail', \Tyhp\Ext::typeIs($list, [1, 'a']));
\Tyhp\show('list empty', \Tyhp\Ext::typeIs($list, []));
\Tyhp\show('map', \Tyhp\Ext::typeIs($map, ['a' => 1]));
\Tyhp\show('map key', \Tyhp\Ext::typeIs($map, [0 => 1]));
\Tyhp\show('iterator', \Tyhp\Ext::typeIs($iterable, new ArrayIterator([1, 2])));
\Tyhp\show('iterator fail', \Tyhp\Ext::typeIs($iterable, new ArrayIterator([1, 'a'])));
\Tyhp\show('generic class', \Tyhp\Ext::typeIs($boxInt, $plain));
\Tyhp\show('generic recorded', \Tyhp\Ext::typeIs($boxInt, $box));
\Tyhp\show('generic mismatch', \Tyhp\Ext::typeIs($boxInt, $other));
\Tyhp\show('generic value', \Tyhp\Ext::typeIs($boxInt, new stdClass()));
\Tyhp\show('alias', \Tyhp\Ext::typeIs($alias, 4));
\Tyhp\show('alias fail', \Tyhp\Ext::typeIs($alias, '4'));
\Tyhp\show('lazy null', \Tyhp\Ext::typeIs($lazyNull, null));
\Tyhp\show('lazy string', \Tyhp\Ext::typeIs($lazyNull, 'a'));
\Tyhp\show('lazy fail', \Tyhp\Ext::typeIs($lazyNull, 2));
\Tyhp\show('lazy name', \Tyhp\Ext::typeIs($lazyName, 1));
\Tyhp\show('lazy name fail', \Tyhp\Ext::typeIs($lazyName, '1'));
\Tyhp\show('lazy readonly', \Tyhp\Ext::typeIs($lazyReadOnly, false));
\Tyhp\show('lazy strip null', \Tyhp\Ext::typeIs($lazyStrip, null));
\Tyhp\show('lazy strip string', \Tyhp\Ext::typeIs($lazyStrip, 'a'));
\Tyhp\show('callable shape', \Tyhp\Ext::typeIs($shape, strlen(...)));
\Tyhp\show('callable shape fail', \Tyhp\Ext::typeIs($shape, 1));
\Tyhp\show('private', \Tyhp\Ext::typeIs($hidden, true));
\Tyhp\show('private fail', \Tyhp\Ext::typeIs($hidden, 1));
\Tyhp\show('private subclass', \Tyhp\Ext::typeIs($hiddenChild, true));
\Tyhp\show('private subclass fail', \Tyhp\Ext::typeIs($hiddenChild, 1));
\Tyhp\show('named', \Tyhp\Ext::typeIs($namedString, 'a'));
\Tyhp\show('named fail', \Tyhp\Ext::typeIs($namedString, 1));
\Tyhp\show('named null', \Tyhp\Ext::typeIs($namedNullable, null));
\Tyhp\show('cycle', \Tyhp\Ext::typeIs($cycleA, 1));
\Tyhp\show('self forward', \Tyhp\Ext::typeIs($self, 1));
\Tyhp\show('recursive value', \Tyhp\Ext::typeIs($node, $loop));
?>
--EXPECT--
subclass=true
parent=false
interface=true
interface fail=false
enum=true
leading slash=true
intersection=true
intersection partial=false
intersection value=false
union int=true
union string=true
union fail=false
list=true
list fail=false
list empty=true
map=true
map key=false
iterator=true
iterator fail=false
generic class=true
generic recorded=true
generic mismatch=false
generic value=false
alias=true
alias fail=false
lazy null=true
lazy string=true
lazy fail=false
lazy name=true
lazy name fail=false
lazy readonly=true
lazy strip null=false
lazy strip string=true
callable shape=true
callable shape fail=false
private=true
private fail=false
private subclass=true
private subclass fail=false
named=true
named fail=false
named null=true
cycle=null
self forward=null
recursive value=null
