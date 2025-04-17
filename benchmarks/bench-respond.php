<?php

$n = 100000;
$i = 0;
$timer = new ExcimerTimer;
$timer->setPeriod( 1e-5 );
$timer->setCallback(
	static function () use ( &$i ) {
		$i++;
	}
);
$timer->start();
while ( $i < $n ) {
	usleep( 10 );
}
