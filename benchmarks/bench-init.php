<?php

$t = microtime( true );
$n = 100000;
for ( $i = 0; $i < $n; $i++ ) {
	$timer = new ExcimerTimer;
	$timer->setInterval( 1000 );
	$timer->start();
}
$t = microtime( true ) - $t;
print round( $t / $n * 1e6, 3 ) . "us\n";
