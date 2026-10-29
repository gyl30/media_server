#!/bin/bash
api=http://${TEST_HOST:?}:38473
device=34020000001320000001
echo "== $(date +%T)"
echo "devices: $(curl -s -m 3 $api/api/devices)"
echo "channels: $(curl -s -m 3 $api/api/devices/$device/channels)"
echo "talk: $(curl -s -m 3 $api/api/devices/$device/talk)"
ssh -o BatchMode=yes root@${TEST_HOST:?} 'd=/var/tmp/gb-camera-e12340d-fBaRgeOC; t=$(cat $d/control.private); echo "receivers: $(curl -s -m 3 -H "Authorization: Bearer $t" http://${TEST_HOST:?}:38472/receivers)"; echo "sockets: $(ss -uanp | grep -c "pid=$(cat $d/media.pid),") udp, $(ss -tanp | grep "pid=$(cat $d/media.pid)," | grep -c ESTAB) tcp-estab"'
