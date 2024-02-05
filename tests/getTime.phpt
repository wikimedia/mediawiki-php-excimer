--TEST--
ExcimerProfiler::getTime
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

$timer = new ExcimerTimer();
$timer->setEventType(EXCIMER_REAL);
$timer->setPeriod(5);

$time = $timer->getTime();
if ($time == 0.0) {
    echo "remaining time is zero when not started: OK\n";
} else {
    echo "unexpected remaining time before starting: $time\n";
}

$timer->start();
sleep(1);

$time = $timer->getTime();

if ($time <= 4.0 && $time >= 3.0) {
    echo "remaining time: OK\n";
} else {
    echo "unexpected remaining time: $time\n";
}

$timer->stop();

$time = $timer->getTime();
if ($time == 0.0) {
    echo "remaining time is zero after stopping: OK\n";
} else {
    echo "unexpected remaining time after stopping: $time\n";
}


--EXPECTF--
remaining time is zero when not started: OK
remaining time: OK
remaining time is zero after stopping: OK
