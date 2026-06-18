#!/bin/sh
# make-initramfs.sh - build a minimal initramfs that boots straight into the
# EtherNet/IP adapter, suitable for running under QEMU.
#
# The image brings up eth0 (DHCP by default, or a static IP) and then execs
# the statically-linked adapter. busybox is used for the shell/networking
# utilities; if it is not found the build aborts with instructions.
#
# Output: build/initramfs.cpio.gz
#
# Environment overrides:
#   IP_MODE   dhcp | static            (default: dhcp)
#   STATIC_IP CIDR, e.g. 192.168.1.50/24 (used when IP_MODE=static)
#   GATEWAY   gateway ip               (optional, static mode)
#   BUSYBOX   path to a busybox binary (default: $(command -v busybox))
#   CONFIG_FILE   adapter config file to embed at /etc/eip-adapter.conf
#   ADAPTER_ARGS  extra args passed to eip_adapter (e.g. "--no-ot-run-idle")
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
ROOTFS="$BUILD/initramfs"
IP_MODE="${IP_MODE:-dhcp}"
STATIC_IP="${STATIC_IP:-192.168.1.50/24}"
GATEWAY="${GATEWAY:-}"
BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"
CONFIG_FILE="${CONFIG_FILE:-}"
ADAPTER_ARGS="${ADAPTER_ARGS:-}"

echo ">> Building static adapter binary"
make -C "$ROOT" static

if [ -z "$BUSYBOX" ] || [ ! -x "$BUSYBOX" ]; then
    echo "ERROR: busybox not found. Install it (e.g. 'apt-get install busybox-static')" >&2
    echo "       or set BUSYBOX=/path/to/static/busybox" >&2
    exit 1
fi

echo ">> Assembling rootfs in $ROOTFS"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/bin "$ROOTFS"/sbin "$ROOTFS"/proc "$ROOTFS"/sys "$ROOTFS"/dev

cp "$ROOT/eip_adapter" "$ROOTFS/bin/eip_adapter"
cp "$BUSYBOX" "$ROOTFS/bin/busybox"

CONFIG_ARG=""
if [ -n "$CONFIG_FILE" ]; then
    [ -f "$CONFIG_FILE" ] || { echo "ERROR: CONFIG_FILE '$CONFIG_FILE' not found" >&2; exit 1; }
    mkdir -p "$ROOTFS/etc"
    cp "$CONFIG_FILE" "$ROOTFS/etc/eip-adapter.conf"
    CONFIG_ARG="--config /etc/eip-adapter.conf"
    echo ">> Embedded config: $CONFIG_FILE -> /etc/eip-adapter.conf"
fi
for applet in sh mount ip ifconfig udhcpc ls cat poweroff sleep; do
    ln -sf busybox "$ROOTFS/bin/$applet"
done

# udhcpc needs a default script; busybox ships a simple one expectation
mkdir -p "$ROOTFS/usr/share/udhcpc"
cat > "$ROOTFS/usr/share/udhcpc/default.script" <<'UDHCP'
#!/bin/sh
[ -n "$1" ] || exit 1
case "$1" in
  deconfig) ip addr flush dev "$interface" 2>/dev/null ;;
  bound|renew)
    ip addr add "$ip/${mask:-24}" dev "$interface" 2>/dev/null || \
      ifconfig "$interface" "$ip" netmask "${subnet:-255.255.255.0}"
    [ -n "$router" ] && ip route add default via "$router" 2>/dev/null
    ;;
esac
UDHCP
chmod +x "$ROOTFS/usr/share/udhcpc/default.script"

cat > "$ROOTFS/init" <<INIT
#!/bin/busybox sh
/bin/busybox --install -s /bin 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null

echo "=== QEMU EtherNet/IP adapter initramfs ==="
ip link set lo up
ip link set eth0 up

if [ "$IP_MODE" = "static" ]; then
    ip addr add $STATIC_IP dev eth0
    [ -n "$GATEWAY" ] && ip route add default via $GATEWAY
else
    udhcpc -i eth0 -q -n 2>/dev/null || echo "(dhcp failed, continuing)"
fi

echo "--- interfaces ---"
ip addr show eth0
MYIP=\$(ip -4 addr show eth0 | sed -n 's/.*inet \\([0-9.]*\\).*/\\1/p' | head -n1)
echo "Adapter IP: \$MYIP"

echo ">> starting eip_adapter"
exec /bin/eip_adapter $CONFIG_ARG --ip "\$MYIP" $ADAPTER_ARGS
INIT
chmod +x "$ROOTFS/init"

echo ">> Packing initramfs"
mkdir -p "$BUILD"
( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$BUILD/initramfs.cpio.gz"

echo ">> Done: $BUILD/initramfs.cpio.gz"
ls -l "$BUILD/initramfs.cpio.gz"
