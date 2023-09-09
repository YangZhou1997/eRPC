# !/bin/bash

ncpu=$1

sudo ethtool -N ens2f0np0 rx-flow-hash udp4 sdfn
sudo ethtool -L ens2f0np0 combined $ncpu

(let cnt=0; cd /sys/class/net/ens2f0np0/device/msi_irqs/;
  for IRQ in *; do
    let CPU=$((cnt*2+3))
    let cnt=$(((cnt+1)%ncpu))
    echo $IRQ '->' $CPU
    echo $CPU | sudo tee /proc/irq/$IRQ/smp_affinity_list > /dev/null
done)