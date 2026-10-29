#!/bin/bash
# usage: scan_reports.sh <dir>... : sanitizer diagnostics in report files and server logs
for dir in "$@"; do
    files=$(find "$dir" -type f \( -name "*.asan.*" -o -name "*.ubsan.*" -o -name "*.tsan.*" -o -name "*.log" -o -name "*.stderr" \) | sort)
    diagnostics=$(grep -h -E "ERROR: (AddressSanitizer|LeakSanitizer|ThreadSanitizer)|WARNING: ThreadSanitizer|runtime error:|SUMMARY: |CHECK failed|DEADLYSIGNAL" $files /dev/null 2>/dev/null)
    count=$(printf "%s" "$diagnostics" | grep -c -E "ERROR: |WARNING: ThreadSanitizer|runtime error:|CHECK failed|DEADLYSIGNAL")
    echo "$dir sanitizer_diagnostics=$count report_files=$(echo "$files" | grep -c -E 'san\.[0-9]+$')"
    printf "%s\n" "$diagnostics" | grep -E "SUMMARY: |runtime error:|CHECK failed" | sed -E 's#/home/gyl/example/media_server/##' | sort | uniq -c
done
