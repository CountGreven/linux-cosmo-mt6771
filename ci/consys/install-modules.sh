#!/usr/bin/env bash
# Install the consys wifi and bluetooth modules of the current image build on the phone, in
# /lib/modules/<release>, so udev loads the stack from the device tree ("mediatek,wifi" ->
# wlan_drv_gen3 and its dependencies). hci_stp has no device to match: modules-load.d names it.
# Only these: the rest of the module tree stays off the phone until it is wanted.
# The release string follows the git commit, so run this for every image that is flashed.
set -eu
T="${COSMO_BUILD:-/storage/kernel/build/ci-wt-wifi-bringup/build-image}"
B="$T/drivers/net/wireless/mediatek/mt6771-consys"
D="kernel/drivers/net/wireless/mediatek/mt6771-consys"
H="${COSMO_SSH:-cosmo-eth}"
R=$(strings "$T/arch/arm64/boot/Image" | grep -m1 -oE "^Linux version [^ ]+" | cut -d' ' -f3)
S=$(mktemp -d /storage/kernel/build/claude-tmp/consys-mods.XXXXXX)
trap 'rm -rf "${S:?}"' EXIT
for k in connadp.ko btif/btif_drv.ko common/wmt_drv.ko wlan/adaptor/wmt_chrdev_wifi.ko wlan/core/gen3/wlan_drv_gen3.ko \
         bt/hci_stp.ko; do
    [ "$(modinfo -F vermagic "$B/$k" | cut -d' ' -f1)" = "$R" ] || { echo "$k is not built for $R"; exit 1; }
    install -D -m 644 "$B/$k" "$S/$R/$D/$k"
    "${CROSS_COMPILE:-aarch64-linux-gnu-}strip" --strip-debug "$S/$R/$D/$k"
done
for k in net/bluetooth/bluetooth.ko net/bluetooth/rfcomm/rfcomm.ko net/bluetooth/bnep/bnep.ko \
         net/bluetooth/hidp/hidp.ko drivers/hid/uhid.ko crypto/ecdh_generic.ko crypto/kpp.ko crypto/ecc.ko \
         drivers/input/keyboard/mtk-pmic-keys.ko drivers/iio/light/stk3310.ko drivers/iio/magnetometer/af6133e.ko drivers/input/touchscreen/novatek-nvt-ts.ko drivers/leds/led-class-multicolor.ko drivers/pwm/pwm-mediatek.ko drivers/leds/led-class-flash.ko drivers/leds/rgb/leds-mt6370-rgb.ko drivers/leds/flash/leds-mt6370-flash.ko drivers/gpu/drm/scheduler/gpu-sched.ko drivers/gpu/drm/panfrost/panfrost.ko sound/soundcore.ko sound/core/snd.ko sound/core/snd-timer.ko sound/core/snd-pcm.ko sound/core/snd-compress.ko sound/core/snd-pcm-dmaengine.ko sound/soc/snd-soc-core.ko sound/soc/mediatek/common/snd-soc-mtk-common.ko sound/soc/mediatek/mt8183/snd-soc-mt8183-afe.ko sound/soc/codecs/snd-soc-mt6358.ko sound/soc/codecs/snd-soc-aw8738.ko sound/soc/codecs/snd-soc-simple-amplifier.ko sound/soc/codecs/snd-soc-mt6358-accdet.ko sound/soc/mediatek/mt8183/mt6771-cosmo.ko drivers/devfreq/mtk-cci-devfreq.ko drivers/devfreq/governor_passive.ko drivers/iio/buffer/kfifo_buf.ko \
         drivers/iio/buffer/industrialio-triggered-buffer.ko drivers/iio/imu/bmi160/bmi160_core.ko \
         drivers/iio/imu/bmi160/bmi160_spi.ko net/nfc/nfc.ko net/nfc/nci/nci.ko \
         drivers/nfc/nxp-nci/nxp-nci.ko drivers/nfc/nxp-nci/nxp-nci_i2c.ko drivers/input/ff-memless.ko drivers/input/misc/regulator-haptic.ko \
         drivers/gnss/gnss.ko drivers/net/wwan/wwan.ko drivers/net/wwan/mtk_md/mtk_md_proto.ko \
         drivers/net/wwan/mtk_md/mtk_md.ko; do
    install -D -m 644 "$T/$k" "$S/$R/kernel/$k"
    "${CROSS_COMPILE:-aarch64-linux-gnu-}strip" --strip-debug "$S/$R/kernel/$k"
done
for f in modules.order modules.builtin modules.builtin.modinfo; do [ -e "$T/$f" ] && cp "$T/$f" "$S/$R/"; done
echo "installing the wifi and bluetooth modules for $R"
tar -C "$S" -cf - "$R" | ssh -o BatchMode=yes "$H" "sudo -n tar -C /lib/modules -xf - --no-same-owner 2>/dev/null
    echo hci_stp | sudo -n tee /etc/modules-load.d/cosmo-bluetooth.conf >/dev/null
    # the modem is started by hand during bring-up: keep udev from loading its driver
    echo "blacklist mtk_md" | sudo -n tee /etc/modprobe.d/cosmo-modem.conf >/dev/null
    sudo -n depmod -a $R && grep -c mediatek,wifi /lib/modules/$R/modules.alias && ls /lib/modules"
