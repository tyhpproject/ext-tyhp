--TEST--
dnsStart treats odd host spellings the way DnsLookup does
--EXTENSIONS--
tyhp
--FILE--
<?php
require __DIR__ . '/dns.inc';

function tyhp_dns_message(string $host): string
{
    return 'Could not resolve host ' . $host . '.';
}

/* Spellings that reach gethostbyname and the AAAA search without a literal IP
 * short-circuit. The extension must agree with the DnsLookup sequence. */
$same = [
    '',
    '.',
    'localhost.',
    'LocalHost',
    'LOCALHOST.',
    '127.1',
    '0x7f.1',
    '0177.0.0.1',
    '1.2.3',
    '2130706433',
    ' 127.0.0.1',
    '127.0.0.1 ',
    "127.0.0.1\n",
    '127.0.0.1.',
    '[::1]',
    'fe80::1%lo0',
    '::ffff:127.0.0.1',
    '::FFFF:7F00:1',
    '0:0:0:0:0:0:0:1',
    '1.2.3.4',
    '01.2.3.4',
    'a..invalid',
    '-x.invalid',
    \str_repeat('a', 64) . '.invalid',
    \str_repeat('a', 300) . '.invalid',
];

foreach ($same as $host) {
    $oracle = @tyhp_dns_oracle($host);
    $result = tyhp_dns_run($host);
    if ($oracle === []) {
        if (($result['ok'] ?? null) !== false
            || ($result['reason'] ?? null) !== 'unresolved'
            || ($result['message'] ?? null) !== tyhp_dns_message($host)) {
            echo 'FAIL unresolved ', \json_encode($host), "\n";
            \var_export($result);
            echo "\n";
            exit(1);
        }
        continue;
    }
    if (($result['ok'] ?? null) !== true || ($result['addresses'] ?? null) !== $oracle) {
        echo 'FAIL ', \json_encode($host), "\n";
        echo 'extension=';
        \var_export($result);
        echo "\noracle=";
        \var_export($oracle);
        echo "\n";
        exit(1);
    }
}

/* A NUL byte is never part of a resolvable name. gethostbynamel() and
 * dns_get_record() reject it, and the C resolvers would otherwise stop at it
 * and look up the prefix. */
foreach (["localhost\0evil", "127.0.0.1\0", "\0", "localhost\0"] as $host) {
    $result = tyhp_dns_run($host);
    if (($result['ok'] ?? null) !== false
        || ($result['reason'] ?? null) !== 'unresolved'
        || ($result['message'] ?? null) !== tyhp_dns_message($host)) {
        echo 'FAIL NUL ', \json_encode($host), "\n";
        \var_export($result);
        echo "\n";
        exit(1);
    }
}

echo "ok\n";
?>
--EXPECT--
ok
