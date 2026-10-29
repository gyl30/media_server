#!/bin/bash
U=/sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/current_freq_khz
P=/sys/class/powercap/intel-rapl:0/energy_uj
R=/sys/class/powercap/intel-rapl:0:0/energy_uj
T=$(grep -l "Package id 0" /sys/class/hwmon/hwmon*/temp*_label 2>/dev/null | head -1 | sed "s/_label/_input/")
pe=$(cat $P); re=$(cat $R); pt=$(date +%s.%N)
echo -e "unix\tuncore_mhz\tpkg_w\tdram_w\tcpu0_7_mhz\tpkg_temp_c"
while true; do
  sleep 5
  ne=$(cat $P); nr=$(cat $R); nt=$(date +%s.%N)
  mhz=$(for c in 0 1 2 3 4 5 6 7; do cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq; done | awk "{s+=\$1} END {printf \"%.0f\", s/NR/1000}")
  temp=$([ -n "$T" ] && awk "{printf \"%.0f\", \$1/1000}" $T || echo NA)
  awk -v t=$nt -v u=$(cat $U) -v pe=$pe -v ne=$ne -v re=$re -v nr=$nr -v pt=$pt -v mhz=$mhz -v temp=$temp "BEGIN {d=t-pt; printf \"%.0f\t%.0f\t%.1f\t%.1f\t%s\t%s\n\", t, u/1000, (ne-pe)/1e6/d, (nr-re)/1e6/d, mhz, temp}"
  pe=$ne; re=$nr; pt=$nt
done
