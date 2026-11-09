--TEST--
typeIs walks a struct payload, including nested fields and required keys
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/tyhp_type.inc';

$address = new Tyhp\Type(
    'struct',
    'Address',
    structFields: [
        'city' => Tyhp\Type::scalar('string'),
    ],
    structRequiredKeys: ['city'],
);
$user = new Tyhp\Type(
    'struct',
    'User',
    structFields: [
        'id' => Tyhp\Type::scalar('int'),
        'name' => Tyhp\Type::scalar('string'),
        'address' => $address,
    ],
    structRequiredKeys: ['id', 'name'],
);
$numeric = new Tyhp\Type(
    'struct',
    structFields: [
        '1' => Tyhp\Type::scalar('string'),
    ],
    structRequiredKeys: ['1'],
);

\Tyhp\show('payload', \Tyhp\Ext::typeIs($user, [
    'id' => 7,
    'name' => 'Ada',
    'address' => ['city' => 'London'],
]));
\Tyhp\show('optional omitted', \Tyhp\Ext::typeIs($user, [
    'id' => 7,
    'name' => 'Ada',
]));
\Tyhp\show('extra key', \Tyhp\Ext::typeIs($user, [
    'id' => 7,
    'name' => 'Ada',
    'extra' => true,
]));
\Tyhp\show('nested fail', \Tyhp\Ext::typeIs($user, [
    'id' => 7,
    'name' => 'Ada',
    'address' => ['city' => 1],
]));
\Tyhp\show('nested missing', \Tyhp\Ext::typeIs($user, [
    'id' => 7,
    'name' => 'Ada',
    'address' => [],
]));
\Tyhp\show('missing required', \Tyhp\Ext::typeIs($user, [
    'id' => 7,
]));
\Tyhp\show('wrong id', \Tyhp\Ext::typeIs($user, [
    'id' => '7',
    'name' => 'Ada',
]));
\Tyhp\show('not array', \Tyhp\Ext::typeIs($user, 'Ada'));
\Tyhp\show('numeric key', \Tyhp\Ext::typeIs($numeric, [1 => 'a']));
\Tyhp\show('numeric missing', \Tyhp\Ext::typeIs($numeric, []));
?>
--EXPECT--
payload=true
optional omitted=true
extra key=true
nested fail=false
nested missing=false
missing required=false
wrong id=false
not array=false
numeric key=true
numeric missing=false
