#!/bin/bash
# QEMU boot of the MPC5200B Vestas BSP, with host->guest TCP forwards
# so we can probe daemons (FTP=21, NFS=2049, portmap=111, WDB=17185).
# Phase 0a of plan-2026-04-30 (confirm-and-bypass Vestas-app spawn gate).

set -u
cd "$(dirname "$0")"

 ninja -C build qemu-system-ppc

LOG="${LOG:-/tmp/qemu_post_gate.log}"
PCAP="${PCAP:-/tmp/qemu_post_gate.bin}"
TIMEOUT="${TIMEOUT:-90}"

timeout "${TIMEOUT}" ./build/qemu-system-ppc \
    -machine mac99 -cpu mpc5200 -m 256 \
    -device loader,file=/tmp/vxworks_romfs/vxworks.out,cpu-num=0 \
    -nic user,id=n0,model=mpc5200-fec,mac=00:1b:f0:00:00:0a,\
net=169.254.254.0/24,host=169.254.254.252,\
hostfwd=tcp::2121-169.254.254.254:21,\
hostfwd=tcp::2049-169.254.254.254:2049,\
hostfwd=tcp::3111-169.254.254.254:111,\
hostfwd=tcp::17185-169.254.254.254:17185,\
hostfwd=tcp::9482-169.254.254.254:9482,\
hostfwd=tcp::8080-169.254.254.254:8080 \
    -object filter-dump,id=f0,netdev=n0,file="${PCAP}" \
    -display none -serial null \
    2>"${LOG}"

echo "QEMU exited. Log: ${LOG}  Pcap: ${PCAP}"
