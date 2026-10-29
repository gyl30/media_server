T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
RT=rt-wp $T/wp.sh wq-in150-none --input whep_pull --sources 150 --outputs none $U
RT=rt-wp $T/wp.sh wq-mono150 --input whep_pull --sources 150 --outputs flv:150 --fixture $T/fixtures-a1/perf_fixture_720p30_mono.flv $U
