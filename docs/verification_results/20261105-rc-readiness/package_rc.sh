#!/bin/bash
# package_rc.sh <clean source checkout> <cmake build dir> <go build dir> <output dir>: packs media_server, signaling and the license texts
# of every linked component into <output dir>/media_server-<version>.tar.gz. The checkout must be clean and media_server --version must
# name its git describe, so a renamed or locally modified binary is refused.
set -euo pipefail
source=$(readlink -f "$1")
build=$(readlink -f "$2")
go_build=$(readlink -f "$3")
output=$(readlink -m "$4")
[ -z "$(git -C "$source" status --porcelain)" ] || { echo "source checkout is not clean" >&2; exit 1; }
describe=$(git -C "$source" describe --always --dirty --abbrev=12)
version=$("$build/media_server" --version)
case $version in "media_server $describe "*) ;; *) echo "media_server --version does not name $describe: $version" >&2; exit 1 ;; esac
[ "$(go version -m "$go_build/signaling" | awk '$2 == "vcs.revision=" || $2 ~ /^vcs.revision=/ {sub("vcs.revision=", "", $2); print $2}')" = "$(git -C "$source" rev-parse HEAD)" ] ||
    { echo "signaling was not built from $(git -C "$source" rev-parse HEAD)" >&2; exit 1; }
go version -m "$go_build/signaling" | grep -q "vcs.modified=false" || { echo "signaling was built from a modified tree" >&2; exit 1; }

name=media_server-$describe
root=$output/$name
mkdir -p "$output"
[ ! -e "$root" ] || { echo "$root already exists" >&2; exit 1; }
mkdir -p "$root/bin" "$root/licenses"
cp "$build/media_server" "$go_build/signaling" "$root/bin/"

license()
{
    mkdir -p "$root/licenses/$1"
    cp "${@:2}" "$root/licenses/$1/"
}
from_archive()
{
    mkdir -p "$root/licenses/$1"
    tar -xf "$source/deps/cache/$2" -C "$root/licenses/$1" --strip-components=1 "$3"
}
license ffmpeg "$source/third/ffmpeg/LICENSE.md" "$source/third/ffmpeg/COPYING.LGPLv2.1"
license fdk-aac "$source/third/fdk-aac/NOTICE"
license opus "$source/third/opus/COPYING"
license libsrtp "$source/third/libsrtp/LICENSE"
license ada "$source/third/ada/LICENSE-APACHE" "$source/third/ada/LICENSE-MIT"
license simdjson "$source/third/simdjson/LICENSE" "$source/third/simdjson/LICENSE-MIT"
license spdlog "$source/third/spdlog/LICENSE" "$source/third/spdlog/include/spdlog/fmt/bundled/fmt.license.rst"
license ireader "$source"/third/ireader/licenses/*
from_archive openssl openssl-3.5.9.tar.gz openssl-3.5.9/LICENSE.txt
from_archive boost boost_1_92_0.tar.gz boost_1_92_0/LICENSE_1_0.txt
from_archive jemalloc jemalloc-5.3.0.tar.bz2 jemalloc-5.3.0/COPYING
license go "$(go env GOROOT)/LICENSE"
modules=$(go env GOMODCACHE)
go version -m "$go_build/signaling" | awk '$1 == "dep" {print $2 "@" $3}' | while read -r module; do
    directory=$modules/$(echo "$module" | sed 's/[A-Z]/!\L&/g')
    files=$(find "$directory" -maxdepth 1 -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' \))
    [ -n "$files" ] || { echo "no license file for $module" >&2; exit 1; }
    license "go-modules/${module%@*}" $files
done

{
    echo "$version"
    echo "git $(git -C "$source" rev-parse HEAD)"
    git -C "$source" submodule status
    echo "deps $(head -1 "$source/deps/install/BUILD-INFO")"
    grep -E '^(CMAKE_BUILD_TYPE|CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|MEDIA_SERVER_HARDENING|MEDIA_SERVER_SANITIZER|MEDIA_SERVER_BUILD_BENCHMARKS):' "$build/CMakeCache.txt"
    go version "$go_build/signaling" | sed "s/^.*: /signaling /"
    readelf -n "$build/media_server" | grep "Build ID"
} > "$root/BUILD-INFO"
(cd "$root" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar -C "$output" --owner=0 --group=0 --numeric-owner --mtime="@$(git -C "$source" log -1 --format=%ct)" --sort=name -czf "$output/$name.tar.gz" "$name"
sha256sum "$output/$name.tar.gz"
