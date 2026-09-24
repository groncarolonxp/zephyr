#!/usr/bin/env bash
#
# vnet_pc_setup.sh - configure a PC NIC for point-to-point testing against the
# vnet sample board, and remove the default route that Linux auto-adds for the
# directly-connected subnet (so it does not interfere with the test wire).
#
# Usage:
#   sudo ./vnet_pc_setup.sh [IP] [NETIF]
#
# Defaults:
#   IP    = 192.0.2.10
#   NETIF = enp0s31f6
#
# The netmask is fixed at 255.255.255.0 (/24), matching the sample addressing
# (PC 192.0.2.10, cpu0 192.0.2.2, cpu1 192.0.2.1).

set -euo pipefail

# Positional parameters with defaults.
IP="${1:-192.0.2.10}"
NETIF="${2:-enp0s31f6}"
NETMASK="255.255.255.0"

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root (use sudo)." >&2
    exit 1
fi

echo "[setup] Bringing up ${NETIF} with ${IP} netmask ${NETMASK}"
nmcli device set "${NETIF}" managed no
ifconfig "${NETIF}" "${IP}" netmask "${NETMASK}" up

echo "[setup] Removing default route via ${NETIF} (if present)"
# Delete may fail if there is no such default route; do not abort in that case.
ip route del default dev "${NETIF}" 2>/dev/null || \
    echo "[setup] no default route via ${NETIF} to remove"

echo "[setup] Current addressing for ${NETIF}:"
ip addr show dev "${NETIF}"

echo "[setup] Done."
