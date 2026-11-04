#!/bin/bash
# udp_transport_check.sh <check binary> [strace]: run as root. Checks media/net/udp_transport GSO batching:
#   1. grouping on loopback; with "strace" the GSO sendmsg (SOL_UDP control message) and plain sendto calls are counted.
#      The error case sends from 127.0.0.1 to an external address: EINVAL, which falls back to single sends (5 callbacks, as uncorked).
#   2. grouping in a namespace whose loopback has transmit checksum offload disabled, so every GSO send fails with EIO and falls
#      back to single sends; there the external address has no route (ENETUNREACH), reported once per failed send call (2 callbacks).
#   3. backpressure in a namespace whose loopback has a 20 Mbit/s tbf qdisc, so the socket send buffer fills and writes queue. tbf on
#      loopback releases its last queued packets only when another packet is enqueued, so the check keeps sending one byte to an
#      unused port from a separate socket while it waits for delivery.
set -u
bin=$(readlink -f $1)
trace=${2:-}
log=$(mktemp -d)
status=0
run() { if [ "$trace" = strace ]; then strace -f -e trace=sendto,sendmsg -o $log/$1.txt "${@:2}"; else "${@:2}"; fi || status=1; }
count() { if [ -e $log/$1.txt ]; then grep -c "$2" $log/$1.txt; else echo -; fi; }
run lo $bin grouping 127.0.0.1 5
echo "loopback: GSO sendmsg $(count lo SOL_UDP) (expected 6: 5 batches and 1 that fails with EINVAL), plain sendto $(count lo '^[0-9]* *sendto') (expected 13)"
if [ "$trace" = strace ]; then
  [ "$(count lo SOL_UDP)" = 6 ] && [ "$(count lo '^[0-9]* *sendto')" = 13 ] || status=1
  grep SOL_UDP $log/lo.txt | grep -o "iov_len=[0-9]*.*= -\?[0-9]*.*" | sed -E 's/iov_len=([0-9]+).*= (-?[0-9]+.*)/  gso batch bytes=\1 result=\2/'
fi
ns=udpcheck-$$
ip netns add $ns
ip -n $ns link set lo up
ip netns exec $ns ethtool -K lo tx off > /dev/null
run nocsum ip netns exec $ns $bin grouping 127.0.0.1 2
echo "checksum offload off: GSO sendmsg $(count nocsum SOL_UDP), of which EIO $(count nocsum 'SOL_UDP.*EIO')"
ip netns exec $ns ethtool -K lo tx on > /dev/null
tc -n $ns qdisc add dev lo root tbf rate 20mbit burst 32kbit limit 8mb
run bp ip netns exec $ns $bin backpressure 127.0.0.1
echo "backpressure: EAGAIN results $(count bp EAGAIN)"
ip netns del $ns
rm -rf $log
echo "overall: $([ $status = 0 ] && echo PASS || echo FAIL)"
exit $status
