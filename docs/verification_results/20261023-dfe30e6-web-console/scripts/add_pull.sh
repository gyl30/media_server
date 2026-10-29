#!/bin/bash
# usage: add_pull.sh <choice> <name> <url>   (base UI: devices page add dialog)
D=$(dirname "$0")/d
$D goto 'url=http://127.0.0.1:39090/#devices' >/dev/null; $D wait ms=500 >/dev/null
$D click 'sel=.page-head button' >/dev/null; $D wait ms=300 >/dev/null
$D select 'sel=#device-dialog select[name=protocol]' -- "$1" >/dev/null
$D fill 'sel=#device-dialog input[name=name]' -- "$2" >/dev/null
$D fill 'sel=#device-dialog input[name=url]' -- "$3" >/dev/null
$D click 'sel=#device-dialog button[type=submit]'
$D wait ms=800 >/dev/null
$D eval -- 'document.querySelector("#device-dialog").open ? document.querySelector("#device-dialog").innerText : "closed"'
