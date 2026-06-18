#!/bin/sh
# setup-bridge.sh - create a host bridge with tap ports so a soft PLC VM and the
# EtherNet/IP adapter VM share one L2 segment and can do Class 1 I/O.
#
#   [ soft PLC VM ] --tap0--\
#                            br0 (host, $HOST_IP)  <- same subnet, L2 connected
#   [ adapter  VM ] --tap1--/
#
# With both guests on br0, bidirectional UDP/2222, broadcast ListIdentity and
# multicast T->O all work natively. The host (br0 IP) can also reach both, so
# you can drive the adapter with tools/eip_originator.py from the host for a
# quick check before wiring up a real soft PLC.
#
# Requires root (creates a bridge + tap devices via iproute2).
#
# Usage:
#   sudo scripts/setup-bridge.sh up        # create br0 + tap0 tap1
#   sudo scripts/setup-bridge.sh status
#   sudo scripts/setup-bridge.sh down      # tear everything down
#
# Environment overrides:
#   BRIDGE   bridge name                 [default: br0]
#   SUBNET   /24 prefix                  [default: 192.168.77]
#   HOST_IP  host address on the bridge  [default: $SUBNET.1]
#   TAPS     space-separated tap names   [default: "tap0 tap1"]
#   OWNER    user allowed to open taps   [default: $SUDO_USER or current user]
#   DHCP     1 to also start dnsmasq DHCP on the bridge [default: 0]
set -eu

BRIDGE="${BRIDGE:-br0}"
SUBNET="${SUBNET:-192.168.77}"
HOST_IP="${HOST_IP:-$SUBNET.1}"
TAPS="${TAPS:-tap0 tap1}"
OWNER="${OWNER:-${SUDO_USER:-$(id -un)}}"
DHCP="${DHCP:-0}"
DNSMASQ_PID="/run/eip-dnsmasq.pid"

need_root() {
    [ "$(id -u)" = 0 ] || { echo "ERROR: must run as root (use sudo)." >&2; exit 1; }
}

case "${1:-}" in
  up)
    need_root
    command -v ip >/dev/null 2>&1 || { echo "ERROR: iproute2 'ip' not found." >&2; exit 1; }

    ip link show "$BRIDGE" >/dev/null 2>&1 || ip link add name "$BRIDGE" type bridge
    ip addr add "$HOST_IP/24" dev "$BRIDGE" 2>/dev/null || true
    ip link set "$BRIDGE" up

    for t in $TAPS; do
        ip link show "$t" >/dev/null 2>&1 || ip tuntap add dev "$t" mode tap user "$OWNER"
        ip link set "$t" master "$BRIDGE"
        ip link set "$t" up
    done

    if [ "$DHCP" = 1 ]; then
        command -v dnsmasq >/dev/null 2>&1 || { echo "WARN: dnsmasq not installed; skipping DHCP." >&2; DHCP=0; }
    fi
    if [ "$DHCP" = 1 ]; then
        dnsmasq --interface="$BRIDGE" --bind-interfaces --except-interface=lo \
                --dhcp-range="$SUBNET.50,$SUBNET.150,12h" \
                --pid-file="$DNSMASQ_PID"
        echo ">> dnsmasq DHCP serving $SUBNET.50-150 on $BRIDGE"
    fi

    echo ">> $BRIDGE up ($HOST_IP/24), taps: $TAPS, owner: $OWNER"
    echo "   Suggested static IPs:  soft PLC = $SUBNET.20 , adapter = $SUBNET.10"
    ;;

  down)
    need_root
    [ -f "$DNSMASQ_PID" ] && kill "$(cat "$DNSMASQ_PID")" 2>/dev/null || true
    rm -f "$DNSMASQ_PID" 2>/dev/null || true
    for t in $TAPS; do
        ip link set "$t" down 2>/dev/null || true
        ip tuntap del dev "$t" mode tap 2>/dev/null || true
    done
    ip link set "$BRIDGE" down 2>/dev/null || true
    ip link del "$BRIDGE" 2>/dev/null || true
    echo ">> $BRIDGE and taps removed"
    ;;

  status)
    ip addr show "$BRIDGE" 2>/dev/null || echo "no $BRIDGE"
    echo "--- bridge ports ---"
    bridge link 2>/dev/null || true
    ;;

  *)
    echo "usage: $0 up|down|status" >&2
    exit 1
    ;;
esac
