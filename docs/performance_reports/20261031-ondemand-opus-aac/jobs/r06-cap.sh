. /var/tmp/ms-perf-891aec1/lib3.sh
CHROME="minptime=10;useinbandfec=1"
st() { python3 $T/st.py $R/$1 | cut -d" " -f1; }
o_whep() { echo whep:$((2 * $1)); }
run cap-whep-a-384-r3 --input whip --sources 384 --outputs $(o_whep 384) --group-size 128 $C
export SERVER_BIN=build-b/media_server
pass=0; fail=0; s=576
while [ $s -le 1152 ]; do
  run cap-whep-b-$s-r1 --input whip --sources $s --outputs $(o_whep $s) --group-size 128 $C
  if [ "$(st cap-whep-b-$s-r1)" = PASS ]; then pass=$s; s=$((s + 64)); else fail=$s; break; fi
done
if [ $fail -gt 0 ] && [ $pass -gt 0 ]; then
  m=$(( (pass + fail) / 2 ))
  run cap-whep-b-$m-r1 --input whip --sources $m --outputs $(o_whep $m) --group-size 128 $C
  [ "$(st cap-whep-b-$m-r1)" = PASS ] && pass=$m || fail=$m
fi
[ $pass -gt 0 ] && run cap-whep-b-$pass-r2 --input whip --sources $pass --outputs $(o_whep $pass) --group-size 128 $C
echo "SEARCH whep-b pass=$pass fail=$fail"
for s in $(seq 512 32 $pass); do
  [ -d $R/cap-whep-b-$s-r1 ] || continue
  python3 $T/st.py $R/cap-whep-b-$s-r1
done
run cap-whepchrome-b-256-r1 --input whip --sources 256 --outputs $(o_whep 256) --group-size 128 --whep-opus-fmtp "$CHROME" $C
run cap-flv-b-480-r1 --input whip --sources 480 --outputs flv:960 $C
unset SERVER_BIN
echo CAP_DONE
