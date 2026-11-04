. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for v in base gso2; do
  mixrun rt-g build/media_server-$v u16-a-1.05-$v-r1 A 1.05
  mixrun rt-g build/media_server-$v u16-c-2.3-$v-r1 C 2.3
done
echo U16_DONE
