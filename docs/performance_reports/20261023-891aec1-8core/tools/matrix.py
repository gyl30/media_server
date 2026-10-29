#!/usr/bin/env python3
import json
import sys
from pathlib import Path


def codecs(metadata):
    streams = metadata.get("streams", []) if isinstance(metadata, dict) else []
    return "+".join(stream.get("codec_name", "?") for stream in streams) or "?"


def probe(entry):
    if not entry:
        return ""
    if not entry.get("ok"):
        return " probe=FAIL"
    parts = [f"{stream['codec_name']}:{stream.get('nb_read_frames')}" for stream in entry.get("streams", [])]
    return " probe=" + ",".join(parts)


def main():
    for item in sys.argv[1:]:
        path = Path(item, "result.json")
        if not path.exists():
            print(f"{Path(item).name}: no result")
            continue
        data = json.loads(path.read_text())
        config = data.get("config", {})
        print(f"{Path(item).name}: input={config.get('input')} fixture={codecs(data.get('fixture_metadata'))} "
              f"status={data['status']}/{data.get('capacity_boundary')}")
        decode = data.get("decode_probe") or {}
        for group in data.get("groups", []):
            protocol = group["name"].rsplit("-", 1)[0]
            measurement = group.get("measurement", {})
            realtime = measurement.get("viewer_realtime_min")
            audio = measurement.get("steady_audio_bytes")
            fields = [f"ready={measurement.get('ready')}/{group['requested']}", f"progressing={measurement.get('progressing')}"]
            if realtime is not None:
                fields.append(f"rt={realtime:.3f}")
            if audio is not None:
                fields.append(f"audio_bytes={audio}")
            if group.get("failure_category"):
                fields.append(f"failure={group['failure_category']} {group.get('first_errors', [])[:2]}")
            print(f"  {protocol:15s} " + " ".join(fields) + probe(decode.get(protocol)))
        for error in data.get("errors", [])[:2]:
            print(f"  error: {error.get('message', '')[:160]}")


if __name__ == "__main__":
    main()
