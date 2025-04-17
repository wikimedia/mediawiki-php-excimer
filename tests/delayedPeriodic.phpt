--TEST--
ExcimerTimer periodic mode with initial delay
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php
$timer = new ExcimerTimer;
$timer->setInterval(0.25);
$timer->setPeriod(0.5);
$expCount = 0;
$events = [];
$timer->setCallback(function($n) use (&$expCount, &$events) {
    $expCount += $n;
	$events[] = microtime(true);
});

$start = microtime(true);
$timer->start();

$elapsed = 0;
$interval = 50000;
while ($elapsed < 1400000) {
	usleep($interval);
    $elapsed += $interval;
}

$firstDelay = $events[0] - $start;
$secondDelay = $events[1] - $events[0];

$count = count($events);
if ($count === 3) {
    echo "periodic event count: OK\n";
} else {
    echo "periodic event count: FAIL - got $count\n";
}

if ($expCount === 3) {
    echo "periodic expiration count: OK\n";
} else {
    echo "periodic expiration count: FAIL - got $expCount\n";
}

if ($firstDelay >= 0.15 && $firstDelay <= 0.35) {
    echo "first delay: OK\n";
} else {
    echo "first delay: FAIL - got $firstDelay\n";
}

if ($secondDelay >= 0.4 && $secondDelay <= 0.6) {
    echo "second delay: OK\n";
} else {
    echo "second delay: FAIL - got $secondDelay\n";
}

--EXPECT--
periodic event count: OK
periodic expiration count: OK
first delay: OK
second delay: OK
