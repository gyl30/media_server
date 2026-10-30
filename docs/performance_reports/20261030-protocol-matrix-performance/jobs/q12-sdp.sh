. /var/tmp/ms-perf-891aec1/lib.sh
NS=msperf-20261013T011210Z-7788320
bash $T/env_check.sh $R/sdp-boundary.env.json > /dev/null || { echo "sdp-boundary BLOCKED"; exit 0; }
ip netns exec $NS python3 $T/sdp_boundary.py --output $R/sdp-boundary --runtime $T/rt-q
run i2-rtmp-aac-none --input rtmp --fixture $R/mx-rel/fixtures/avsync_720p30.flv --sources 16 --outputs none $C
run i2-rtsptcp-aac-none --input rtsp_tcp --fixture $R/mx-rel/fixtures/avsync_720p30.flv --sources 16 --outputs none $C
echo SDP_DONE
cp $T/stage/whep_dump_av.py $T/rt-q/bench/
mkdir -p $R/g711-replay
for s in mx-rel/rtmp-h264-pcma mx-rel/rtmp-h264-pcmu mx-rel/rtsp-tcp-h264-pcma mx-rel-bframes/whip-h264-opus mx-rel-bframes/rtmp-h264-bframes-aac; do
  n=$(echo $s | tr / _)
  (cd $T/rt-q && taskset -c 8-15 ./run.sh python3 bench/whep_dump_av.py $R/$s/whep-0.dump --output $R/g711-replay/$n.mkv --ffmpeg-bin bin/ffmpeg) > $R/g711-replay/$n.json 2>&1
  echo "$n $(python3 -c "import json,sys; r=json.load(open('$R/g711-replay/$n.json')); print({k: r.get(k) for k in ('ok','av_markers','av_offset_ms_p50','av_offset_ms_max','audio_pts_gaps','decode_warnings','video_pts_backward','recorder_exit','error')})" 2>&1 | tail -1)"
done
echo REPLAY_DONE
