T=/var/tmp/ms-perf-891aec1
RT=rt-wp $T/prof3.sh wq150-seq --input whep_pull --sources 150 --outputs flv:150 --upstream-workers 16 --perf-stat --ready-timeout 300 --ramp-per-second 200 --group-size 256
