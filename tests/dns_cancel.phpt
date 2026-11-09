--TEST--
dnsCancel before dnsTake reports cancelled
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/dns.inc';

$started = \Tyhp\Ext::dnsStart('localhost');
if (!\is_int($started['id'] ?? null) || !\is_resource($started['pipe'] ?? null)) {
    echo "FAIL start\n";
    \var_export($started);
    echo "\n";
    exit(1);
}

\Tyhp\Ext::dnsCancel($started['id']);
tyhp_dns_wait($started['pipe']);
$result = \Tyhp\Ext::dnsTake($started['id']);

if (($result['ok'] ?? null) !== false || ($result['reason'] ?? null) !== 'cancelled') {
    echo "FAIL cancel\n";
    \var_export($result);
    echo "\n";
    exit(1);
}
if (\array_key_exists('addresses', $result)) {
    echo "FAIL cancel returned addresses\n";
    \var_export($result);
    echo "\n";
    exit(1);
}
if (!\is_string($result['message'] ?? null)) {
    echo "FAIL cancel message\n";
    \var_export($result);
    echo "\n";
    exit(1);
}

\Tyhp\Ext::dnsCancel($started['id']);
echo "ok\n";
?>
--EXPECT--
ok
