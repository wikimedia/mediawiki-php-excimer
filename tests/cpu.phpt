--TEST--
ExcimerProfiler CPU profile
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--INI--
zend.assertions=1
--FILE--
<?php

function getClosures() {
	return [
		function () {
			return md5(str_repeat('x', 100000)); # line 5
		},
		function () {
			return md5(str_repeat('x', 100000)); # line 8
		}
	];
};

list($closure1, $closure2) = getClosures();
$c1line = (new ReflectionFunction($closure1))->getStartLine();
$c2line = (new ReflectionFunction($closure1))->getStartLine();

class C {
	function member() {
		return md5(str_repeat('x', 100000));
	}
}

$obj = new C;

function foo() {
	bar();
}

function bar() {
	global $closure1, $closure2, $obj;
	$closure1();
	$closure2();
	$obj->member();
	baz();
}

function baz() {
	return md5(str_repeat('x', 100000));
}

function cpu() {
	$r = getrusage();
	return $r['ru_utime.tv_sec'] + 1e-6 * $r['ru_utime.tv_usec'];
}

$profiler = new ExcimerProfiler;
$profiler->setEventType(EXCIMER_CPU);
$profiler->setPeriod(0.1);
$t = cpu();
$profiler->start();

while (cpu() - $t < 3) {
	for ($i = 0; $i < 100; $i++) {
		foo();
	}
}

$profiler->stop();
$log = $profiler->flush();

function sort_lines($text) {
	$lines = explode("\n", trim($text));
	sort($lines);
	return implode("\n", $lines);
}

// It takes 3 seconds to populate the profiler log, which is quite expensive,
// so we do as many tests as possible on the result while we have it.

// Test formatCollapsed
$collapsed = $log->formatCollapsed();
// Normalize variable file path, and sample count
$collapsed = str_replace(__FILE__, '/data/cpu.php', $collapsed);
$collapsed = preg_replace('/ \d+$/m', ' 00', $collapsed);
$collapsed = sort_lines($collapsed);
echo "formatCollapsed:\n$collapsed\n\n";

// Test getSpeedscopeData
function check_speedscope($speedscope, $name, $expected) {
	$ok = false;
	foreach ($speedscope['profiles'][0]['samples'] as $sample) {
		if (count($sample) !== count($expected)) {
			break;
		}
		$matches = 0;
		foreach ($expected as $i => $expectedFrame) {
			if (!isset($sample[$i])) {
				break;
			}
			$frame = $speedscope['shared']['frames'][$sample[$i]];
			if ($frame === $expectedFrame) {
				$matches++;
			} else {
				break;
			}
		}
		if ($matches === count($expected)) {
			$ok = true;
			break;
		}
	}
	echo "getSpeedscopeData $name: " . ($ok ? "OK\n" : "FAILED\n");
	if (!$ok) {
		echo "Expected: ";
		var_dump($expected);
		echo "Actual: ";
		var_dump($speedscope);
	}
}

$speedscope = $log->getSpeedscopeData();
check_speedscope(
	$speedscope,
	'baz',
	[
		['name' => __FILE__, 'file' => __FILE__],
		['name' => 'foo', 'file' => __FILE__],
		['name' => 'bar', 'file' => __FILE__],
		['name' => 'baz', 'file' => __FILE__]
	]
);

// Test foreach (get_iterator handler)
$found = [];
$count = 0;
$eventCount = 0;
$traces = [];
$firstTimestamp = false;
$lastTimestamp = false;
foreach ($log as $entry) {
	$trace = $entry->getTrace();
	$traces[] = $trace;
	if (count($trace) == 4
		&& ($trace[0]['function'] ?? '') === 'baz'
		&& ($trace[1]['function'] ?? '') === 'bar'
		&& ($trace[2]['function'] ?? '') === 'foo'
		&& ($trace[3]['function'] ?? '') === '')
	{
		$found['baz'] = true;
	}

	if (count($trace) == 4
		&& ($trace[0]['closure_line'] ?? '') === $c1line
		&& ($trace[1]['function'] ?? '') === 'bar'
		&& ($trace[2]['function'] ?? '') === 'foo'
		&& ($trace[3]['function'] ?? '') === '')
	{
		$found['closure1'] = true;
	}

	if (count($trace) == 4
		&& ($trace[0]['closure_line'] ?? '') === $c2line
		&& ($trace[1]['function'] ?? '') === 'bar'
		&& ($trace[2]['function'] ?? '') === 'foo'
		&& ($trace[3]['function'] ?? '') === '')
	{
		$found['closure2'] = true;
	}

	if (count($trace) == 4
		&& ($trace[0]['class'] ?? '') === 'C'
		&& ($trace[0]['function'] ?? '') === 'member'
		&& ($trace[1]['function'] ?? '') === 'bar'
		&& ($trace[2]['function'] ?? '') === 'foo'
		&& ($trace[3]['function'] ?? '') === '')
	{
		$found['member'] = true;
	}

	if ($firstTimestamp === false) {
		$firstTimestamp = $entry->getTimestamp();
	}

	if ($lastTimestamp !== false) {
		assert(is_float($entry->getTimestamp()));
		assert($entry->getTimestamp() >= 0);
		assert($entry->getTimestamp() > $lastTimestamp);
	}
	$lastTimestamp = $entry->getTimestamp();

	assert( $entry->getEventCount() > 0 );
	$eventCount += $entry->getEventCount();
	$count++;
}

assert( $log->getEventCount() === $eventCount );

function check_found($found, $func) {
	if (isset($found[$func])) {
		echo "foreach found $func: OK\n";
	} else {
		echo "foreach found $func: FAILED\n";
	}
}
check_found($found, 'baz');
check_found($found, 'closure1');
check_found($found, 'closure2');
check_found($found, 'member');

$timestampDiff = $lastTimestamp - $firstTimestamp;
if ($timestampDiff > 1 && $timestampDiff < 1000) {
	echo "getTimestamp: OK\n";
} else {
	echo "getTimestamp: FAILED $timestampDiff\n";
}

echo 'ExcimerLog::count ' . ($count == $log->count() ? "OK\n" : "FAILED\n");
echo 'count(ExcimerLog) ' . ($count == count($log) ? "OK\n" : "FAILED\n");

// Test Iterator interface
$count = 0;
for ($log->rewind(); $log->valid(); $log->next(), $count++) {
	$entry = $log->current();
	assert(serialize($entry->getTrace()) == serialize($traces[$count]));
}
assert($count == $log->count());

// Test aggregateByFunction
// Typically the parent functions foo() and bar() will have self=0 and
// inclusive ~= 30. The other 4 functions will have a count of about 30/4 = 7.5.
// The probability of C::member() or baz() having a count of zero is about 1 in 5600.
$stats = $log->aggregateByFunction();
assert($stats['foo']['inclusive'] > 10);
assert($stats['foo']['self'] < 10);
assert($stats['bar']['inclusive'] > 10);
assert($stats['bar']['self'] < 10);
assert($stats['C::member']['inclusive'] > 0);
assert($stats['C::member']['self'] > 0);
assert($stats['baz']['inclusive'] > 0);
assert($stats['baz']['self'] > 0);

// Ensure $stats is sorted
$counts = array_column( $stats, 'inclusive' );
$sortedCounts = $counts;
rsort( $sortedCounts );
assert( $sortedCounts === $counts );

--EXPECT--
formatCollapsed:
/data/cpu.php;foo;bar;C::member 00
/data/cpu.php;foo;bar;baz 00
/data/cpu.php;foo;bar;{closure:/data/cpu.php(5)} 00
/data/cpu.php;foo;bar;{closure:/data/cpu.php(8)} 00

getSpeedscopeData baz: OK
foreach found baz: OK
foreach found closure1: OK
foreach found closure2: OK
foreach found member: OK
getTimestamp: OK
ExcimerLog::count OK
count(ExcimerLog) OK
