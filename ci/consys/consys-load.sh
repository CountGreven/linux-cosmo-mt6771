#!/usr/bin/env bash
# Copy the four wifi modules to the phone and insert them one by one, reading dmesg after each.
set -u
T=/storage/kernel/build/ci-wt-wifi-bringup/build-image
B=$T/drivers/net/wireless/mediatek/mt6771-consys
H="${COSMO_SSH:-cosmo-eth}"
ssh_() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$H" "$@"; }
ssh_ 'mkdir -p ~/consys'
for k in connadp.ko btif/btif_drv.ko common/wmt_drv.ko wlan/adaptor/wmt_chrdev_wifi.ko wlan/core/gen3/wlan_drv_gen3.ko; do
    scp -q "$B/$k" "$H:~/consys/" || { echo "copy failed: $k"; exit 1; }
done
ssh_ 'sudo -n dmesg -C; for m in connadp btif_drv wmt_drv wmt_chrdev_wifi wlan_drv_gen3; do
    echo "=== insmod $m"; sudo -n insmod ~/consys/$m.ko 2>&1; sleep 2
    sudo -n dmesg | tail -n 25 | cut -c1-160; sudo -n dmesg -C; done
echo "=== loaded:"; lsmod | grep -E "btif_drv|connadp|wmt|wlan"; ls -la /dev/wmtdetect /dev/stpwmt /dev/wmtWifi 2>&1'
