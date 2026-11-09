--TEST--
lookups left untaken at the end of the request do not hang or crash shutdown
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/dns.inc';

$kept = [];
foreach (['localhost', '127.0.0.1', 'tyhp-ext-does-not-exist.invalid'] as $host) {
    $kept[] = \Tyhp\Ext::dnsStart($host);
}

/* One cancelled, one left alone, one with its pipe closed. */
\Tyhp\Ext::dnsCancel($kept[0]['id']);
\fclose($kept[2]['pipe']);

/* A lookup that finished and was taken is gone; the others stay pending. */
tyhp_dns_wait($kept[1]['pipe']);
$taken = \Tyhp\Ext::dnsTake($kept[1]['id']);
if (($taken['ok'] ?? null) !== true) {
    echo "FAIL take\n";
    exit(1);
}

echo "ok\n";
?>
--EXPECT--
ok
