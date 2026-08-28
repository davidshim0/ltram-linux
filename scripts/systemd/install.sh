#!/bin/bash
# Install the erase-count persistence unit on z08. Run ON z08.
set -euo pipefail
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
sudo install -m 0755 "$D/ltram-erase-counts" /usr/local/sbin/ltram-erase-counts
sudo install -m 0644 "$D/ltram-erase-counts.service"      /etc/systemd/system/
sudo install -m 0644 "$D/ltram-erase-counts-save.service" /etc/systemd/system/
sudo install -m 0644 "$D/ltram-erase-counts.timer"        /etc/systemd/system/
sudo mkdir -p /var/lib/ltram
sudo systemctl daemon-reload
sudo systemctl enable --now ltram-erase-counts.service
echo
echo "installed and enabled. optional 10-minute checkpoint:"
echo "    sudo systemctl enable --now ltram-erase-counts.timer"
echo
systemctl status ltram-erase-counts.service --no-pager -l | head -12
