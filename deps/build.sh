#!/bin/bash
set -euo pipefail

usage()
{
    echo "usage: $0 [--jobs N]" >&2
    exit 2
}

here=$(cd "$(dirname "$0")" && pwd)
prefix=$here/install
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
[ ! -e "$prefix" ] || { echo "prefix $prefix exists" >&2; exit 1; }
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

openssl=$(fetch openssl)
(cd "$openssl" &&
    ./Configure --prefix="$prefix" --libdir=lib --openssldir="$prefix/ssl" CC="$cc" no-shared no-tests no-docs -fPIC &&
    make -j"$jobs" && make install_sw)

jemalloc=$(fetch jemalloc)
(cd "$jemalloc" &&
    ./configure --prefix="$prefix" CC="$cc" CXX="$cxx" EXTRA_CFLAGS=-fPIC EXTRA_CXXFLAGS=-fPIC \
        --disable-shared --enable-stats --enable-prof &&
    make -j"$jobs" && make install)

boost=$(fetch boost)
echo "using gcc : : $cxx ;" > "$work/user-config.jam"
(cd "$boost" &&
    ./bootstrap.sh --with-libraries=context,program_options &&
    ./b2 --user-config="$work/user-config.jam" --build-dir="$work/b2" --prefix="$prefix" -j"$jobs" \
        variant=release link=static threading=multi cxxflags=-fPIC install &&
    ./b2 --user-config="$work/user-config.jam" --build-dir="$work/b2-asan" --prefix="$prefix/asan" --includedir="$prefix/include" \
        -j"$jobs" --with-context variant=release link=static threading=multi context-impl=ucontext \
        define=BOOST_USE_UCONTEXT define=BOOST_USE_ASAN "cxxflags=-fPIC -fsanitize=address,undefined -fno-omit-frame-pointer" install)

{
    "$cc" --version | head -1
    "$cxx" --version | head -1
    sha256sum "$here"/sources.txt "$here"/build.sh
} > "$prefix/BUILD-INFO"
rm -rf "$work"
echo "installed $prefix"
