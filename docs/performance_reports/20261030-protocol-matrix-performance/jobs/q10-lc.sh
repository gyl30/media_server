. /var/tmp/ms-perf-891aec1/lib.sh
cp $T/stage/perf_run.py $T/rt-q/bench/
CHROME="minptime=10;useinbandfec=1"
LC="--source-churn 60 --source-churn-input whip --warmup 10 --duration 200 --ready-timeout 120 --cleanup-settle 10 --perf-stat --control-probe-ms 200"
traced lc-full-a --input whip --sources 8 --outputs whep:16,flv:16 $LC \
  --source-churn-viewers hls:1,gb_tcp_active:1,rtsp:2,rtmp:2,whep:2,flv:2,gb_udp:1,gb_tcp_passive:1
traced lc-full-b --input whip --sources 8 --outputs whep:16,flv:16 $LC --whep-opus-fmtp "$CHROME" \
  --source-churn-viewers whep:2,flv:2,gb_udp:1,gb_tcp_passive:1,hls:1,gb_tcp_active:1,rtsp:2,rtmp:2
for n in lc-full-a lc-full-b; do
  python3 -c "
import json,gzip; r=json.load(gzip.open('$R/$n/result.json.gz'))
for x in r['source_churn']['rounds']: print({k: x.get(k) for k in ('round','publisher_stop','publish_ready_ms','viewers_ready_ms','viewers_left','publish_gone_ms','viewers_ended','viewers_lingering','viewers_gone_ms','server_fd_before_viewers','server_fd_after_viewers','ok')})
c=r['cleanup']; print('fd', r['baseline']['fd'], (c.get('server_settled') or {}).get('fd'), 'left', c.get('remaining_owned_receivers'))"
  grep -v "^$" $R/trace/$n.bt | tail -30; cat $R/trace/$n.tasks | head -12
done
echo LC_DONE
