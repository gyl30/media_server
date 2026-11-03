T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
RT=$T/rt-g
R=$T/runs7
mkdir -p $R
bash $T/cleanup.sh runs7 > /dev/null
CHROME=$T/chrome-dl/root/opt/google/chrome/chrome
ready() { rm -rf $R/$1; bash $T/env_check.sh $R/$1.env.json > /dev/null || { echo "$1 BLOCKED"; return 1; }; }
san_env() {
  case $1 in
    asan) echo "ASAN_OPTIONS=log_path=$R/$2/sanitizer:detect_leaks=1";;
    tsan) echo "TSAN_OPTIONS=log_path=$R/$2/sanitizer";;
    *) echo "NONE=1";;
  esac
}
san_bin() { case $1 in asan|tsan) echo san/media_server-$1;; *) echo build/media_server;; esac; }
san_report() {
  echo "sanitizer files: $(ls $R/$1 2>/dev/null | grep -c '^sanitizer')"
  cat $R/$1/sanitizer* 2>/dev/null | grep -v "makecontext/swapcontext\|Boost.Context\|^==[0-9]*==WARNING: ASan doesn't fully support\|^==[0-9]*==WARNING: ASan is ignoring requested\|False positive error reports may follow\|For details see" | grep -v "^$" | head -40
}
