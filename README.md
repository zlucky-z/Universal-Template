# 边缘AI设备管理系统 - 通用前端模板 V2

## 项目简介

这是一个通用的边缘AI设备管理系统前端模板，是此前 `font` 仓库的 V2 升级版本。该项目提供了完整的Web管理界面和后端API服务，可作为基础模板进行定制化开发，适用于各类边缘AI设备的管理和监控场景。

### 支持平台

本项目已在以下嵌入式平台上完美运行：

- **Sophon SOC平台**
  - BM1688 芯片
  - CV186X 系列芯片

- **Sigmastar平台**
  - SSC377DE 芯片

## 核心特性

### 1. 用户认证系统
- 简洁的登录界面
- 基于 localStorage 的会话管理
- 默认账号：`admin` / `admin`

### 2. 视频流管理
- 支持 RTSP 视频流接入
- 多路视频源管理
- 实时视频预览
- 视频录制与分段存储

### 3. 告警管理系统
- 实时告警记录与展示
- 告警图片上传与存储
- 多维度告警筛选（任务、视频源、日期、状态）
- 告警状态管理（未上报/已上报/上报失败）
- 批量删除功能
- 完整的 RESTful API 接口

### 4. 智能区域配置
- 基于 Fabric.js 的可视化区域绘制
- 多边形区域定义
- 区域入侵检测配置
- 区域配置持久化存储

### 5. 系统管理
- 网络配置管理
- 参数设置
- TF卡存储管理
- 系统更新
- 用户管理

### 6. 任务管理
- 视频分析任务配置
- 任务状态监控
- 多任务并发支持

## 技术栈

### 前端技术
- **HTML5 + CSS3** - 现代化响应式界面
- **JavaScript (ES6+)** - 原生JavaScript开发
- **Bootstrap 5** - UI框架
- **Axios** - HTTP请求库
- **Fabric.js** - Canvas图形绘制
- **Chart.js** - 数据可视化
- **Font Awesome** - 图标库
- **Lodash** - 工具函数库

### 后端技术
- **C++17** - 高性能后端服务
- **cpp-httplib** - HTTP服务器库
- **nlohmann/json** - JSON解析库
- **FFmpeg** - 视频处理

## 项目结构

```
celectronicfence/
├── static/                      # 前端静态资源目录
│   ├── index.html              # 登录页面
│   ├── index1.html             # 主界面框架
│   ├── home.html               # 首页
│   ├── alarm.html              # 告警管理页面
│   ├── stream_new.html         # 视频流管理
│   ├── smart.html              # 智能区域配置
│   ├── task.html               # 任务管理
│   ├── network.html            # 网络配置
│   ├── params.html             # 参数设置
│   ├── TF.html                 # TF卡管理
│   ├── updata.html             # 系统更新
│   ├── user.html               # 用户管理
│   ├── CmeraIp.html            # 摄像头IP配置
│   ├── setting.html            # 系统设置
│   ├── bootstrap.min.css       # Bootstrap样式
│   ├── axios.min.js            # Axios库
│   ├── fabric.min.js           # Fabric.js库
│   ├── chart.min.js            # Chart.js库
│   ├── lodash.min.js           # Lodash库
│   ├── fontawesome-all.min.css # Font Awesome图标
│   ├── background.png          # 背景图片
│   ├── upload/                 # 上传文件目录
│   │   └── alarm/              # 告警图片存储
│   └── webfonts/               # 字体文件
├── main.cc                      # 主程序源码
├── server                       # 编译后的可执行文件
├── conf.json                    # 区域配置文件
├── default.json                 # 默认配置文件
├── alarms.json                  # 告警记录文件
├── channels.json                # 视频通道配置
├── params.json                  # 系统参数配置
├── tasks.json                   # 任务配置文件
├── ALARM_API.md                 # 告警API文档
├── build.sh                     # 编译脚本
├── run.sh                       # 运行脚本
└── README.md                    # 项目说明文档
```

## 快速开始

### 环境要求

- Linux 操作系统（推荐 Ubuntu 18.04+）
- GCC 7.0+ 或 Clang 5.0+（支持 C++17）
- FFmpeg 开发库
- 网络连接（用于访问RTSP视频流）

### 编译项目

```bash
# 进入项目目录
cd celectronicfence

# 编译项目
chmod +x build.sh
./build.sh
```

### 运行服务

```bash
# 运行HTTP服务器（默认端口8088）
chmod +x run.sh
./run.sh
```

### 访问系统

在浏览器中访问：
```
http://<设备IP地址>:8088
```

默认登录账号：
- 用户名：`admin`
- 密码：`admin`

## API 接口文档

### 告警管理 API

详细的告警管理API文档请参考 [ALARM_API.md](ALARM_API.md)

#### 主要接口

- `GET /api/alarms` - 获取所有告警
- `POST /api/alarms` - 添加新告警
- `POST /api/alarms/upload` - 上传告警图片
- `DELETE /api/alarms/:id` - 删除指定告警
- `POST /api/alarms/batch-delete` - 批量删除告警

### 视频流管理 API

- `POST /api/start-recording` - 开始录制
- `POST /api/stop-recording` - 停止录制
- `GET /api/recording-status` - 获取录制状态

### 系统配置 API

- `GET /api/config` - 获取系统配置
- `POST /api/config` - 更新系统配置
- `GET /api/tf-card-info` - 获取TF卡信息

## 定制化开发指南

### 1. 基于本模板创建新项目

```bash
# 复制项目模板
cp -r celectronicfence my-custom-project
cd my-custom-project

# 修改项目配置
vim conf.json
vim default.json
```

### 2. 自定义界面

修改 `static/` 目录下的HTML文件：

- **修改登录页面**：编辑 `static/index.html`
- **修改主界面**：编辑 `static/index1.html`
- **添加新功能页面**：在 `static/` 目录下创建新的HTML文件

### 3. 扩展后端功能

在 `main.cc` 中添加新的API端点：

```cpp
// 添加新的API路由
svr.Get("/api/your-endpoint", [](const Request& req, Response& res) {
    json response;
    response["status"] = "success";
    response["data"] = "your data";
    res.set_content(response.dump(), "application/json");
});
```

### 4. 集成算法程序

参考 [ALARM_API.md](ALARM_API.md) 中的示例代码，将您的AI算法与告警系统集成：

**Python示例：**
```python
import requests

def send_alarm(image_path, task_id, video_source_id, alarm_type):
    # 上传图片
    with open(image_path, 'rb') as f:
        files = {'image': f}
        upload_resp = requests.post(
            'http://localhost:8088/api/alarms/upload',
            files=files
        )
        image_url = upload_resp.json()['imageUrl']

    # 创建告警
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
```

## 配置说明

### conf.json - 区域配置

定义智能分析的区域坐标：

```json
{
    "polygon": [
        {
            "points": [
                {"x": 401, "y": 183},
                {"x": 1516, "y": 173},
                {"x": 1784, "y": 928},
                {"x": 195, "y": 927}
            ]
        }
    ]
}
```

### default.json - 默认配置

配置默认的视频流地址和存储位置：

```json
{
    "current": {
        "defaultRtspStreamUrl": "rtsp://admin:password@192.168.1.64:554/media/video1",
        "defaultRtspStreamUrl2": "rtsp://admin:password@192.168.1.64:554/media/video2",
        "defaultSaveLocation": "/mnt/tfcard/videos1",
        "defaultSaveLocation2": "/mnt/tfcard/videos2"
    }
}
```

### params.json - 系统参数

配置系统运行参数，如录制分段时间、存储策略等。

## 部署建议

### 1. 开发环境部署

```bash
# 直接运行
./run.sh
```

### 2. 生产环境部署

建议使用 systemd 服务管理：

```bash
# 创建服务文件
sudo vim /etc/systemd/system/edge-ai-manager.service
```

服务文件内容：
```ini
[Unit]
Description=Edge AI Device Manager
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/path/to/celectronicfence
ExecStart=/path/to/celectronicfence/server
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

启动服务：
```bash
sudo systemctl daemon-reload
sudo systemctl enable edge-ai-manager
sudo systemctl start edge-ai-manager
```

### 3. 跨平台部署注意事项

#### Sophon BM1688/CV186X 平台
- 确保已安装 Sophon SDK
- 配置正确的交叉编译工具链
- 注意内存和存储限制

#### Sigmastar SSC377DE 平台
- 使用对应的交叉编译工具链
- 确保 FFmpeg 库已正确编译
- 注意文件系统权限配置

## 常见问题

### Q1: 编译失败怎么办？

确保已安装必要的开发库：
```bash
sudo apt-get install build-essential libavformat-dev libavcodec-dev libavutil-dev
```

### Q2: 无法访问Web界面？

检查防火墙设置：
```bash
sudo ufw allow 8088/tcp
```

### Q3: 视频录制失败？

- 检查RTSP地址是否正确
- 确认存储路径有写入权限
- 检查磁盘空间是否充足

### Q4: 告警图片无法上传？

确保 `static/upload/alarm/` 目录存在且有写入权限：
```bash
mkdir -p static/upload/alarm
chmod 755 static/upload/alarm
```

## 版本历史

### V2.0 (当前版本)
- 重构前端界面，采用现代化设计
- 完善告警管理系统
- 优化视频流处理性能
- 增强跨平台兼容性
- 添加完整的API文档

### V1.0 (font仓库)
- 基础功能实现
- 简单的视频流管理
- 基础告警功能

## 贡献指南

欢迎提交 Issue 和 Pull Request 来改进这个项目。

## 许可证

本项目采用 MIT 许可证。详见 [LICENSE](LICENSE) 文件。

## 技术支持

如有问题或建议，请通过以下方式联系：

- 提交 GitHub Issue
- 发送邮件至项目维护者

## 致谢

感谢所有为本项目做出贡献的开发者和使用者。

---

**注意**：本项目为通用模板，实际部署时请根据具体需求进行定制化开发和安全加固。
