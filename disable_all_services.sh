#!/bin/bash

# 检查是否以root权限运行
if [ "$(id -u)" -ne 0 ]; then
    echo "错误：请用root权限运行脚本（sudo ./disable_all_services.sh）"
    exit 1
fi

# 定义需要禁用的服务列表
SERVICES=(
    "gpio-config.service"
    "autostart.service"
    "ffmpeg-start.service"
)

# ==============================================
# 禁用所有服务的开机自启
# ==============================================
echo "===== 开始禁用所有服务的开机自启 ====="
for service in "${SERVICES[@]}"; do
    # 检查服务是否存在
    if systemctl is-enabled "$service" &> /dev/null; then
        # 禁用服务自启
        systemctl disable "$service"
        # 显示禁用后的状态
        echo "$service 自启状态：$(systemctl is-enabled "$service")"
    else
        echo "$service 不存在或未配置，跳过"
    fi
done


# ==============================================
# （可选）停止当前运行的服务
# ==============================================
read -p $'\n是否需要立即停止当前运行的这些服务？(y/n) ' stop_choice
if [ "$stop_choice" = "y" ] || [ "$stop_choice" = "Y" ]; then
    echo -e "\n===== 开始停止服务 ====="
    for service in "${SERVICES[@]}"; do
        if systemctl is-active "$service" &> /dev/null; then
            systemctl stop "$service"
            echo "$service 已停止（当前状态：$(systemctl is-active "$service")）"
        else
            echo "$service 未在运行，跳过"
        fi
    done
else
    echo -e "\n已跳过停止服务操作，当前运行的服务会继续运行至下次重启"
fi


# ==============================================
# （可选）移除TF卡自动挂载配置
# ==============================================
read -p $'\n是否需要移除TF卡自动挂载配置？(y/n) ' umount_choice
if [ "$umount_choice" = "y" ] || [ "$umount_choice" = "Y" ]; then
    echo -e "\n===== 开始移除TF卡自动挂载配置 ====="
    
    # 删除udev规则文件
    UDEV_RULE="/etc/udev/rules.d/99-auto-mount-tfcard.rules"
    if [ -f "$UDEV_RULE" ]; then
        rm -f "$UDEV_RULE"
        echo "已删除udev规则文件：$UDEV_RULE"
    else
        echo "udev规则文件 $UDEV_RULE 不存在，跳过"
    fi
    
    # 删除挂载/卸载脚本
    MOUNT_SCRIPT="/sbin/mount-tfcard.sh"
    if [ -f "$MOUNT_SCRIPT" ]; then
        rm -f "$MOUNT_SCRIPT"
        echo "已删除挂载脚本：$MOUNT_SCRIPT"
    else
        echo "挂载脚本 $MOUNT_SCRIPT 不存在，跳过"
    fi
    
    UNMOUNT_SCRIPT="/sbin/unmount-tfcard.sh"
    if [ -f "$UNMOUNT_SCRIPT" ]; then
        rm -f "$UNMOUNT_SCRIPT"
        echo "已删除卸载脚本：$UNMOUNT_SCRIPT"
    else
        echo "卸载脚本 $UNMOUNT_SCRIPT 不存在，跳过"
    fi
    
    # 重载udev规则使删除生效
    udevadm control --reload-rules
    udevadm trigger
    echo "udev规则已重载，TF卡自动挂载配置已移除"
else
    echo -e "\n已保留TF卡自动挂载配置"
fi


# ==============================================
# 完成提示
# ==============================================
echo -e "\n===== 操作完成！====="
echo "1. 服务自启状态已更新，下次重启后生效"
echo "2. 如需重新启用自启，可重新运行 setup_all_services.sh 脚本"
