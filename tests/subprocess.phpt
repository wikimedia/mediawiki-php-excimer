--TEST--
Long-running subprocess does not interfere with ExcimerTimer
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

$timer = new ExcimerTimer();
$timer->setEventType(EXCIMER_REAL);
$timer->setInterval(5);
$timer->start();

shell_exec('sleep 10 >/dev/null 2>&1 &');

$before = microtime(true);
$timer->stop();
$after = microtime(true);

if ( ( $after - $before ) < 10 ) {
    echo 'OK';
}

--EXPECTF--
OK
