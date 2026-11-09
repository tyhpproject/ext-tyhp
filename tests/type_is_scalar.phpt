--TEST--
typeIs matches and rejects scalar kinds
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/tyhp_type.inc';

$int = Tyhp\Type::scalar('int');
$float = Tyhp\Type::scalar('float');
$string = Tyhp\Type::scalar('string');
$null = Tyhp\Type::scalar('null');
$void = Tyhp\Type::scalar('void');
$mixed = Tyhp\Type::scalar('mixed');
$never = Tyhp\Type::scalar('never');
$bool = Tyhp\Type::scalar('bool');
$array = Tyhp\Type::scalar('array');
$object = Tyhp\Type::scalar('object');
$callable = Tyhp\Type::scalar('callable');
$iterable = Tyhp\Type::scalar('iterable');
$resource = Tyhp\Type::scalar('resource');
$nullableString = Tyhp\Type::scalar('string', isNullable: true);
$unknownScalar = Tyhp\Type::scalar('decimal');

\Tyhp\show('method', method_exists('Tyhp\\Ext', 'typeIs'));
\Tyhp\show('ext class', (new ReflectionClass('Tyhp\\Ext'))->isFinal());
\Tyhp\show('int match', \Tyhp\Ext::typeIs($int, 1));
\Tyhp\show('int fail', \Tyhp\Ext::typeIs($int, '1'));
\Tyhp\show('float int', \Tyhp\Ext::typeIs($float, 1));
\Tyhp\show('float num', \Tyhp\Ext::typeIs($float, 1.5));
\Tyhp\show('float string', \Tyhp\Ext::typeIs($float, '1.5'));
\Tyhp\show('string', \Tyhp\Ext::typeIs($string, 'a'));
\Tyhp\show('bool', \Tyhp\Ext::typeIs($bool, false));
\Tyhp\show('bool int', \Tyhp\Ext::typeIs($bool, 0));
\Tyhp\show('null', \Tyhp\Ext::typeIs($null, null));
\Tyhp\show('null fail', \Tyhp\Ext::typeIs($null, 0));
\Tyhp\show('void', \Tyhp\Ext::typeIs($void, null));
\Tyhp\show('void fail', \Tyhp\Ext::typeIs($void, 0));
\Tyhp\show('mixed', \Tyhp\Ext::typeIs($mixed, null));
\Tyhp\show('never', \Tyhp\Ext::typeIs($never, null));
\Tyhp\show('array', \Tyhp\Ext::typeIs($array, []));
\Tyhp\show('array fail', \Tyhp\Ext::typeIs($array, new stdClass()));
\Tyhp\show('object', \Tyhp\Ext::typeIs($object, new stdClass()));
\Tyhp\show('callable', \Tyhp\Ext::typeIs($callable, strlen(...)));
\Tyhp\show('callable string', \Tyhp\Ext::typeIs($callable, 'strlen'));
\Tyhp\show('callable fail', \Tyhp\Ext::typeIs($callable, 1));
\Tyhp\show('iterable', \Tyhp\Ext::typeIs($iterable, []));
\Tyhp\show('iterable object', \Tyhp\Ext::typeIs($iterable, new ArrayIterator([])));
\Tyhp\show('iterable fail', \Tyhp\Ext::typeIs($iterable, 1));
\Tyhp\show('nullable null', \Tyhp\Ext::typeIs($nullableString, null));
\Tyhp\show('nullable string', \Tyhp\Ext::typeIs($nullableString, 'a'));
\Tyhp\show('nullable fail', \Tyhp\Ext::typeIs($nullableString, 1));
\Tyhp\show('unknown scalar name', \Tyhp\Ext::typeIs($unknownScalar, 1));

$handle = fopen('php://memory', 'r');
\Tyhp\show('resource', \Tyhp\Ext::typeIs($resource, $handle));
fclose($handle);
\Tyhp\show('resource closed', \Tyhp\Ext::typeIs($resource, $handle));
\Tyhp\show('resource fail', \Tyhp\Ext::typeIs($resource, 1));
?>
--EXPECT--
method=true
ext class=true
int match=true
int fail=false
float int=true
float num=true
float string=false
string=true
bool=true
bool int=false
null=true
null fail=false
void=true
void fail=false
mixed=true
never=false
array=true
array fail=false
object=true
callable=true
callable string=true
callable fail=false
iterable=true
iterable object=true
iterable fail=false
nullable null=true
nullable string=true
nullable fail=false
unknown scalar name=false
resource=true
resource closed=true
resource fail=false
