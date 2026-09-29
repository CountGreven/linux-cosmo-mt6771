#!/usr/bin/env bash
# Install the core-off idle gate timer on the phone (usage: install.sh [ssh host]).
set -eu
H="${1:-cosmo-eth}"
D=$(dirname "$0")
scp -q "$D/cosmo-cpuidle.service" "$D/cosmo-cpuidle.timer" "$H:/tmp/"
ssh "$H" 'sudo install -m 644 /tmp/cosmo-cpuidle.service /tmp/cosmo-cpuidle.timer /etc/systemd/system/ &&
	sudo systemctl daemon-reload && sudo systemctl enable cosmo-cpuidle.timer && systemctl list-timers cosmo-cpuidle.timer --no-pager'
