--TEST--
dnsStart on localhost matches the DnsLookup address list
--EXTENSIONS--
tyhp
--SKIPIF--
<?php
require __DIR__ . '/dns.inc';
$oracle = tyhp_dns_oracle('localhost');
if ($oracle !== []) {
    return;
}
$started = \Tyhp\Ext::dnsStart('localhost');
tyhp_dns_wait($started['pipe']);
$result = \Tyhp\Ext::dnsTake($started['id']);
if (($result['ok'] ?? null) === false && ($result['reason'] ?? null) === 'unresolved') {
    die('skip localhost does not resolve');
}
?>
--FILE--
<?php
require __DIR__ . '/dns.inc';

function tyhp_dns_check(string $host): void
{
    $started = \Tyhp\Ext::dnsStart($host);
    if (!\is_int($started['id'] ?? null)) {
        echo "FAIL $host id\n";
        \var_export($started);
        echo "\n";
        exit(1);
    }
    if (!\is_resource($started['pipe'] ?? null) || \get_resource_type($started['pipe']) !== 'stream') {
        echo "FAIL $host pipe\n";
        \var_export($started);
        echo "\n";
        exit(1);
    }

    tyhp_dns_wait($started['pipe']);
    $result = \Tyhp\Ext::dnsTake($started['id']);
    $oracle = tyhp_dns_oracle($host);

    if ($oracle === [] && ($result['ok'] ?? null) === false && ($result['reason'] ?? null) === 'unresolved') {
        echo "FAIL $host both unresolved\n";
        exit(1);
    }
    if (($result['ok'] ?? null) !== true || ($result['addresses'] ?? null) !== $oracle) {
        echo "FAIL $host\n";
        echo "extension=";
        \var_export($result);
        echo "\noracle=";
        \var_export($oracle);
        echo "\n";
        exit(1);
    }
    if (!\array_is_list($result['addresses'])) {
        echo "FAIL $host addresses are not a list\n";
        exit(1);
    }

    \Tyhp\Ext::dnsCancel($started['id']);
    try {
        \Tyhp\Ext::dnsTake($started['id']);
        echo "FAIL $host second take did not throw\n";
        exit(1);
    } catch (\InvalidArgumentException $e) {
    }
}

tyhp_dns_check('localhost');
tyhp_dns_check('127.0.0.1');
tyhp_dns_check('::1');
echo "ok\n";
?>
--EXPECT--
ok
