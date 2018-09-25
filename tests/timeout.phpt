--TEST--
excimer_set_timeout
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php
$timer = excimer_set_timeout(
	function () {
		throw new Exception('Timeout');
	}, 1);
try {
	while (true) {
		usleep(100000);
	}
} catch (Exception $e) {
	echo "OK\n";
}
--EXPECT--
OK
