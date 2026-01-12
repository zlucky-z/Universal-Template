#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
告警发送示例脚本
演示如何从算法程序发送告警到告警管理系统
"""

import requests
import json
import cv2
import base64
from datetime import datetime
import numpy as np

# 服务器地址
SERVER_URL = "http://localhost:8088"

def send_alarm(image_path, task_id, video_source_id, video_source_name, alarm_type, 
               report_url="", description=""):
    """
    发送告警到服务器
    
    参数:
        image_path: 告警图片路径（或numpy数组）
        task_id: 任务ID
        video_source_id: 视频源ID
        video_source_name: 视频源名称
        alarm_type: 告警类型（如"区域入侵"）
        report_url: 上报地址（可选）
        description: 告警描述（可选）
    
    返回:
        bool: 是否发送成功
    """
    try:
        # 1. 先上传图片
        if isinstance(image_path, str):
            # 从文件读取
            with open(image_path, 'rb') as f:
                image_data = f.read()
        elif isinstance(image_path, np.ndarray):
            # 从numpy数组编码
            _, buffer = cv2.imencode('.jpg', image_path)
            image_data = buffer.tobytes()
        else:
            print("错误: 不支持的图片格式")
            return False
        
        # 上传图片
        files = {'image': ('alarm.jpg', image_data, 'image/jpeg')}
        upload_response = requests.post(f"{SERVER_URL}/api/alarms/upload", files=files)
        
        if upload_response.status_code != 200:
            print(f"图片上传失败: {upload_response.text}")
            return False
        
        upload_result = upload_response.json()
        if upload_result['status'] != 'success':
            print(f"图片上传失败: {upload_result.get('message', '未知错误')}")
            return False
        
        image_url = upload_result['imageUrl']
        print(f"图片上传成功: {image_url}")
        
        # 2. 创建告警记录
        alarm_data = {
            "taskId": task_id,
            "videoSourceId": video_source_id,
            "videoSourceName": video_source_name,
            "alarmType": alarm_type,
            "imageUrl": image_url,
            "reportUrl": report_url,
            "status": "reported" if report_url else "pending",
            "description": description
        }
        
        alarm_response = requests.post(
            f"{SERVER_URL}/api/alarms",
            headers={'Content-Type': 'application/json'},
            data=json.dumps(alarm_data)
        )
        
        if alarm_response.status_code != 200:
            print(f"告警创建失败: {alarm_response.text}")
            return False
        
        alarm_result = alarm_response.json()
        if alarm_result['status'] != 'success':
            print(f"告警创建失败: {alarm_result.get('message', '未知错误')}")
            return False
        
        print(f"告警发送成功! ID: {alarm_result['alarm']['id']}")
        return True
        
    except Exception as e:
        print(f"发送告警时发生错误: {str(e)}")
        return False


def send_alarm_from_opencv(frame, task_id, video_source_id, video_source_name, alarm_type):
    """
    从OpenCV帧直接发送告警
    
    参数:
        frame: OpenCV读取的图像帧（numpy数组）
        task_id: 任务ID
        video_source_id: 视频源ID
        video_source_name: 视频源名称
        alarm_type: 告警类型
    
    返回:
        bool: 是否发送成功
    """
    return send_alarm(
        image_path=frame,
        task_id=task_id,
        video_source_id=video_source_id,
        video_source_name=video_source_name,
        alarm_type=alarm_type,
        description="检测到异常情况"
    )


# 使用示例
if __name__ == "__main__":
    # 示例1: 从文件发送告警
    # send_alarm(
    #     image_path="/path/to/alarm_image.jpg",
    #     task_id=1,
    #     video_source_id=1,
    #     video_source_name="ipc206",
    #     alarm_type="区域入侵",
    #     report_url="http://192.168.1.230:8000/upload/alarm",
    #     description="检测到有人员进入禁止区域"
    # )
    
    # 示例2: 从摄像头捕获并发送告警
    # cap = cv2.VideoCapture(0)
    # ret, frame = cap.read()
    # if ret:
    #     send_alarm_from_opencv(
    #         frame=frame,
    #         task_id=1,
    #         video_source_id=1,
    #         video_source_name="摄像头1",
    #         alarm_type="区域入侵"
    #     )
    # cap.release()
    
    print("告警发送示例脚本")
    print("请在代码中取消注释相应的示例来测试")

