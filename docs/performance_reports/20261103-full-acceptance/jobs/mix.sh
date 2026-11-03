. /var/tmp/ms-perf-891aec1/jobs-lib/lib.sh
scale() { python3 -c "import sys; s=float(sys.argv[1]); print(','.join(f'{k}:{max(1,round(int(v)*s))}' for k,v in (e.split(':') for e in sys.argv[2].split(','))))" "$@"; }
MIX_A_IN=rtmp:24,rtsp_tcp:8,rtsp_udp:8,gb_udp:8,rtmp_pull:4,rtsp_pull:4,whip:4,whep_pull:4
MIX_A_OUT=rtmp:1500,rtsp:1500,flv:1500,hls:200,whep:400,gb_udp:500,gb_tcp_active:200
MIX_B_IN=whip:48,whep_pull:48,rtmp:32
MIX_B_OUT=whep:640,flv:640,rtmp:320
MIX_C_IN=rtmp:96,rtsp_tcp:32,rtsp_udp:32,gb_udp:64,gb_tcp_passive:16,rtsp_pull:16,rtmp_pull:16,whip:32,whep_pull:32
MIX_C_OUT=flv:336,rtmp:336,hls:32,whep:64,gb_udp:64
MIX_D_IN=whip:48,whep_pull:48,rtmp:32
MIX_D_OUT=whep_video:640,whep:256,flv:32,rtmp:32,hls:8
COMMON="--upstream-workers 16 --upstream-sources 64 --upstream-bin $T/rt-e1/upstream.sh --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 5 --min-free-disk-mib 3072 --viewer-interleave-seed 11 --pull-interleave-seed 11 --source-churn 20 --churn rtmp:100 --av-probe 20"
mixrun() {
  rt=$1; bin=$2; name=$3; mix=$4; s=$5; shift 5
  eval "in=\$MIX_${mix}_IN out=\$MIX_${mix}_OUT"
  ready $name || return 0
  cd $T/$rt
  ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name \
    --server-bin $bin --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 --inputs $(scale $s $in) --outputs $(scale $s $out) $COMMON $*" > $R/$name.out 2>&1
  echo "== $name rt=$rt bin=$bin mix=$mix scale=$s exit=$?"
  python3 $T/sum.py $R/$name
  python3 $T/jobs-lib/life_summary.py $R/$name | cut -c1-1500
  find $R/$name -type f \( -name "*.dump" -o -name "*.mkv" -o -name "*.rtp" -o -name "*.ts" -o -name "*.flv" -o -name "*.es" \) -size +100k -delete
  gzip -f $R/$name/result.json
}
