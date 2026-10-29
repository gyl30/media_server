#!/bin/bash
set -euo pipefail

usage()
{
    echo "usage: $0 [--jobs N]" >&2
    exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
prefix=$here/install
work=$prefix.build
cache=$here/cache
jobs=$(nproc)
cc=${CC:-cc}
cxx=${CXX:-c++}
while [ $# -gt 0 ]; do
    case $1 in
        --jobs) jobs=$2; shift 2 ;;
        *) usage ;;
    esac
done
[ ! -f "$prefix/BUILD-INFO" ] || { echo "$prefix is already built; remove it to rebuild" >&2; exit 1; }
rm -rf "$prefix" "$work"
trap 'rm -rf "$work"; [ -f "$prefix/BUILD-INFO" ] || rm -rf "$prefix"' EXIT
mkdir -p "$cache" "$work"

fetch()
{
    local url sha256
    read -r _ url sha256 < <(grep "^$1 " "$here/sources.txt")
    local archive
    archive=$cache/$(basename "$url")
    if [ ! -f "$archive" ]; then
        curl -fL --retry 3 -o "$archive.part" "$url"
        echo "$sha256  $archive.part" | sha256sum -c --quiet
        mv "$archive.part" "$archive"
    fi
    echo "$sha256  $archive" | sha256sum -c --quiet
    mkdir "$work/$1"
    tar -xf "$archive" -C "$work/$1" --strip-components=1
}

fetch openssl
(cd "$work/openssl" &&
    ./Configure --prefix="$prefix" --libdir=lib --openssldir="$prefix/ssl" CC="$cc" no-shared no-tests no-docs -fPIC &&
    make -j"$jobs" && make install_sw)

fetch jemalloc
(cd "$work/jemalloc" &&
    ./configure --prefix="$prefix" CC="$cc" CXX="$cxx" EXTRA_CFLAGS=-fPIC EXTRA_CXXFLAGS=-fPIC \
        --disable-shared --disable-cxx --enable-stats --enable-prof &&
    make -j"$jobs" && make install)

fetch boost
echo "using gcc : : $cxx ;" > "$work/user-config.jam"
(cd "$work/boost" &&
    ./bootstrap.sh --with-libraries=context,program_options &&
    ./b2 --user-config="$work/user-config.jam" --build-dir="$work/b2" --prefix="$prefix" -j"$jobs" \
        variant=release link=static threading=multi cxxflags=-fPIC install &&
    ./b2 --user-config="$work/user-config.jam" --build-dir="$work/b2-asan" --prefix="$prefix/asan" --includedir="$prefix/include" \
        -j"$jobs" --with-context variant=release link=static threading=multi context-impl=ucontext \
        define=BOOST_USE_UCONTEXT define=BOOST_USE_ASAN "cxxflags=-fPIC -fsanitize=address,undefined -fno-omit-frame-pointer" install &&
    ./b2 --user-config="$work/user-config.jam" --build-dir="$work/b2-tsan" --prefix="$prefix/tsan" --includedir="$prefix/include" \
        -j"$jobs" --with-context variant=release link=static threading=multi context-impl=ucontext \
        define=BOOST_USE_UCONTEXT define=BOOST_USE_TSAN "cxxflags=-fPIC -fsanitize=thread" install)

{
    echo "inputs $(cat "$here/sources.txt" "$here/build.sh" | sha256sum | cut -d' ' -f1)"
    echo "cc $("$cc" --version | head -1)"
    echo "cxx $("$cxx" --version | head -1)"
} > "$prefix/BUILD-INFO"
echo "installed $prefix"
