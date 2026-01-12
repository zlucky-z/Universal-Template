#!/bin/bash

program_name="main"

# 函数：检查程序是否运行并终止
terminate_program() {
    local pid=$(pgrep -f "$program_name")
    if [ -n "$pid" ]; then
        echo "程序正在运行，进程 ID: $pid。正在尝试终止程序..."
        sudo killall -15 "$program_name"
        echo "发送 SIGTERM 信号后等待程序停止..."
        local wait_counter=0
        while ps -p "$pid" > /dev/null; do
            ((wait_counter++))
            if [ "$wait_counter" -eq 10 ]; then
                echo "程序未能及时停止，尝试强制终止..."
                sudo killall -9 "$program_name"
                break
            fi
            sleep 0.5
        done
    else
        echo "程序未在运行。"
    fi
}

# 主流程
terminate_program

# 启动另一个脚本，使用 sudo 权限
echo "启动新脚本..."
cd /data/sophon-stream/samples/yolov8/scripts/
sudo ./run_hdmi_show.sh