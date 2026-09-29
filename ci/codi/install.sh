#!/bin/sh
# Run on the phone: makes codi-app work on the mainline kernel (keeps working on the vendor one).
# Originals are kept as *.py.orig; running it twice is harmless.
set -e
here=$(dirname "$0")
L=/usr/lib/codi
sudo install -m 0644 "$here/cosmo_hw.py" "$L/cosmo_hw.py"
for f in codiReset.py codiUpdate.py CodiStatus.py LEDManager.py; do
	[ -f "$L/$f.orig" ] || sudo cp -p "$L/$f" "$L/$f.orig"
	python3 - "$L/$f.orig" /tmp/codi-$f <<'PY'
import sys
src, dst = sys.argv[1:3]
lines = open(src).read().replace("open('/proc/", "cosmo_hw.proc_open('/proc/").splitlines(True)
at = 1 if lines and lines[0].startswith("#!") else 0
lines.insert(at, "import cosmo_hw\n")
open(dst, "w").write("".join(lines))
PY
	sudo install -m "$(stat -c %a "$L/$f.orig")" /tmp/codi-$f "$L/$f"
	rm -f /tmp/codi-$f
done
sudo install -m 0644 "$here/cosmo_usb.py" "$L/cosmo_usb.py"
for f in codiServer.py codi_st32_generated_functions.py; do
	[ -f "$L/$f.orig" ] || sudo cp -p "$L/$f" "$L/$f.orig"
	python3 "$here/codi_patch.py" "$f" "$L/$f.orig" /tmp/codi-$f
	sudo install -m "$(stat -c %a "$L/$f.orig")" /tmp/codi-$f "$L/$f"
	rm -f /tmp/codi-$f
done
sudo install -m 0644 "$here/cosmo-codi-reset.service" /etc/systemd/system/cosmo-codi-reset.service
sudo systemctl daemon-reload
sudo systemctl enable cosmo-codi-reset.service
sudo install -m 0755 "$here/right-usb-otg.py" "$L/right-usb-otg.py"
sudo install -m 0644 "$here/cosmo-right-usb@.service" /etc/systemd/system/cosmo-right-usb@.service
sudo install -m 0644 "$here/90-cosmo-right-usb.rules" /etc/udev/rules.d/90-cosmo-right-usb.rules
sudo systemctl daemon-reload
sudo udevadm control --reload
