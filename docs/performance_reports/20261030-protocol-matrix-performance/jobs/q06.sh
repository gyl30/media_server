. /var/tmp/ms-perf-891aec1/lib.sh
traced lc-smoke3 --input whip --sources 4 --outputs whep:8,flv:8 --source-churn 30 --source-churn-input whip \
  --source-churn-viewers hls:1,gb_tcp_active:1,rtsp:2,rtmp:1,whep:2,flv:2,gb_udp:1,gb_tcp_passive:1 --warmup 10 --duration 70 --ready-timeout 120 --cleanup-settle 5 --perf-stat
python3 -c "
import json,gzip; r=json.load(gzip.open('$R/lc-smoke3/result.json.gz'))
print(json.dumps(r.get('source_churn'),indent=1)); print(r.get('cleanup',{}).get('server_settled',{}).get('fd'), r.get('baseline',{}).get('fd'))"
tail -40 $R/trace/lc-smoke3.bt; cat $R/trace/lc-smoke3.tasks | head -20
run w-whep-a2 --input whip --sources 1 --outputs whep:4 --av-probe 20 --decode-probe $C
