--TEST--
an unknown host is unresolved with DnsLookup's message
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/dns.inc';

$host = 'tyhp-ext-does-not-exist.invalid';
$started = \Tyhp\Ext::dnsStart($host);
tyhp_dns_wait($started['pipe']);
$result = \Tyhp\Ext::dnsTake($started['id']);
$expected = 'Could not resolve host tyhp-ext-does-not-exist.invalid.';

if (($result['ok'] ?? null) !== false || ($result['reason'] ?? null) !== 'unresolved' || ($result['message'] ?? null) !== $expected) {
    echo "FAIL unresolved\n";
    echo "expected message=$expected\n";
    \var_export($result);
    echo "\n";
    exit(1);
}
echo "ok\n";
?>
--EXPECT--
ok
