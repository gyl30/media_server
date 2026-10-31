T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs3
F=$R/c-fixtures
mkdir -p $F
cd $T/rt-r
ff() { LD_LIBRARY_PATH=lib taskset -c 8-15 bin/ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
ff -i fixtures/avsync_whip.mkv -map 0 -c:v copy -c:a libopus -ac 1 -b:a 64k $F/c-mono.mkv
ff -i fixtures/avsync_whip.mkv -map 0 -c:v copy -c:a libopus -ac 2 -b:a 128k $F/c-stereo.mkv
ff -i fixtures/avsync_whip.mkv -map 0:a -t 40 -c:a libopus -ac 1 -b:a 64k $F/a-1.mka
ff -ss 40 -i fixtures/avsync_whip.mkv -map 0:a -t 40 -c:a libopus -ac 2 -b:a 128k $F/a-2.mka
ff -ss 80 -i fixtures/avsync_whip.mkv -map 0:a -c:a libopus -ac 1 -b:a 64k $F/a-3.mka
printf "file '%s'\nfile '%s'\nfile '%s'\n" $F/a-1.mka $F/a-2.mka $F/a-3.mka > $F/switch.txt
ff -f concat -safe 0 -i $F/switch.txt -c copy $F/a-switch.mka
ff -i fixtures/avsync_whip.mkv -i $F/a-switch.mka -map 0:v -map 1:a -c copy $F/c-switch.mkv
LD_LIBRARY_PATH=lib bin/ffprobe -v error -show_entries format=duration:stream=codec_name,channels $F/c-switch.mkv | tr '\n' ' '; echo
for variant in a c; do
  bin=$([ $variant = a ] && echo build/media_server || echo build-c/media_server)
  bash $T/env_check.sh $R/channels2-$variant.env.json > /dev/null || { echo "channels2-$variant BLOCKED"; continue; }
  ip netns exec $NS env PYTHONPATH=$T/pwlib python3 $T/c_channels.py --output $R/channels2-$variant --runtime $T/rt-r --server-bin $bin --fixtures $F \
    --chrome-bin $T/chrome-dl/root/opt/google/chrome/chrome --seconds 110 > $R/channels2-$variant.out 2>&1
  echo "== channels2-$variant exit=$?"; cat $R/channels2-$variant.out
done
echo CHANNELS_DONE
