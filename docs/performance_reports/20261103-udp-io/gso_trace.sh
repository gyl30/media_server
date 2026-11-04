#!/bin/sh
program=$(cat <<'BT'
kprobe:udp_sendmsg /pid == $1/
{
  @udp_sendmsg = count();
  @udp_sendmsg_bytes = sum(arg2);
  @over_mtu_sendmsg = sum(arg2 > 1500 ? 1 : 0);
  @udp_sendmsg_len = hist(arg2);
}
kprobe:__udp_gso_segment { @gso_segment_calls = count(); }
kprobe:__udp_enqueue_schedule_skb { @delivered_datagrams = count(); }
interval:s:10 { exit(); }
BT
)
exec bpftrace -e "$program" "$@"
