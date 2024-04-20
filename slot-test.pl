#!/usr/bin/env perl
use strict;
use warnings;

use constant {
  CHERRY => 0,
  ORANGE => 1,
  PLUM => 2,
  BELL => 3,
  BAR => 4
};

use constant PULLS => 100_000_000;

my @reel0 = ( ORANGE, BAR, PLUM, CHERRY, PLUM, ORANGE, BELL, PLUM, ORANGE, CHERRY, ORANGE, BAR, ORANGE, PLUM, ORANGE, PLUM, CHERRY, BAR, ORANGE, PLUM );
my @reel1 = ( BELL, CHERRY, BELL, CHERRY, BELL, CHERRY, BELL, ORANGE, BELL, CHERRY, BELL, CHERRY, BELL, BAR, BELL, CHERRY, BELL, CHERRY, BELL, PLUM );
my @reel2 = ( ORANGE, CHERRY, ORANGE, PLUM, ORANGE, BAR, ORANGE, PLUM, ORANGE, BELL, ORANGE, CHERRY, ORANGE, PLUM, ORANGE, PLUM, ORANGE, CHERRY, ORANGE, PLUM );

my $payout = 0;

my ($r0, $r1, $r2);

for (1 .. PULLS)
{
  ($r0, $r1, $r2) = ($reel0[rand(20)], $reel1[rand(20)], $reel2[rand(20)]);

  $payout += 
  $r0 == CHERRY ? ($r1 == CHERRY ? ($r2 == CHERRY ? 11 : 5) : 3) :
     ($r0 == ORANGE && $r1 == ORANGE && ($r2 == ORANGE || $r2 == BAR) ? 11 :
     ($r0 == PLUM && $r1 == PLUM && ($r2 == PLUM || $r2 == BAR) ? 13 :
     ($r0 == BELL && $r1 == BELL && ($r2 == BELL || $r2 == BAR) ? 18 :
     ($r0 == BAR && $r1 == BAR && $r2 == BAR ? 100 : 0))));
}

print "payout = $payout, loss = " . PULLS . ", net = " . ($payout - PULLS) . ", " . ($payout / (PULLS / 100)) . "%\n";
