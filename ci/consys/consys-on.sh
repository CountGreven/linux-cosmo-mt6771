#!/usr/bin/env bash
# After consys-load.sh: replay wmt_loader and wmt_launcher, then turn wifi on and watch.
set -u
H="${COSMO_SSH:-cosmo-eth}"; S=$(dirname "$0")
scp -q "$S/wmt_loader.py" "$S/wmt_launcher.py" "$H:~/consys/"
ssh -o BatchMode=yes "$H" 'sudo -n dmesg -C; echo "=== loader"; sudo -n python3 ~/consys/wmt_loader.py; sleep 1; sudo -n dmesg | cut -c1-160 | tail -30; sudo -n dmesg -C
echo "=== launcher"; sudo -n python3 ~/consys/wmt_launcher.py; sleep 1; sudo -n dmesg | cut -c1-160 | tail -30; sudo -n dmesg -C
echo "=== wifi on"; sudo -n sh -c "echo 1 > /dev/wmtWifi"; sleep 8; echo "--- irqs:"; grep -E "btif|1100c000|apdma|consys|18070000" /proc/interrupts | cut -c1-120; echo "--- XO_WCN:"; sudo -n grep -E "^0788: " /sys/kernel/debug/regmap/1000d000.pwrap/registers; sudo -n dmesg | grep -E "XO_WCN|STP-DBG.*Rx|in\([0-9]+\), out" | head -8 | cut -c1-150; sudo -n dmesg | cut -c1-170 | tail -40; ip link | grep -E "wlan|ap0|p2p" ; iw dev 2>/dev/null | head'
