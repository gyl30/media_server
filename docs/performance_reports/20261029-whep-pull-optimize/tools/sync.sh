#!/bin/bash
cd "$(dirname "$0")/.."
mkdir -p raw/runs raw/prof
rsync -a -e "ssh -o BatchMode=yes" --exclude='source-ready-*' --exclude='publisher-*' --exclude='upstream-ready-*' --exclude='*.credentials' --exclude='*.token' \
    --include='d-x-*/***' --include='f-[AL]-*/***' --include='e-[FP]-*/***' --include='av-[LF]-*/***' --include='d-x-*.out' --include='f-[AL]-*.out' --include='e-[FP]-*.out' --include='av-[LF]-*.out' --exclude='*' \
    root@${TEST_HOST:?}:/var/tmp/ms-perf-891aec1/runs/ raw/runs/
rsync -a -e "ssh -o BatchMode=yes" --exclude perf.data --include='x-*/***' --include='pf-*/***' --exclude='*' root@${TEST_HOST:?}:/var/tmp/ms-perf-891aec1/prof/ raw/prof/
