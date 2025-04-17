--TEST--
ExcimerTimer one-shot mode
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php
$timer = new ExcimerTimer;
$timer->setInterval(0.5);
$count = 0;
$timer->setCallback(function() use (&$count) {
    $count++;
});

$timer->start();

$start = microtime(true);
$elapsed = 0;
$fired_at = 0;
$interval = 20000;
while ($elapsed < 600000) {
    $t = microtime(true);
    $elapsed = ($t - $start) * 1e6;
    if ($count === 1 && $fired_at === 0) {
        $fired_at = $elapsed;
    }
    usleep($interval);
}
if ($count === 1 && $fired_at >= 400000 && $fired_at <= 600000) {
    echo "oneshot: OK\n";
} else {
    echo "oneshot: FAIL - count: $count, fired_at: $fired_at, elapsed: $elapsed\n";
}
--EXPECT--
oneshot: OK
