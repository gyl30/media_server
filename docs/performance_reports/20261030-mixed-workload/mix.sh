T=/var/tmp/ms-perf-891aec1
scale() { python3 -c "import sys; s=float(sys.argv[1]); print(','.join(f'{k}:{max(1,round(int(v)*s))}' for k,v in (e.split(':') for e in sys.argv[2].split(','))))" "$@"; }
MIX_A_IN=rtmp:24,rtsp_tcp:8,rtsp_udp:8,gb_udp:8,rtmp_pull:4,rtsp_pull:4,whip:4,whep_pull:4
MIX_A_OUT=rtmp:1500,rtsp:1500,flv:1500,hls:200,whep:400,gb_udp:500,gb_tcp_active:200
MIX_B_IN=whip:48,whep_pull:48,rtmp:32
MIX_B_OUT=whep:640,flv:640,rtmp:320
MIX_C_IN=rtmp:96,rtsp_tcp:32,rtsp_udp:32,gb_udp:64,gb_tcp_passive:16,rtsp_pull:16,rtmp_pull:16,whip:32,whep_pull:32
MIX_C_OUT=flv:336,rtmp:336,hls:32,whep:64,gb_udp:64
COMMON="--upstream-workers 16 --upstream-sources 64 --upstream-bin $T/rt-e1/upstream.sh --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 5 --min-free-disk-mib 3072 --viewer-interleave-seed 11 --pull-interleave-seed 11 --source-churn 20 --churn rtmp:100 --av-probe 20"
mixrun() { rt=$1; name=$2; mix=$3; s=$4; shift 4; eval "in=\$MIX_${mix}_IN out=\$MIX_${mix}_OUT"; RT=$rt $T/run_perf.sh $name --inputs $(scale $s $in) --outputs $(scale $s $out) $COMMON "$@"; gzip -f $T/runs/$name/result.json; }
