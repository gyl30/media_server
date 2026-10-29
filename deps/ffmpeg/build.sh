#!/bin/bash
set -euo pipefail

usage()
{
    echo "usage: $0 --prefix DIR [--patched] [--fdk-aac] [--cache DIR] [--jobs N]" >&2
    exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
prefix=""
patched=0
fdk_aac=0
cache="${XDG_CACHE_HOME:-$HOME/.cache}/media_server-deps"
jobs=$(nproc)
cc=${CC:-cc}
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) prefix=$2; shift 2 ;;
        --patched) patched=1; shift ;;
        --fdk-aac) fdk_aac=1; shift ;;
        --cache) cache=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$prefix" ] || usage
[ ! -e "$prefix" ] || { echo "prefix $prefix exists" >&2; exit 1; }
prefix=$(realpath -m "$prefix")
work=$prefix.build
[ ! -e "$work" ] || { echo "work directory $work exists" >&2; exit 1; }
mkdir -p "$cache" "$work"

fetch()
{
    read -r _ url sha256 < <(grep "^$1 " "$here/sources.txt")
    local archive=$cache/$(basename "$url")
    if [ ! -f "$archive" ]; then
        curl -fL --retry 3 -o "$archive.part" "$url" >&2
        mv "$archive.part" "$archive"
    fi
    echo "$sha256  $archive" | sha256sum -c --quiet
    tar -xf "$archive" -C "$work"
    echo "$work/$(tar -tf "$archive" | head -1 | cut -d/ -f1)"
}

opus=$(fetch opus)
(cd "$opus" && CC=$cc ./configure --prefix="$prefix" --enable-static --disable-shared --with-pic --disable-doc --disable-extra-programs && make -j"$jobs" && make install)

aac_encoders=aac
if [ "$fdk_aac" = 1 ]; then
    fdk=$(fetch fdk-aac)
    (cd "$fdk" && CC=$cc CXX=${CXX:-c++} ./configure --prefix="$prefix" --enable-static --disable-shared --with-pic && make -j"$jobs" && make install)
    aac_encoders=aac,libfdk_aac
fi

ffmpeg=$(fetch ffmpeg)
if [ "$patched" = 1 ]; then
    for patch in "$here"/patches/*.patch; do
        patch -d "$ffmpeg" -p1 --forward < "$patch"
    done
fi
(cd "$ffmpeg" && PKG_CONFIG_PATH="$prefix/lib/pkgconfig" ./configure \
    --prefix="$prefix" \
    --cc="$cc" \
    --pkg-config-flags=--static \
    --enable-static --disable-shared --enable-pic \
    --disable-programs --disable-doc \
    --disable-autodetect --disable-everything \
    --enable-libopus $([ "$fdk_aac" = 0 ] || echo --enable-libfdk-aac) \
    --enable-decoder=aac,opus \
    --enable-encoder=$aac_encoders,libopus,opus \
    --enable-parser=aac,opus,h264 \
    --enable-demuxer=matroska \
    --enable-protocol=file \
    --enable-swresample --enable-swscale &&
    make -j"$jobs" && make install)

{
    echo "patched=$patched fdk_aac=$fdk_aac"
    "$cc" --version | head -1
    sha256sum "$here"/sources.txt "$here"/build.sh
    [ "$patched" = 0 ] || sha256sum "$here"/patches/*.patch
    grep -E "FFMPEG_(CONFIGURATION|LICENSE)" "$ffmpeg/config.h"
} > "$prefix/BUILD-INFO"
rm -rf "$work"
echo "installed $prefix"
