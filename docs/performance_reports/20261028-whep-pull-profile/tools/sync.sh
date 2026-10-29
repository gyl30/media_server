#!/bin/bash
cd "$(dirname "$0")/.."
rsync -a -e "ssh -o BatchMode=yes" --exclude='source-ready-*' --exclude='publisher-*' --exclude='upstream-ready-*' --exclude='*.credentials' --exclude='*.token' \
    --include='wq-*/***' --include='d-wq-*/***' --include='d-wp-*/***' --include='p3-wq*/***' --include="*.json.gz" --include='wq-*.out' --include='d-w[pq]-*.out' --exclude='*' \
    root@${TEST_HOST:?}:/var/tmp/ms-perf-891aec1/runs/ raw/runs/
rsync -a -e "ssh -o BatchMode=yes" --exclude perf.data --include='wp-*/***' --include='wq*/***' --exclude='*' root@${TEST_HOST:?}:/var/tmp/ms-perf-891aec1/prof/ raw/prof/
