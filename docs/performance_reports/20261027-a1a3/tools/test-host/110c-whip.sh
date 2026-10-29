T=/var/tmp/ms-perf-891aec1
U="--perf-stat --warmup 15 --duration 60 --ready-timeout 240 --ramp-per-second 200 --group-size 256"
for i in 2 3; do
RT=rt-a3f $T/a3.sh a3f-whip200-A3-r$i --input whip --sources 200 --outputs flv:200 $U
RT=rt-a11gdf $T/a3.sh a3f-whip200-F-r$i --input whip --sources 200 --outputs flv:200 $U
done
