#!/bin/sh
program=$(cat <<'BT'
/*
 * udp_trace.bt <server pid>: 10 s of server syscall counts and time, UDP datagram sizes and, per thread, runs of consecutive
 * sendto/sendmsg calls on the same fd with no other syscall in between (the burst a per-socket batch could cover without waiting).
 * x86_64 ids: 0 read, 1 write, 20 writev, 44 sendto, 45 recvfrom, 46 sendmsg, 47 recvmsg, 232 epoll_wait, 281 epoll_pwait, 307 sendmmsg.
 */
tracepoint:raw_syscalls:sys_enter /pid == $1/
{
  @start[tid] = nsecs;
  @id[tid] = args->id;
  if (args->id == 44 || args->id == 46) {
    $fd = (int64)args->args[0];
    if (@run[tid] > 0 && $fd == @lastfd[tid]) {
      @run[tid] = @run[tid] + 1;
    } else {
      if (@run[tid] > 0) { @burst = lhist(@run[tid], 1, 33, 1); }
      @run[tid] = 1;
      @lastfd[tid] = $fd;
    }
  } else {
    if (@run[tid] > 0) { @burst = lhist(@run[tid], 1, 33, 1); @run[tid] = 0; }
  }
}

tracepoint:raw_syscalls:sys_exit /pid == $1 && @start[tid]/
{
  $id = @id[tid];
  @calls[$id] = count();
  @ns[$id] = sum(nsecs - @start[tid]);
  delete(@start[tid]);
  delete(@id[tid]);
}

kprobe:udp_sendmsg /pid == $1/ { @udp_tx_len = lhist(arg2, 0, 1600, 100); }
kretprobe:udp_recvmsg /pid == $1 && (int64)retval > 0/ { @udp_rx_len = lhist(retval, 0, 1600, 100); }
kprobe:tcp_sendmsg /pid == $1/ { @tcp_tx_len = hist(arg2); }

interval:s:10
{
  exit();
}
BT
)
exec bpftrace -e "$program" "$@"
