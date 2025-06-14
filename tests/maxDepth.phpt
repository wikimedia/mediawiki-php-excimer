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

function quux() {
	bar_1();
}
function bar_1() {
	bar_2();
}
function bar_2() {
	bar_3();
}
function bar_3() {
	bar_4();
}
function bar_4() {
	foo_5();
}
function foo_5() {
	foo_6();
}
function foo_6() {
	foo_7();
}
function foo_7() {
	foo_8();
}
function foo_8() {
	foo_9();
}
function foo_9() {
	foo_10();
}
function foo_10() {
	global $profiler;
	$profiler->start();
	while (!count($profiler->getLog())) {
		// 10ms toward the 0-100ms interval (random/staggered start time)
		usleep(10000);
	}
	$profiler->stop();
}

quux();

$log = $profiler->flush();
echo $log->formatCollapsed() . "\n";

// T380748: PHP 8.1-8.3 includes one more frame than PHP 8.4+,
// because PHP 8.4 adds support for internal frames like usleep().

// php83> excimer_truncated;foo_5;foo_6;foo_7;foo_8;foo_9;foo_10
// php84> excimer_truncated;foo_6;foo_7;foo_8;foo_9;foo_10
//
// We tolerate this via the "%s" wildcard.
// Note that it is should not be in front of "foo_6" but inside of it,
// as we must strictly assert the top-most frame to be foo (5 or 6),
// and not bar_1 to bar_4 (which would mean the maxDepth is broken).

--EXPECTF--
excimer_truncated;fo%s_6;foo_7;foo_8;foo_9;foo_10 %d
