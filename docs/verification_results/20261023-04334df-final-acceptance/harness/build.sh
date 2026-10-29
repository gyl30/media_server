#!/bin/bash
# usage: build.sh <build-dir> <output>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$1"
INC=$(sed -n 's/^CXX_INCLUDES = //p' CMakeFiles/media_core.dir/flags.make)
DEF=$(sed -n 's/^CXX_DEFINES = //p' CMakeFiles/media_core.dir/flags.make)
g++-16 -std=c++23 -O1 -g $DEF $INC -I"$here/../../../.." "$here/lp_check.cpp" -o "$2" libmedia_core.a libireader_media.a
