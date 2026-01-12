# 告警管理 API 文档

## 概述

告警管理系统提供完整的告警记录、查询和管理功能。支持从算法程序自动发送告警，并在Web界面中查看和管理。

## API 端点

### 1. 获取所有告警
**GET** `/api/alarms`

**响应示例:**
```json
{
  "status": "success",
  "alarms": [
    {
      "id": 1,
      "taskId": 1,
      "videoSourceId": 1,
      "videoSourceName": "ipc206",
      "alarmType": "区域入侵",
      "imageUrl": "/upload/alarm/alarm_20251113_140518_1234.jpg",
      "reportUrl": "http://192.168.1.230:8000/upload/alarm",
      "status": "reported",
      "description": "检测到有人员进入禁止区域",
      "timestamp": "2025-11-13 14:05:18"
    }
  ]
}
```

### 2. 添加新告警
**POST** `/api/alarms`

**请求体:**
```json
{
  "taskId": 1,
  "videoSourceId": 1,
  "videoSourceName": "ipc206",
  "alarmType": "区域入侵",
  "imageUrl": "/upload/alarm/alarm_20251113_140518_1234.jpg",
  "reportUrl": "http://192.168.1.230:8000/upload/alarm",
  "status": "reported",
  "description": "检测到有人员进入禁止区域"
}
```

**响应:**
```json
{
  "status": "success",
  "message": "Alarm added successfully",
  "alarm": { ... }
}
```

### 3. 上传告警图片
**POST** `/api/alarms/upload`

**请求:** multipart/form-data
- `image`: 图片文件 (JPEG格式)

**响应:**
```json
{
  "status": "success",
  "imageUrl": "/upload/alarm/alarm_20251113_140518_1234.jpg",
  "filename": "alarm_20251113_140518_1234.jpg"
}
```

### 4. 删除告警
**DELETE** `/api/alarms/:id`

**响应:**
```json
{
  "status": "success",
  "message": "Alarm deleted successfully"
}
```

### 5. 批量删除告警
**POST** `/api/alarms/batch-delete`

**请求体:**
```json
{
  "ids": [1, 2, 3]
}
```

## 告警状态说明

- `pending`: 未上报 - 告警已记录但未上报到外部系统
- `reported`: 已上报 - 告警已成功上报到外部系统
- `failed`: 上报失败 - 告警上报到外部系统时失败

## 从算法程序发送告警

### Python 示例

```python
import requests
import json

def send_alarm(image_path, task_id, video_source_id, alarm_type):
    # 1. 上传图片
    with open(image_path, 'rb') as f:
        files = {'image': f}
        upload_resp = requests.post(
            'http://localhost:8088/api/alarms/upload',
            files=files
        )
        image_url = upload_resp.json()['imageUrl']
    
    # 2. 创建告警
    alarm_data = {
        "taskId": task_id,
        "videoSourceId": video_source_id,
        "videoSourceName": "摄像头1",
        "alarmType": alarm_type,
        "imageUrl": image_url,
        "status": "pending"
    }
    
    requests.post(
        'http://localhost:8088/api/alarms',
        json=alarm_data
    )

# 使用
send_alarm('/path/to/image.jpg', 1, 1, '区域入侵')
```

### C++ 示例

```cpp
#include "httplib.hpp"
#include "json.hpp"
#include <fstream>

void send_alarm(const std::string& image_path, int task_id, 
                int video_source_id, const std::string& alarm_type) {
    httplib::Client cli("http://localhost:8088");
    
    // 1. 上传图片
    std::ifstream file(image_path, std::ios::binary);
    std::string image_data((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    
    httplib::MultipartFormDataItems items = {
        {"image", image_data, "image.jpg", "image/jpeg"}
    };
    
    auto upload_res = cli.Post("/api/alarms/upload", items);
    auto upload_json = nlohmann::json::parse(upload_res->body);
    std::string image_url = upload_json["imageUrl"];
    
    // 2. 创建告警
    nlohmann::json alarm_data;
    alarm_data["taskId"] = task_id;
    alarm_data["videoSourceId"] = video_source_id;
    alarm_data["videoSourceName"] = "摄像头1";
    alarm_data["alarmType"] = alarm_type;
    alarm_data["imageUrl"] = image_url;
    alarm_data["status"] = "pending";
    
    cli.Post("/api/alarms", alarm_data.dump(), "application/json");
}
```

## Web界面功能

### 查看告警
1. 进入"告警管理"页面
2. 查看告警卡片网格，每个卡片显示：
   - 告警图片
   - 告警时间
   - 告警类型
   - 视频源
   - 上报状态
   - 告警内容

### 筛选告警
- 按任务筛选
- 按视频源筛选
- 按日期筛选
- 按告警内容搜索
- 按状态筛选

### 查看详情
点击告警卡片查看详细信息

## 文件存储

- 告警记录: `/data/lintech/celectronicfence/alarms.json`
- 告警图片: `/data/lintech/celectronicfence/static/upload/alarm/`

## 集成到算法程序

在你的区域入侵算法中，当检测到入侵时：

```python
# 在检测到入侵时
if intrusion_detected:
    # 保存当前帧
    cv2.imwrite('/tmp/alarm_frame.jpg', frame)
    
    # 发送告警
    send_alarm(
        image_path='/tmp/alarm_frame.jpg',
        task_id=current_task_id,
        video_source_id=current_video_source_id,
        video_source_name=current_video_source_name,
        alarm_type='区域入侵',
        description='检测到有人员进入禁止区域'
    )
```

## 注意事项

1. 图片上传大小限制：建议不超过 5MB
2. 图片格式：支持 JPEG、PNG
3. 告警记录会持久化保存
4. 建议定期清理旧的告警图片以节省存储空间

