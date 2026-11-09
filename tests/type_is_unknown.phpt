--TEST--
typeIs returns null for a kind it does not walk
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/tyhp_type.inc';

$unknown = new Tyhp\Type('not-a-type', 'Widget');
$shape = new Tyhp\Type('objectShape', 'Bag');
$alias = new Tyhp\Type('alias', 'UserId');
$placeholder = new Tyhp\Type('ref', 'UserId', placeholder: true);
$lazyPlaceholder = new Tyhp\Type('lazy', 'UserId', lazyInner: $placeholder, lazyOp: 'nullable');
$union = new Tyhp\Type('union', typeArgs: [
    Tyhp\Type::scalar('int'),
    new Tyhp\Type('objectShape', 'Bag'),
]);

\Tyhp\show('unknown', \Tyhp\Ext::typeIs($unknown, 1));
\Tyhp\show('object shape', \Tyhp\Ext::typeIs($shape, new stdClass()));
\Tyhp\show('alias kind', \Tyhp\Ext::typeIs($alias, 1));
\Tyhp\show('unresolved ref', \Tyhp\Ext::typeIs($placeholder, 1));
\Tyhp\show('lazy over unresolved ref', \Tyhp\Ext::typeIs($lazyPlaceholder, 1));
\Tyhp\show('union known arm', \Tyhp\Ext::typeIs($union, 1));
\Tyhp\show('union unknown arm', \Tyhp\Ext::typeIs($union, 'x'));
?>
--EXPECT--
unknown=null
object shape=null
alias kind=null
unresolved ref=null
lazy over unresolved ref=null
union known arm=true
union unknown arm=null
