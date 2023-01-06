--TEST--
ExcimerProfiler max depth
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

$profiler = new ExcimerProfiler;
$profiler->setEventType(EXCIMER_REAL);
$profiler->setPeriod(0.1);
$profiler->setMaxDepth(5);

function foo( $depth ) {
	global $profiler;

	if ( $depth > 0 ) {
		foo( $depth - 1 );
	} else {
		$profiler->start();
		usleep(100000);
		$profiler->stop();
	}
}

foo( 20 );

$log = $profiler->flush();
echo $log->formatCollapsed() . "\n";

--EXPECTF--
excimer_truncated;foo;foo;foo;foo;foo;foo %d
