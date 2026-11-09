--TEST--
a forked child does not wait for threads that exist only in the parent
--EXTENSIONS--
tyhp
pcntl
--FILE--
<?php
require __DIR__ . '/dns.inc';

/* Left untaken: the parent has a thread for this lookup, the child only has
 * the record. */
$pending = \Tyhp\Ext::dnsStart('localhost');

$pid = \pcntl_fork();
if ($pid === -1) {
    echo "FAIL fork\n";
    exit(1);
}
if ($pid === 0) {
    $result = tyhp_dns_run('127.0.0.1');
    exit(($result['ok'] ?? null) === true && ($result['addresses'] ?? null) === ['127.0.0.1'] ? 0 : 3);
}

\pcntl_waitpid($pid, $status);
if (!\pcntl_wifexited($status) || \pcntl_wexitstatus($status) !== 0) {
    echo "FAIL child status $status\n";
    exit(1);
}

tyhp_dns_wait($pending['pipe']);
$result = \Tyhp\Ext::dnsTake($pending['id']);
if (($result['ok'] ?? null) === null) {
    echo "FAIL parent take\n";
    exit(1);
}
echo "ok\n";
?>
--EXPECT--
ok
