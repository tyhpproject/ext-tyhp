--TEST--
The tyhp module loads and reports version 0.1.0
--EXTENSIONS--
tyhp
--FILE--
<?php
var_dump(extension_loaded('tyhp'));
var_dump(phpversion('tyhp') === '0.1.0');
?>
--EXPECT--
bool(true)
bool(true)
