--TEST--
ExcimerTimer periodic mode
--FILE--
<?php
$timer = new ExcimerTimer;
$timer->setPeriod(0.1);
$callCount = 0;
$eventCount = 0;
$timer->setCallback(function($n) use (&$callCount, &$eventCount) {
	$callCount ++;
	$eventCount += $n;
});
$t = microtime(true);
$timer->start();
while (microtime(true) - $t < 1) {
	usleep(10000);
}
if ($callCount > 5 && $callCount < 15) {
	echo "call count: OK\n";
} else {
	echo "call count: FAILED\n";
}
if ($eventCount > 5 && $eventCount < 15) {
	echo "event count: OK\n";
} else {
	echo "event count: FAILED\n";
}
--EXPECT--
call count: OK
event count: OK
