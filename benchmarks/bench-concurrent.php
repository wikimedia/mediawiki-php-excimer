<?php

$timers = [];

$numTimers = $argv[1] ?? 1000;

$eventsHandled = 0;

$t = microtime( true );

for ( $i = 0; $i < $numTimers; $i++ ) {
    $timer = new ExcimerTimer;
    $timer->setPeriod( 1e-2 );
    $timer->setCallback( function () use ( &$eventsHandled ) {
        $eventsHandled++;
    } );
    $timer->start();
    $timers[] = $timer;
}

print "Setup: " . ( microtime( true ) - $t ) . "\n";

for ( $i = 0; $i < 10; $i++ ) {
    $t = microtime( true );
    while ( ( microtime( true ) - $t ) < 1 );
    print "$eventsHandled\n";
}


