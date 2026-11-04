#!/bin/bash
# build_check.sh <build directory> <check source> <output binary>: compiles a check against that tree's libmedia_core.a with the
# compile flags of media/net/udp_transport.cpp and links it with the link line of whep_fanout.
#   srtp_profile_check.cpp drives the production dtls_transport/srtp_transport as DTLS server against an OpenSSL client that offers
#   one SRTP profile at a time, and decrypts both directions with libsrtp sessions whose policies and key sizes the check writes out.
#   udp_transport_check.cpp is run by udp_transport_check.sh.
set -eu
build=$(readlink -f "$1")
source=$(readlink -f "$2")
output=$(readlink -m "$3")
compile=$(python3 -c "
import json, sys
for entry in json.load(open(sys.argv[1])):
    if entry['file'].endswith('media/net/udp_transport.cpp'):
        print(entry['command'].split(' -o ')[0])
" "$build/compile_commands.json")
link=$(sed -E 's/ CMakeFiles\/whep_fanout\.dir\/[^ ]+\.o//g; s/ -o whep_fanout//; s/^[^ ]+ //' "$build/CMakeFiles/whep_fanout.dir/link.txt")
object=$(mktemp --suffix=.o)
$compile -o "$object" -c "$source"
cd "$build"
${compile%% *} "$object" -o "$output" $link
rm -f "$object"
