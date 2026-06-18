#!/bin/sh
# run-qemu.sh - boot the EtherNet/IP adapter inside QEMU.
#
# Two networking modes:
#
#   tap  (recommended for real Class 1 I/O)
#       Gives the guest a real L2 interface on a host bridge/tap, so
#       bidirectional UDP/2222, broadcast ListIdentity and multicast all work.
#       Requires a preconfigured tap device (TAP=tap0) and usually root.
#
#   user (zero-config, limited)
#       Uses QEMU user-mode networking with host port forwards for the
#       explicit (TCP/44818) and implicit (UDP/2222) ports. Good for a quick
#       smoke test from the host, but NAT does not pass broadcast discovery and
#       target-initiated UDP to an arbitrary originator is constrained.
#
# Environment overrides:
#   KERNEL    path to a bootable kernel (bzImage)   [default: /boot/vmlinuz-$(uname -r)]
#   INITRD    path to the initramfs                 [default: build/initramfs.cpio.gz]
#   MODE      tap | user                            [default: user]
#   TAP       tap device name (MODE=tap)            [default: tap0]
#   MEM       guest RAM                             [default: 256M]
#   QEMU      qemu binary                           [default: qemu-system-x86_64]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL="${KERNEL:-/boot/vmlinuz-$(uname -r)}"
INITRD="${INITRD:-$ROOT/build/initramfs.cpio.gz}"
MODE="${MODE:-user}"
TAP="${TAP:-tap0}"
MEM="${MEM:-256M}"
QEMU="${QEMU:-qemu-system-x86_64}"

[ -f "$INITRD" ] || { echo "ERROR: initramfs '$INITRD' missing. Run scripts/make-initramfs.sh first." >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "ERROR: kernel '$KERNEL' not found. Set KERNEL=/path/to/bzImage." >&2; exit 1; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "ERROR: $QEMU not installed." >&2; exit 1; }

APPEND="console=ttyS0 quiet"

case "$MODE" in
  tap)
    NET="-netdev tap,id=n0,ifname=$TAP,script=no,downscript=no -device e1000,netdev=n0"
    echo ">> MODE=tap via $TAP (ensure it is up and bridged to your network)"
    ;;
  user)
    # Forward host:44818 -> guest:44818 (TCP) and host:2222 -> guest:2222 (UDP).
    NET="-netdev user,id=n0,hostfwd=tcp::44818-:44818,hostfwd=udp::2222-:2222 -device e1000,netdev=n0"
    echo ">> MODE=user with host port forwards tcp/44818 and udp/2222"
    echo "   (broadcast ListIdentity discovery will NOT traverse user-mode NAT)"
    ;;
  *) echo "ERROR: unknown MODE=$MODE (use tap or user)" >&2; exit 1 ;;
esac

echo ">> Booting QEMU (Ctrl-A X to quit)"
# shellcheck disable=SC2086
exec "$QEMU" \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "$APPEND" \
    -m "$MEM" \
    -nographic \
    $NET
