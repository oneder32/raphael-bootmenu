#!/system/bin/sh
# BootMenu V0.4 wrapper - runs as recovery service
# Mounts /data, runs bootmenu, then execs real recovery

LOG=/tmp/bootmenu_wrapper.log
echo "=== BootMenu wrapper starting ===" > $LOG

# SELinux permissive
echo 0 > /sys/fs/selinux/enforce 2>/dev/null

# Get bootdevice
BOOTDEV=$(getprop ro.boot.bootdevice 2>/dev/null)
[ -z "$BOOTDEV" ] && BOOTDEV="1d84000.ufshc"
echo "bootdevice=$BOOTDEV" >> $LOG

# Wait for bootdevice
i=0
while [ $i -lt 30 ]; do
    [ -e "/dev/block/platform/soc/$BOOTDEV" ] && break
    sleep 1
    i=$((i+1))
done

# Create bootdevice symlink
[ ! -e /dev/block/bootdevice ] && ln -s "/dev/block/platform/soc/$BOOTDEV" /dev/block/bootdevice 2>/dev/null

# Wait for userdata by-name
i=0
while [ $i -lt 20 ]; do
    [ -e /dev/block/bootdevice/by-name/userdata ] && break
    sleep 1
    i=$((i+1))
done
echo "userdata exists: $([ -e /dev/block/bootdevice/by-name/userdata ] && echo yes || echo no)" >> $LOG

# Mount /data
mkdir -p /data 2>/dev/null
mount -t f2fs /dev/block/bootdevice/by-name/userdata /data >> $LOG 2>&1
if [ $? -ne 0 ]; then
    echo "f2fs failed, trying ext4" >> $LOG
    mount -t ext4 /dev/block/bootdevice/by-name/userdata /data >> $LOG 2>&1
fi
echo "data mounted: $?" >> $LOG

mkdir -p /data/setboot 2>/dev/null

# Run bootmenu
echo "Starting bootmenu..." >> $LOG
/sbin/bootmenu >> $LOG 2>&1
echo "bootmenu exit: $?" >> $LOG

# Exec real recovery
echo "Execing recovery" >> $LOG
exec /system/bin/recovery
