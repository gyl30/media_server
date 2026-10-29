# usage: source san_env.sh <asan|ubsan|tsan> <report-prefix>
san_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export LD_LIBRARY_PATH=$san_root/sanitizer-runtime
unset ASAN_OPTIONS UBSAN_OPTIONS TSAN_OPTIONS
case $1 in
    asan)
        export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:log_path=$2.asan"
        export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$2.ubsan"
        ;;
    ubsan)
        export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$2.ubsan"
        ;;
    tsan)
        export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:log_path=$2.tsan"
        ;;
esac
