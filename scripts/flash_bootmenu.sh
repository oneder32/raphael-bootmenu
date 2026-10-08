#!/sbin/sh
# BootMenu V0.6 安装脚本
# 在 Recovery (TWRP/OrangeFox) 中运行
# 用法: 在 Recovery 中通过 ADB 或终端执行: sh flash_bootmenu.sh

echo "========================================"
echo "  BootMenu V0.6 Multi-Boot Installer"
echo "  for Redmi K20 Pro (raphael)"
echo "========================================"
echo ""

# 检查是否在 recovery 环境
if [ ! -d /dev/block/bootdevice ]; then
    echo "[!] 未找到 bootdevice, 请确保在 Recovery 中运行"
    exit 1
fi

BOOT_PARTITION="/dev/block/bootdevice/by-name/boot"
BOOT_IMG="/sdcard/bootmenu/boot.img"

# 查找 boot.img 位置
if [ -f "/sdcard/bootmenu/boot.img" ]; then
    BOOT_IMG="/sdcard/bootmenu/boot.img"
elif [ -f "/external_sd/bootmenu/boot.img" ]; then
    BOOT_IMG="/external_sd/bootmenu/boot.img"
elif [ -f "/usb_otg/bootmenu/boot.img" ]; then
    BOOT_IMG="/usb_otg/bootmenu/boot.img"
elif [ -f "./boot.img" ]; then
    BOOT_IMG="./boot.img"
else
    echo "[!] 未找到 boot.img"
    echo "    请将 boot.img 放到 /sdcard/bootmenu/ 目录"
    exit 1
fi

echo "[*] 找到 boot.img: $BOOT_IMG"
echo "[*] 目标分区: $BOOT_PARTITION"
echo ""
echo "[!] 即将刷入 BootMenu 到 boot 分区"
echo "[!] 这将覆盖当前 boot 分区!"
echo ""
read -p "确认刷入? (y/N): " confirm
if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
    echo "[*] 已取消"
    exit 0
fi

echo ""
echo "[*] 正在备份当前 boot 分区..."
dd if="$BOOT_PARTITION" of="/sdcard/boot_backup_$(date +%Y%m%d_%H%M%S).img" 2>/dev/null
echo "[*] 备份完成 (保存到 /sdcard/)"

echo "[*] 正在刷入 BootMenu..."
dd if="$BOOT_IMG" of="$BOOT_PARTITION" 2>/dev/null
sync

echo "[*] 刷入完成!"
echo ""
echo "[*] 创建配置目录..."
mkdir -p /data/setboot

# 如果配置文件不存在, 创建示例配置
if [ ! -f /data/setboot/file.txt ]; then
    cat > /data/setboot/file.txt << 'EOF'
# BootMenu 配置文件
# 格式: 启动名称|boot.img路径|dtbo.img路径
# 路径相对于 /data, 最多10个启动项
#
# 示例:
# MIUI 14|/setboot/miui14/boot.img|/setboot/miui14/dtbo.img
# LineageOS 21|/setboot/los21/boot.img|/setboot/los21/dtbo.img
EOF
    echo "[*] 已创建示例配置: /data/setboot/file.txt"
fi

echo ""
echo "========================================"
echo "  安装完成!"
echo "========================================"
echo ""
echo "下一步:"
echo "  1. 编辑 /data/setboot/file.txt 添加启动项"
echo "  2. 将各系统的 boot.img 和 dtbo.img 放到"
echo "     /data/setboot/ 对应目录"
echo "  3. 重启设备, 即可看到启动菜单"
echo ""
echo "操作说明:"
echo "  音量+ / 音量- : 上下选择"
echo "  电源键 / 触控点击 : 确认启动"
echo "  返回键 : 跳过菜单进入 Recovery"
echo ""
