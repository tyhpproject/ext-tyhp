--TEST--
a closed read end does not stop the lookup or kill the process
--EXTENSIONS--
tyhp
pcntl
--FILE--
<?php
require __DIR__ . '/dns.inc';

function tyhp_dns_take_when_ready(int $id): array
{
    $deadline = \microtime(true) + 30;
    while (true) {
        try {
            return \Tyhp\Ext::dnsTake($id);
        } catch (\LogicException $e) {
            if (\microtime(true) > $deadline) {
                echo "FAIL still not ready\n";
                exit(1);
            }
            \usleep(100);
        }
    }
}

/* The CLI ignores SIGPIPE. Restore the default action so a write to a closed
 * pipe from the side thread would end the process unless that thread blocks
 * the signal itself. */
\pcntl_signal(\SIGPIPE, \SIG_DFL);

/* The thread writes its wake byte to a pipe whose read end is gone: EPIPE,
 * never SIGPIPE. Whether the write lands after the close depends on timing,
 * so start many lookups and close each pipe straight away. */
$ids = [];
for ($i = 0; $i < 200; $i++) {
    $started = \Tyhp\Ext::dnsStart($i % 10 === 0 ? 'tyhp-ext-does-not-exist.invalid' : 'localhost');
    $ids[] = $started['id'];
    \fclose($started['pipe']);
}
unset($started);

foreach ($ids as $id) {
    $result = tyhp_dns_take_when_ready($id);
    if (!\is_bool($result['ok'] ?? null)) {
        echo "FAIL result\n";
        \var_export($result);
        echo "\n";
        exit(1);
    }
}

/* The process disposition is the one we set; only the side threads block it. */
if (\pcntl_signal_get_handler(\SIGPIPE) !== \SIG_DFL) {
    echo "FAIL SIGPIPE disposition changed\n";
    exit(1);
}

$second = tyhp_dns_run('127.0.0.1');
if (($second['ok'] ?? null) !== true || ($second['addresses'] ?? null) !== ['127.0.0.1']) {
    echo "FAIL second lookup\n";
    \var_export($second);
    echo "\n";
    exit(1);
}
echo "ok\n";
?>
--EXPECT--
ok
