#!/bin/sh
# Run on the phone from this directory: installs the UCM profile and the mainline PulseAudio selection
set -e
here=$(dirname "$0")
sudo rm -rf /usr/share/alsa/ucm/cosmo
sudo cp -r "$here/../ucm/cosmo" /usr/share/alsa/ucm/cosmo
sudo install -m 0644 "$here/default.pa.mainline" /etc/pulse/default.pa.mainline
sudo install -m 0755 "$here/pulseaudio-cosmo" /usr/local/bin/pulseaudio-cosmo
sudo install -D -m 0644 "$here/cosmo-mainline.conf" /etc/systemd/user/pulseaudio.service.d/cosmo-mainline.conf
