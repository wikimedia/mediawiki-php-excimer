--TEST--
ExcimerProfiler start time stagger
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

function f1() {
	usleep(100000);
}

function f2() {
	usleep(800000);
}

for ($i = 0; $i < 30; $i++) {
	$profiler = new ExcimerProfiler;
	$profiler->setEventType(EXCIMER_REAL);
	$profiler->setPeriod(0.999);
	$profiler->start();
	f1();
	f2();
	$log = $profiler->flush();
	if (isset($log[0]) && $log[0]->getTrace()[0]['function'] === 'f2') {
		echo "OK\n";
		return;
	}
}
echo "FAILED\n";
--EXPECT--
OK
