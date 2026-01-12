#!/bin/bash

# 检查是否以root权限运行
if [ "$(id -u)" -ne 0 ]; then
    echo "错误：请用root权限运行脚本（sudo ./setup_all_services.sh）"
    exit 1
fi

# ==============================================
# 1. GPIO引脚配置服务（gpio-config.service）
#    配置一次后，开机自启，每次重启自动执行
# ==============================================
echo "===== 开始配置 GPIO 服务 ====="
GPIO_SERVICE="/etc/systemd/system/gpio-config.service"

if [ ! -f "$GPIO_SERVICE" ]; then
    cat > "$GPIO_SERVICE" << 'EOF'
[Unit]
Description=GPIO Pins Configuration Service
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/bash -c ' \
    echo 429 > /sys/class/gpio/export; \
    echo 430 > /sys/class/gpio/export; \
    echo out > /sys/class/gpio/gpio429/direction; \
    echo out > /sys/class/gpio/gpio430/direction; \
    chmod 777 /sys/class/gpio/gpio429/value; \
    chmod 777 /sys/class/gpio/gpio430/value \
'
User=root
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
    echo "gpio-config.service 创建成功"
else
    echo "gpio-config.service 已存在，跳过创建"
fi


# ==============================================
# 2. 自启页面和算法服务（autostart.service）
# ==============================================
echo -e "\n===== 开始配置 autostart.service ====="
AUTOSTART_SERVICE="/etc/systemd/system/autostart.service"

if [ ! -f "$AUTOSTART_SERVICE" ]; then
    cat > "$AUTOSTART_SERVICE" << 'EOF'
[Unit]
Description=Auto-Start Service for Scripts
After=network.target

[Service]
Type=oneshot
ExecStart=/bin/bash /data/lintech/celectronicfence/run-server.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
    echo "autostart.service 创建成功"
else
    echo "autostart.service 已存在，跳过创建"
fi


# ==============================================
# 3. ffmpeg进程服务（ffmpeg-start.service）
# ==============================================
echo -e "\n===== 开始配置 ffmpeg-start.service ====="
FFMPEG_SERVICE="/etc/systemd/system/ffmpeg-start.service"

if [ ! -f "$FFMPEG_SERVICE" ]; then
    cat > "$FFMPEG_SERVICE" << 'EOF'
[Unit]
Description=FFmpeg Startup Service
After=network.target

[Service]
Type=forking
ExecStart=/bin/bash /data/lintech/celectronicfence/ffmpeg.sh
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF
    echo "ffmpeg-start.service 创建成功"
else
    echo "ffmpeg-start.service 已存在，跳过创建"
fi


# ==============================================
# 4. 配置所有systemd服务开机自启
#    （无论是否新建，都重载配置并确保自启）
# ==============================================
echo -e "\n===== 配置所有服务开机自启 ====="
systemctl daemon-reload  # 重载配置，确保新服务生效

# 启用所有服务（已启用则自动忽略）
systemctl enable gpio-config.service
systemctl enable autostart.service
systemctl enable ffmpeg-start.service

# 检查自启状态
echo "gpio-config.service 自启状态：$(systemctl is-enabled gpio-config.service)"
echo "autostart.service 自启状态：$(systemctl is-enabled autostart.service)"
echo "ffmpeg-start.service 自启状态：$(systemctl is-enabled ffmpeg-start.service)"


# ==============================================
# 5. TF卡自动挂载配置（一次性配置，永久生效）
# ==============================================
echo -e "\n===== 开始配置TF卡自动挂载 ====="

# 新增：创建/mnt/tfcard目录并设置777权限
echo "===== 配置/mnt/tfcard目录权限 ====="
TF_MOUNT_POINT="/mnt/tfcard"
# 确保目录存在（-p表示父目录不存在也一起创建）
mkdir -p "$TF_MOUNT_POINT"
# 设置权限为777（所有用户可读可写可执行）
chmod 777 "$TF_MOUNT_POINT"
echo "$TF_MOUNT_POINT 目录已创建，权限已设置为777"


# 5.1 创建udev规则文件
UDEV_RULE="/etc/udev/rules.d/99-auto-mount-tfcard.rules"
if [ ! -f "$UDEV_RULE" ]; then
    cat > "$UDEV_RULE" << 'EOF'
# Auto mount TF card when inserted
KERNEL=="mmcblk1p1", ACTION=="add", SUBSYSTEM=="block", RUN+="/sbin/mount-tfcard.sh"

# Unmount TF card when removed
KERNEL=="mmcblk1p1", ACTION=="remove", SUBSYSTEM=="block", RUN+="/sbin/unmount-tfcard.sh"

# Create symlink for TF card device file
KERNEL=="mmcblk1p1", SUBSYSTEM=="block", ATTRS{removable}=="1", SYMLINK+="tfcard-%k"
EOF
    echo "udev规则文件 $UDEV_RULE 创建成功"
else
    echo "udev规则文件 $UDEV_RULE 已存在，跳过创建"
fi


# 5.2 创建挂载脚本（mount-tfcard.sh）
MOUNT_SCRIPT="/sbin/mount-tfcard.sh"
if [ ! -f "$MOUNT_SCRIPT" ]; then
    cat > "$MOUNT_SCRIPT" << 'EOF'
#!/bin/bash

# 指定TF卡的挂载点目录
MOUNT_POINT="/mnt/tfcard"

# TF卡设备节点
DEVICE="/dev/mmcblk1p1"

# 检查并创建挂载点
if [ ! -d "$MOUNT_POINT" ]; then
    mkdir -p "$MOUNT_POINT"
fi

# 挂载TF卡（vfat格式）
mount -t vfat "$DEVICE" "$MOUNT_POINT"
EOF
    chmod +x "$MOUNT_SCRIPT"
    echo "挂载脚本 $MOUNT_SCRIPT 创建并授权成功"
else
    chmod +x "$MOUNT_SCRIPT"  # 确保权限正确
    echo "挂载脚本 $MOUNT_SCRIPT 已存在，确保权限正确"
fi


# 5.3 创建卸载脚本（unmount-tfcard.sh）
UNMOUNT_SCRIPT="/sbin/unmount-tfcard.sh"
if [ ! -f "$UNMOUNT_SCRIPT" ]; then
    cat > "$UNMOUNT_SCRIPT" << 'EOF'
#!/bin/bash

# TF卡设备节点
DEVICE="/dev/mmcblk1p1"

# 卸载TF卡
umount "$DEVICE"
EOF
    chmod +x "$UNMOUNT_SCRIPT"
    echo "卸载脚本 $UNMOUNT_SCRIPT 创建并授权成功"
else
    chmod +x "$UNMOUNT_SCRIPT"  # 确保权限正确
    echo "卸载脚本 $UNMOUNT_SCRIPT 已存在，确保权限正确"
fi


# 5.4 重新加载udev规则
echo -e "\n===== 重新加载udev规则 ====="
udevadm control --reload-rules
udevadm trigger
echo "udev规则已重载"


# ==============================================
# 完成提示
# ==============================================
echo -e "\n===== 所有配置已完成！====="
echo "1. GPIO配置：已设置开机自启，每次重启会自动执行引脚配置"
echo "2. 其他服务：autostart、ffmpeg服务已设置开机自启，重启后自动运行"
echo "3. TF卡挂载：已创建/mnt/tfcard目录（权限777），配置自动挂载"
echo "4. 后续操作："
echo "   - 手动启动服务：systemctl start gpio-config.service autostart.service ffmpeg-start.service"
echo "   - 查看服务状态：systemctl status [服务名]"