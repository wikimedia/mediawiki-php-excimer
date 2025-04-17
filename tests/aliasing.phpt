--TEST--
glibc timer aliasing (T389734)
--SKIPIF--
<?php if (!extension_loaded("excimer")) print "skip"; ?>
--FILE--
<?php

$noop = function () {};
$throw = function () {
	throw new RuntimeException( 'timer expired' );
};

for ( $i = 0; $i < 10; $i++ ) {
	$timer1 = new ExcimerTimer;
	$timer1->setPeriod( 0.001 );
	$timer1->setCallback( $noop );
	$timer1->start();
	usleep( 1000 );
	$timer1 = null;

	$timer2 = new \ExcimerTimer;
	$timer2->setInterval( 60 );
	$timer2->setCallback( $throw );
	$timer2->start();
	usleep( 1000 );
	$timer2 = null;
}
echo "OK\n";
--EXPECT--
OK
