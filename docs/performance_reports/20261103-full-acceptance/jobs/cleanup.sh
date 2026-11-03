#!/bin/bash
T=/var/tmp/ms-perf-891aec1
dry=0
[ "${1:-}" = "--dry-run" ] && { dry=1; shift; }
prune=()
for keep in "$@"; do prune+=(-path "$T/$keep" -prune -o); done
find $T/runs* "${prune[@]}" -type f -size +1M \
  \( -name '*.dump' -o -name '*.mkv' -o -name '*.flv' -o -name '*.ps' -o -name '*.rtp' -o -name '*.h264' -o -name '*.h265' \
     -o -name '*.aac' -o -name '*.es' -o -name '*.ts' -o -name '*.wav' -o -name '*.pcm' -o -name '*.raw' \) \
  ! -path '*/d-fixtures/*' -print0 |
  if [ $dry = 1 ]; then du -ch --files0-from=- | tail -1; else xargs -0 -r rm -f; fi
df -h /var/tmp | tail -1
