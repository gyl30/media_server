#!/bin/bash
# WHIP placement sanitizer and lifecycle runs: base release reference, then tsan/asan/ubsan of the change
E=$(cd "$(dirname "$0")/.." && pwd); R=$(cd $E/../../../.. && pwd)
$E/scripts/whip_lifecycle.sh none /var/tmp/ms-rt-a3f/build/media_server wl-base-r1 | tail -12
$E/scripts/whip_lifecycle.sh none $R/build/media_server wl-wb-r1 | tail -12
for k in tsan asan ubsan; do
    $E/scripts/whip_lifecycle.sh $k $R/build-$k/media_server wl-$k-r1 | tail -12
done
