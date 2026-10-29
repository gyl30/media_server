#!/bin/bash
# usage: build.sh <impl.c> <output-prefix> : builds <prefix>-tsan, <prefix>-asan, <prefix>-plain linked with the 3cbc170 copy as old_rfc822_datetime_format
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd $H/../../../.. && pwd)
impl=$1; out=$2
cc=/usr/bin/gcc-16
common="-std=gnu11 -g -Wall -Werror=implicit-function-declaration -I$R/third/ireader/libhttp/include"
for kind in tsan asan plain; do
    case $kind in
        tsan) flags="-O2 -fsanitize=thread";;
        asan) flags="-O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer";;
        plain) flags="-O2";;
    esac
    $cc $common $flags -c "$impl" -o $out-$kind-impl.o
    $cc $common $flags -Drfc822_datetime_format=old_rfc822_datetime_format -c $H/rfc822-datetime-3cbc170.c -o $out-$kind-old.o
    $cc $common $flags $H/rfc822_check.c $out-$kind-impl.o $out-$kind-old.o -lpthread -o $out-$kind
    rm -f $out-$kind-impl.o $out-$kind-old.o
done
