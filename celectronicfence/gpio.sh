#!/bin/bash

# GPIO引脚编号
GPIO_PIN=429

# 导出GPIO引脚
echo $GPIO_PIN > /sys/class/gpio/export

# 设置GPIO引脚为输出模式
echo "out" > /sys/class/gpio/gpio$GPIO_PIN/direction

while true; do
    # 拉高GPIO引脚
    echo 1 > /sys/class/gpio/gpio$GPIO_PIN/value
    sleep 1

    # 拉低GPIO引脚
    echo 0 > /sys/class/gpio/gpio$GPIO_PIN/value
    sleep 1
done

# 取消导出GPIO引脚（如果脚本结束需要执行这一步，这里示例中脚本不会主动结束）
# echo $GPIO_PIN > /sys/class/gpio/unexport