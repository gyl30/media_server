. /var/tmp/ms-perf-891aec1/lib4.sh
export SERVER_BIN=build-p/media_server
for rep in 1 2; do
  traced p-ab-chrome-$rep --input whip --sources 64 --outputs whep:128 --whep-opus-fmtp "$CHROME" $C
  run p-cap-chrome-1056-r$rep --input whip --sources 1056 --outputs whep:2112 --group-size 128 --whep-opus-fmtp "$CHROME" $C
  echo "p-cap-chrome-1056-r$rep $(python3 $T/st.py $R/p-cap-chrome-1056-r$rep)"
done
unset SERVER_BIN
python3 $T/ab_table.py $R --pattern "p-ab-chrome-*"
echo CAP_DONE
