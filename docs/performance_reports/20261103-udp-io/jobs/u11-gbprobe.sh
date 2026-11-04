. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
AV="--fixture fixtures/avsync_720p30.flv --whip-fixture fixtures/avsync_whip.mkv"
for v in base gso2; do
  B=build/media_server-$v
  mixrun rt-g $B u11-a-1.0-avsync-$v A 1.0 $AV
  mixrun rt-g $B u11-c-2.15-avsync-$v C 2.15 $AV
done
echo U11_DONE
