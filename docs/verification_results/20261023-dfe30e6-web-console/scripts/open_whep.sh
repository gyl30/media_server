#!/bin/bash
log=runs/base-1/media.log
comm -23 <(grep -o "whep session created [0-9a-f]*" $log | awk '{print $4}' | sort -u) <(grep -o "whep session shutdown [0-9a-f]*" $log | awk '{print $4}' | sort -u) | wc -l
