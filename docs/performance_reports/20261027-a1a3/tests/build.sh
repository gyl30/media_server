#!/bin/bash
# usage: build.sh <repo> <out-dir>; builds the tcp_transport test against <repo>: plain, tsan and asan+ubsan
set -eu
repo=$1
out=$2
mkdir -p $out
here=$(cd "$(dirname "$0")" && pwd)
common="-std=c++23 -g -Wall -Wextra -I$repo -I/usr/local/include -pthread $here/tcp_transport_test.cpp $repo/media/net/tcp_transport.cpp -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lboost_context"
/usr/bin/g++-16 -O2 $common -o $out/tcp_transport_test
/usr/bin/g++-16 -O1 -fsanitize=thread $common -o $out/tcp_transport_test_tsan
/usr/bin/g++-16 -O1 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-omit-frame-pointer $common -o $out/tcp_transport_test_asan
