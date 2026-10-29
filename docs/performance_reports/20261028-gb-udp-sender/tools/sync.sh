#!/bin/bash
# usage: sync.sh; copy gb-* runs from the test host without credentials or publisher material
cd "$(dirname "$0")/.."
rsync -a -e "ssh -o BatchMode=yes" --exclude='source-ready-*' --exclude='publisher-*' --exclude='upstream-ready-*' --exclude='*.credentials' \
    --exclude='*.token' --include='gb-*/***' --include='gb-*' --exclude='*' root@${TEST_HOST:?}:/var/tmp/ms-perf-891aec1/runs/ raw/runs/
find raw/runs \( -name "*.credentials" -o -name "*.token" \) | wc -l
