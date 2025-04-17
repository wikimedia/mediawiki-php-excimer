--TEST--
Concurrent timers
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

$interval = 0.2;
$timers = [];
$fired = [];
for ($i = 1; $i <= 10; $i++) {
    $timer = new ExcimerTimer;
    $timer->setInterval($i * $interval);
    $timer->setCallback(function () use (&$fired, $i) {
        $fired[$i] = microtime(true);
    } );
    $timers[] = $timer;
}
$start = microtime(true);
foreach ($timers as $timer) {
    $timer->start();
}
while (count($fired) < 10 && (microtime(true) - $start) < 10) {
    usleep(100000);
}
for ($i = 1; $i <= 10; $i++) {
    $min = ($i - 1) * $interval;
    $max = ($i + 2) * $interval;
    $t = $fired[$i] - $start;
    if ($t >= $min && $t <= $max) {
        print "$i: OK\n";
    } else {
        print "$i: FAILED: $min <= $t <= $max\n";
    }
}
--EXPECT--
1: OK
2: OK
3: OK
4: OK
5: OK
6: OK
7: OK
8: OK
9: OK
10: OK
