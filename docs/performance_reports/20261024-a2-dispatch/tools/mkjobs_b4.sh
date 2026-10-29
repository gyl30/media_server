#!/bin/bash
# usage: mkjobs_b4.sh <B runtime dir> <run prefix>; prints the formal A/B job list for rt-a2A-c999003 vs the given B3 bundle
B=$1
P=$2
A=rt-a2A-c999003
MIX7000="rtmp:1167,flv:1167,rtsp:1167,whep:1167,gb_udp:1166,gb_tcp_passive:1166"
MIX7250="rtmp:1209,flv:1209,rtsp:1208,whep:1208,gb_udp:1208,gb_tcp_passive:1208"
echo 'T=/var/tmp/ms-perf-891aec1'
echo 'S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 240 --ramp-per-second 200 --group-size 256"'
echo 'G="--gb-port-base 18700 --perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"'
declare -A count
pair() {
  local name=$1 order=$2 opts=$3
  shift 3
  local rt run
  for variant in $(echo $order | sed 's/./& /g'); do
    count[$name$variant]=$((${count[$name$variant]:-0} + 1))
    run=$P-$name-$variant-r${count[$name$variant]}
    rt=$A
    [ $variant = B ] && rt=$B
    echo "RT=$rt \$T/pr2.sh $run $* \$$opts"
  done
}
echo "RT=$B \$T/pr2.sh f-$P-all8-B --input rtmp --sources 1 --outputs rtmp:20,rtsp:20,flv:20,hls:5,whep:20,gb_udp:20,gb_tcp_active:20,gb_tcp_passive:20 --decode-probe --churn rtmp:20 --warmup 10 --duration 60 --cleanup-settle 10"
echo "RT=$B \$T/pr2.sh f-$P-stop-B --input rtmp --sources 1 --outputs rtmp:50,rtsp:50,flv:50,whep:50,gb_udp:50,gb_tcp_passive:50 --fault source_stop --warmup 10 --duration 30 --cleanup-settle 10"
echo "RT=$B \$T/pr2.sh f-$P-src10-B --input rtmp --sources 10 --outputs rtmp:300,rtsp:200,flv:200,whep:100,gb_udp:200 --warmup 10 --duration 60 --cleanup-settle 10"
pair sn ABBA S --outputs rtmp:6500 --duration 60
pair whep3500 ABBAAB S --outputs whep:3500 --churn whep:50 --duration 120
pair gbudp8000 BAABBA S --outputs gb_udp:8000 --churn gb_udp:100 --duration 120
pair gbtcpactive12000 ABBA G --outputs gb_tcp_active:12000 --duration 60
pair gbtcppassive7000 BAAB S --outputs gb_tcp_passive:7000 --duration 60
pair rtsp6000 ABBAAB S --outputs rtsp:6000 --duration 60
pair mix7000 BAABBA S --outputs $MIX7000 --duration 60
pair sn BAAB S --outputs rtmp:6500 --duration 60
pair whep3250 BAAB S --outputs whep:3250 --churn whep:50 --duration 120
pair gbudp7500 ABBA S --outputs gb_udp:7500 --churn gb_udp:100 --duration 120
pair gbtcpactive13000 BAAB G --outputs gb_tcp_active:13000 --duration 60
pair gbtcppassive12000 ABBA G --outputs gb_tcp_passive:12000 --duration 60
pair mix7250 BAAB S --outputs $MIX7250 --duration 60
pair rtmp8500 ABBA S --outputs rtmp:8500 --duration 60
pair rtsp7750 BAAB S --outputs rtsp:7750 --duration 60
pair flv6000 ABBA S --outputs flv:6000 --duration 60
pair whep3750 BA S --outputs whep:3750 --churn whep:50 --duration 120
pair rtmp9000 AB S --outputs rtmp:9000 --duration 60
pair sn ABBA S --outputs rtmp:6500 --duration 60
