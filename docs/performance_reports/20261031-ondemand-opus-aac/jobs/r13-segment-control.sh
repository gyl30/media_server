T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs3
F=$R/c-fixtures
G=$R/c-fixtures-control
mkdir -p $G
cd $T/rt-r
ff() { LD_LIBRARY_PATH=lib taskset -c 8-15 bin/ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
ff -i fixtures/avsync_whip.mkv -map 0:a -t 40 -c:a libopus -ac 2 -b:a 128k $G/a-1.mka
ff -ss 40 -i fixtures/avsync_whip.mkv -map 0:a -t 40 -c:a libopus -ac 2 -b:a 128k $G/a-2.mka
ff -ss 80 -i fixtures/avsync_whip.mkv -map 0:a -c:a libopus -ac 2 -b:a 128k $G/a-3.mka
printf "file '%s'\nfile '%s'\nfile '%s'\n" $G/a-1.mka $G/a-2.mka $G/a-3.mka > $G/switch.txt
ff -f concat -safe 0 -i $G/switch.txt -c copy $G/a-switch.mka
ff -i fixtures/avsync_whip.mkv -i $G/a-switch.mka -map 0:v -map 1:a -c copy $G/c-switch.mkv
cp $F/c-mono.mkv $F/c-stereo.mkv $G/
bash $T/env_check.sh $R/channels2-control.env.json > /dev/null || { echo "control BLOCKED"; exit; }
ip netns exec $NS env PYTHONPATH=$T/pwlib python3 $T/c_channels.py --output $R/channels2-control --runtime $T/rt-r --server-bin build-c/media_server \
  --fixtures $G --chrome-bin $T/chrome-dl/root/opt/google/chrome/chrome --seconds 110 > $R/channels2-control.out 2>&1
echo "== control exit=$?"
python3 -c "
import json
r=json.load(open('$R/channels2-control/result.json'))['sources']['switch']
print({k: (v.get('toc'), v.get('audio_pts_overlaps'), v.get('decode_warnings')) for k, v in r.items() if k in ('stereo0', 'firefox')})
c=r['chrome']; print('chrome', {k: c[k] for k in ('concealed', 'concealment_events')})
prev=None
for x in c['samples']:
    k=(x.get('concealedSamples'), x.get('concealmentEvents'))
    if k!=prev: print(x['t'], k)
    prev=k
"
echo CONTROL_DONE
