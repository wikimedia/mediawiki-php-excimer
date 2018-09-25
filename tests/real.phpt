--TEST--
ExcimerProfiler real time profile
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

function foo() {
	usleep(100000);
}

$profiler = new ExcimerProfiler;
$profiler->setEventType(EXCIMER_REAL);
$profiler->setPeriod(0.1);

for ($j = 0; $j < 3; $j++) {
	$profiler->start();
	for ($i = 0; $i < 10; $i++) {
		foo();
	}
	$profiler->stop();
}

$log = $profiler->flush();
$found = 0;
foreach ($log as $entry) {
	$trace = $entry->getTrace();
	if (isset($trace[0]['function'])
		&& $trace[0]['function'] === 'foo'
		&& !isset($trace[1]['function']))
	{
		$found++;
	}
}
if ($found > 11) {
	echo "OK\n";
} else {
	echo "FAILED\n";
}

--EXPECT--
OK
