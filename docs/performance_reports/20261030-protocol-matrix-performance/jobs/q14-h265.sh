. /var/tmp/ms-perf-891aec1/lib.sh
F=$R/mx-rel/fixtures
run h265-rtmp-whep-a --input rtmp --fixture $F/h265_aac.flv --sources 1 --outputs whep:4 --av-probe 20 $C
run h265-rtsp-whep-a --input rtsp_tcp --fixture $F/h265_aac.flv --sources 1 --outputs whep:4 --av-probe 20 $C
run i-rtmp-h265-whep --input rtmp --fixture $F/h265_aac.flv --sources 16 --outputs whep:32 $C
python3 - <<PY
import json,gzip
for n in ("h265-rtmp-whep-a","h265-rtsp-whep-a"):
    r=json.load(gzip.open("$R/%s/result.json.gz"%n))
    print(n, r["status"], {k:{x:v.get(x) for x in ("ok","av_markers","av_offset_ms_p50","decode_warnings","video_fps","video_pts_backward","audio_pts_gaps","error")} for k,v in (r.get("av_probe") or {}).items() if isinstance(v,dict)})
PY
echo H265_DONE
