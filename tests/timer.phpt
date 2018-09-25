--TEST--
ExcimerTimer
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php
$timer = new ExcimerTimer;
$timer->setInterval(1);
$timer->setCallback(function() {
	throw new Exception('timeout');
});
$timer->start();
try {
	while (true) {
		usleep(100000);
	}
} catch (Exception $e) {
	echo "OK\n";
}
--EXPECT--
OK
