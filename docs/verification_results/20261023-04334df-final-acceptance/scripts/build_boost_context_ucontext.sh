#!/bin/bash
# usage: build_boost_context_ucontext.sh <src-dir> <prefix> <sanitizer-flags> <defines...>
set -euo pipefail
src=$1; prefix=$2; flags=$3; shift 3
mkdir -p "$prefix/lib/cmake/boost_context-1.92.0" "$prefix/obj"
ln -sfn /usr/local/include "$prefix/include"
cp /usr/local/lib/cmake/BoostDetectToolset-1.92.0.cmake "$prefix/lib/cmake/"
cp /usr/local/lib/cmake/boost_context-1.92.0/boost_context-config.cmake \
   /usr/local/lib/cmake/boost_context-1.92.0/boost_context-config-version.cmake \
   /usr/local/lib/cmake/boost_context-1.92.0/libboost_context-variant-static.cmake \
   "$prefix/lib/cmake/boost_context-1.92.0/"
defs=()
for d in "$@"; do defs+=("-D$d"); done
for f in fiber continuation posix/stack_traits; do
    /usr/bin/g++-16 -std=c++23 -O1 -g -fPIC $flags -fno-omit-frame-pointer "${defs[@]}" \
        -DBOOST_CONTEXT_SOURCE -DBOOST_CONTEXT_STATIC_LINK -I/usr/local/include \
        -c "$src/$f.cpp" -o "$prefix/obj/$(basename $f).o"
done
rm -f "$prefix/lib/libboost_context.a"
/usr/bin/gcc-ar-16 rcs "$prefix/lib/libboost_context.a" "$prefix"/obj/*.o
nm -C "$prefix/lib/libboost_context.a" | grep -E "fiber_activation_record::current|__sanitizer_|__tsan_" || true
sha256sum "$prefix/lib/libboost_context.a"
