#!/bin/sh
set -eu
bundle_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$bundle_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export MEDIA_SERVER_BENCH_HEAD=1ac4e042f0ced7520a21fddb024008975d2005aa
cd "$bundle_dir"
exec "$@"
