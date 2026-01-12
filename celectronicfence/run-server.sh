
cd /data/lintech
sudo ./mediamtx &
# sudo ./mediamtx > /lintech/mediamtx.log 2>&1 &      

cd /data/lintech/celectronicfence
sudo ./server &
#调试打印log
# sudo ./server > /lintech/celectronicfence/server.log 2>&1 &

sudo -i
cvi_pinmux -w PWM2/GPIO77
echo 429 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio429/direction

cvi_pinmux -w PWM3/GPIO78
echo 430 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio430/direction

cd /data/sophon-stream/samples/yolov8/scripts/
# ./run_hdmi_show.sh > /data/sophon-stream/samples/yolov8/scripts/hdmi_show.log 2>&1 &
./run_hdmi_show.sh &
exit