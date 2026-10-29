#!/bin/bash
# usage: build.sh <repo> <out-dir>; builds the dispatcher test against <repo>'s headers: plain, tsan and asan+ubsan
set -eu
repo=$1
out=$2
mkdir -p $out
here=$(cd "$(dirname "$0")" && pwd)
common="-std=c++23 -g -Wall -Wextra -I$repo -I/usr/local/include -pthread $here/dispatcher_test.cpp $repo/media/net/worker_context.cpp"
/usr/bin/g++-16 -O2 $common -o $out/dispatcher_test
/usr/bin/g++-16 -O1 -fsanitize=thread $common -o $out/dispatcher_test_tsan
/usr/bin/g++-16 -O1 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-omit-frame-pointer $common -o $out/dispatcher_test_asan
