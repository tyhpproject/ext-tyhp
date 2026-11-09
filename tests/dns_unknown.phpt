--TEST--
dnsTake of an unknown id throws and dnsCancel does not
--EXTENSIONS--
tyhp
--FILE--
<?php
foreach ([0, 1, -1, \PHP_INT_MAX] as $id) {
    try {
        \Tyhp\Ext::dnsTake($id);
        echo "FAIL take $id did not throw\n";
        exit(1);
    } catch (\InvalidArgumentException $e) {
    } catch (\Throwable $e) {
        echo "FAIL take $id threw " . $e::class . "\n";
        exit(1);
    }
    \Tyhp\Ext::dnsCancel($id);
}
echo "ok\n";
?>
--EXPECT--
ok
