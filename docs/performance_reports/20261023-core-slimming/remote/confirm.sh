#!/bin/bash
T=/var/tmp/media_server_tooling/claude-20261023
export CLIENT_DIR=client2
for round in r1 r2; do
    for out in rtsp whep; do
        $T/ab.sh confirm-ps1-${out}500-$round variants/ps1/media_server $out:500
        $T/ab.sh confirm-fix1-${out}500-$round variants/fix1/media_server $out:500
    done
done
$T/ab.sh confirm-fix1-mixed variants/fix1/media_server rtmp:20,rtsp:20,flv:20,hls:20,whep:20,gb_udp:20,gb_tcp_passive:20,gb_tcp_active:20 --duration 20
