#!/bin/bash
# udp_transport_check.sh <check binary> [strace]: run as root. Checks media/net/udp_transport on the synchronous send path:
#   1. ordering on loopback: mixed sizes and two endpoints arrive unchanged and in order; with "strace" every datagram is one sendto.
#      The error case sends from 127.0.0.1 to an external address: each send fails with EINVAL and is reported once (5 callbacks).
#   2. backpressure in a namespace whose loopback has a 20 Mbit/s tbf qdisc, so the socket send buffer fills, writes queue behind
#      async_send_to and the 1 MiB high-water mark rejects new datagrams. tbf on loopback releases its last queued packets only when
#      another packet is enqueued, so the check keeps sending one byte to an unused port from a separate socket while it waits.
set -u
bin=$(readlink -f $1)
trace=${2:-}
log=$(mktemp -d)
status=0
run() { if [ "$trace" = strace ]; then strace -f -e trace=sendto,sendmsg,sendmmsg -o $log/$1.txt "${@:2}"; else "${@:2}"; fi || status=1; }
count() { if [ -e $log/$1.txt ]; then grep -c "$2" $log/$1.txt; else echo -; fi; }
run lo $bin ordering 127.0.0.1 5
echo "loopback: sendto $(count lo '^[0-9]* *sendto') (expected 86: 81 datagrams and 5 failed sends), sendmsg/sendmmsg $(count lo 'sendm') (expected 0)"
if [ "$trace" = strace ]; then
  [ "$(count lo '^[0-9]* *sendto')" = 86 ] && [ "$(count lo 'sendm')" = 0 ] || status=1
fi
ns=udpcheck-$$
ip netns add $ns
ip -n $ns link set lo up
tc -n $ns qdisc add dev lo root tbf rate 20mbit burst 32kbit limit 8mb
run bp ip netns exec $ns $bin backpressure 127.0.0.1
echo "backpressure: EAGAIN results $(count bp EAGAIN)"
ip netns del $ns
rm -rf $log
echo "overall: $([ $status = 0 ] && echo PASS || echo FAIL)"
exit $status
