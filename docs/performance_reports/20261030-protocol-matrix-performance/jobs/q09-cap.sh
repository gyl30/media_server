. /var/tmp/ms-perf-891aec1/lib.sh
CHROME="minptime=10;useinbandfec=1"
ALL_C=whep:64,rtmp:64,flv:64,rtsp:64,hls:16,gb_udp:32,gb_tcp_active:16,gb_tcp_passive:16
run w-whepchrome2-b1 --input whip --sources 4 --outputs whep:200 --whep-opus-fmtp "$CHROME" $C
run w-whepchrome2-b2 --input whip --sources 4 --outputs whep:400 --whep-opus-fmtp "$CHROME" $C
run w-whepchrome2-c1 --input whip --sources 32 --outputs whep:64 --whep-opus-fmtp "$CHROME" $C
run w-whepchrome2-c2 --input whip --sources 64 --outputs whep:128 --whep-opus-fmtp "$CHROME" $C
traced w-allchrome2-c --input whip --sources 64 --outputs $ALL_C --whep-opus-fmtp "$CHROME" $C
st() { python3 $T/st.py $R/$1 | cut -d" " -f1; }
search() {
  path=$1; outputs_of=$2; shift 2
  pass=0; fail=0; s=320
  while [ $s -le 704 ]; do
    run cap-$path-$s-r1 --input whip --sources $s --outputs $($outputs_of $s) "$@" $C
    if [ "$(st cap-$path-$s-r1)" = PASS ]; then pass=$s; s=$((s + 64)); else fail=$s; break; fi
  done
  if [ $fail -gt 0 ] && [ $((fail - pass)) -gt 32 ]; then
    m=$(( (pass + fail) / 2 ))
    [ $pass -eq 0 ] && m=256
    run cap-$path-$m-r1 --input whip --sources $m --outputs $($outputs_of $m) "$@" $C
    [ "$(st cap-$path-$m-r1)" = PASS ] && pass=$m || fail=$m
  fi
  [ $pass -gt 0 ] && run cap-$path-$pass-r2 --input whip --sources $pass --outputs $($outputs_of $pass) "$@" $C
  echo "SEARCH $path pass=$pass fail=$fail"
}
for x in rtmp flv rtsp gb_udp gb_tcp_active gb_tcp_passive hls; do
  eval "o_$x() { echo $x:\$((2 * \$1)); }"
  search $x o_$x
done
o_whep() { echo whep:$((2 * $1)); }
search whep o_whep --group-size 128
search whepchrome o_whep --group-size 128 --whep-opus-fmtp "$CHROME"
o_all() { echo whep:$1,rtmp:$1,flv:$1,rtsp:$1,hls:$(($1 / 4)),gb_udp:$(($1 / 2)),gb_tcp_active:$(($1 / 4)),gb_tcp_passive:$(($1 / 4)); }
search all o_all --group-size 128
echo CAP_DONE
