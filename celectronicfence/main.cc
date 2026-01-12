#include "httplib.hpp"
#include "json.hpp"
#include <iostream>
#include <fstream>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <string>
#include <cstdio>
#include <getopt.h>

#include <iomanip>
#include <condition_variable>

#include <cstdio>
#include <vector>
#include <algorithm>
#include <dirent.h> // 添加的这个头文件，用于目录操作相关函数及结构体的声明
#include <cerrno>
#include <sys/stat.h> // 添加用于读取文件状态的头文件
using json = nlohmann::json;
using namespace httplib;

//----------------------------------------------------------------------------------------------------------------------------
std::string executeCommand(const char *command);
void deleteOldestFiles(const std::string &mountPath);
// 文件序号，用于区分不同时间段生成的文件，初始为0
int fileIndex = 0;
// 全局声明一个用于保护文件操作相关并发访问的互斥锁
std::mutex fileOperationMutex;
// 在全局作用域声明globalMutex
std::mutex globalMutex;
// 在全局作用域声明mutex和cv
std::mutex mutex;
std::condition_variable cv;
// 结构体用于存储TF卡信息
struct TFCardInfo
{
    std::string mountPath;
    std::string usedMemory;
    std::string freeMemory;
};
// 在全局作用域声明用于记录开始录制时间的变量
std::chrono::time_point<std::chrono::system_clock> startRecordingTime;
// 在全局作用域定义线程结束标志位，初始化为false
std::atomic<bool> stopTimerThread(false);
// 在全局作用域定义VIDEO_RETENTION_MINUTES变量
const int VIDEO_RETENTION_MINUTES = 10;
// 在全局作用域声明用于记录上次执行删除操作时间的变量
std::chrono::time_point<std::chrono::system_clock> lastDeleteTime;
// 在全局作用域声明互斥锁
std::mutex lastDeleteTimeMutex;
// 全局变量用于存储开机自启的默认视频流地址和保存地址（初始化为从JSON文件读取的值）
std::string defaultRtspStreamUrl;
std::string defaultSaveLocation;
std::string defaultRtspStreamUrl2;
std::string defaultSaveLocation2;
// 全局变量用于存储开机自启的默认分段时间（单位：秒）
int defaultSegmentTime = 600;
// 用于标记是否是开机自启的情况，初始化为false
bool isAutoStart = false;
//----------------------------------------------------------------------------------------------------------------------------
// 获取TF卡信息的函数（修改为了直接提取df -h命令输出中的已用和剩余内存信息）
TFCardInfo getTFCardInfo()
{
    TFCardInfo tfCardInfo;
    tfCardInfo.mountPath = "/mnt/tfcard"; // 设置TF卡挂载路径

    // 基于设置好的挂载路径构建df -h命令，去获取对应磁盘信息
    std::string dfCommand = "df -h " + tfCardInfo.mountPath;
    std::string dfOutput = executeCommand(dfCommand.c_str());

    // 解析df -h命令输出内容，获取已用和剩余内存信息
    std::istringstream iss(dfOutput);
    std::string line;
    std::getline(iss, line);
    std::getline(iss, line);
    std::istringstream lineStream(line);
    std::string filesystem, size, used, free, usePercent, mountPoint;
    lineStream >> filesystem >> size >> used >> free >> usePercent >> mountPoint;
    tfCardInfo.usedMemory = used;
    tfCardInfo.freeMemory = free;

    return tfCardInfo;
}
//----------------------------------------------------------------------------------------------------------------------------
bool waitForMountPoint(const std::string &mountPath, int maxWaitSeconds = 45)
{
    int waitSeconds = 0;
    struct stat sb;
    while (stat(mountPath.c_str(), &sb) != 0)
    {
        if (waitSeconds >= maxWaitSeconds)
        {
            std::cerr << "等待TF卡挂载超时,挂载点 " << mountPath << " 不可用。" << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        waitSeconds++;
    }
    return true;
}
//----------------------------------------------------------------------------------------------------------------------------
// 执行系统命令并获取输出结果的函数（优化缓冲区大小版本）
std::string executeCommand(const char *command)
{
    char buffer[1024]; // 增大缓冲区大小
    std::string result = "";
    FILE *pipe = popen(command, "r");
    if (!pipe)
    {
        std::cerr << "执行命令失败: " << command << ", 错误码: " << errno << std::endl;
        return result;
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        result += buffer;
    }
    pclose(pipe);
    return result;
}
//----------------------------------------------------------------------------------------------------------------------------
// 从default.json文件读取默认地址信息
void readDefaultAddressesFromJson(json &defaultAddresses)
{
    std::ifstream jsonFile("default.json");
    if (jsonFile.is_open())
    {
        jsonFile >> defaultAddresses;
        jsonFile.close();
    }
    else
    {
        std::cerr << "Error opening default.json file for reading." << std::endl;
    }
    // 初始化全局的默认地址变量（之前已经有这些全局变量声明）
    defaultRtspStreamUrl = defaultAddresses["current"].value("defaultRtspStreamUrl", "");
    defaultSaveLocation = defaultAddresses["current"].value("defaultSaveLocation", "");
    defaultRtspStreamUrl2 = defaultAddresses["current"].value("defaultRtspStreamUrl2", "");
    defaultSaveLocation2 = defaultAddresses["current"].value("defaultSaveLocation2", "");
}
//----------------------------------------------------------------------------------------------------------------------------
// 更新default.json文件中的默认地址信息
void updateDefaultAddressesInJson(const json &newAddresses)
{
    // 验证传入的JSON数据结构，只允许包含"current"和"initial"节点
    if (!newAddresses.contains("current") ||!newAddresses.contains("initial"))
    {
        std::cerr << "Invalid JSON structure passed to updateDefaultAddressesInJson. Must contain 'current' and 'initial' nodes." << std::endl;
        return;
    }

    std::ofstream jsonFile("default.json");
    if (jsonFile.is_open())
    {
        jsonFile << newAddresses.dump(4);
        jsonFile.close();
    }
    else
    {
        std::cerr << "Error opening default.json file for writing." << std::endl;
    }
}
//----------------------------------------------------------------------------------------------------------------------------
// 定时器线程函数，定期调用deleteOldestFiles函数
void timerThreadFunction()
{
    while (!stopTimerThread.load())
    {
        // 我们的TF卡挂载路径是硬编码的 "/mnt/tfcard"，可调整传入参数方式
        deleteOldestFiles("/mnt/tfcard");
        std::this_thread::sleep_for(std::chrono::seconds(180));
    }
    std::cout << "定时器线程已结束。" << std::endl;
}
//----------------------------------------------------------------------------------------------------------------------------
// 删除挂载路径下最早生成的文件（修改后的版本，基于时间判断来删除超过设定天数的文件）
void deleteOldestFiles(const std::string &mountPath)
{
    const double VIDEO_RETENTION_DAYS = 10.01; // 设定视频文件保留的天数阈值，修改为double类型
    std::vector<std::string> fileList;
    // 构建查找指定挂载路径下所有视频文件的命令
    // std::string listCommand = "sudo find " + mountPath + "/videos -type f";
    std::string listCommand = "sudo find " + mountPath + "/videos1 " + mountPath + "/videos2 -type f";
    // 执行命令获取文件列表
    std::string fileOutput = executeCommand(listCommand.c_str());
    char *token = strtok(const_cast<char *>(fileOutput.c_str()), "\n");
    while (token != nullptr)
    {
        fileList.push_back(token);
        token = strtok(nullptr, "\n");
    }

    // 比较函数，基于文件的最后修改时间判断是否超过保留天数
    auto compareFilesByTime = [](const std::string &file1, const std::string &file2)
    {
        struct stat fileStat1, fileStat2;
        if (stat(file1.c_str(), &fileStat1) < 0 || stat(file2.c_str(), &fileStat2) < 0)
        {
            std::cerr << "获取文件时间属性失败，可能影响排序" << std::endl;
            return file1 < file2;
        }
        // 将fileStat1.st_mtime和fileStat2.st_mtime转换为std::chrono::system_clock::time_point类型来计算时间差
        auto timePoint1 = std::chrono::system_clock::from_time_t(fileStat1.st_mtime);
        auto timePoint2 = std::chrono::system_clock::from_time_t(fileStat2.st_mtime);
        // 获取当前时间并转换为std::chrono::system_clock::time_point类型
        auto currentTime = std::chrono::system_clock::now();
        // 计算文件1距离当前时间的分钟数差，使用std::chrono库来更精确处理时间
        auto diff1_minutes = std::chrono::duration_cast<std::chrono::minutes>(currentTime - timePoint1).count();
        // 将分钟数差换算为天数差，并添加详细调试输出，这里改为double类型计算
        double diff1 = diff1_minutes / (24.0 * 60.0);
        std::cout << "文件1的时间戳(st_mtime)转换后的时间点（精确到纳秒）: " << std::chrono::duration_cast<std::chrono::nanoseconds>(timePoint1.time_since_epoch()).count() << " 纳秒" << std::endl;
        std::cout << "当前时间（精确到纳秒）: " << std::chrono::duration_cast<std::chrono::nanoseconds>(currentTime.time_since_epoch()).count() << std::endl;
        std::cout << "文件1距离当前时间换算后的天数差: " << diff1 << ", 分钟数差: " << diff1_minutes << std::endl;
        // 计算文件2距离当前时间的分钟数差
        auto diff2_minutes = std::chrono::duration_cast<std::chrono::minutes>(currentTime - timePoint2).count();
        double diff2 = diff2_minutes / (24.0 * 60.0);
        std::cout << "文件2的时间戳(st_mtime)转换后的时间点（精确到纳秒）: " << std::chrono::duration_cast<std::chrono::nanoseconds>(timePoint2.time_since_epoch()).count() << " 纳秒" << std::endl;
        std::cout << "当前时间（精确到纳秒）: " << std::chrono::duration_cast<std::chrono::nanoseconds>(currentTime.time_since_epoch()).count() << std::endl;
        std::cout << "文件2距离当前时间换算后的天数差: " << diff2 << ", 分钟数差: " << diff2_minutes << std::endl;
        return diff1 > diff2;
    };

    // 根据比较函数对文件列表进行排序，使得超过保留天数的文件排在前面
    std::sort(fileList.begin(), fileList.end(), compareFilesByTime);

    // 新增的逻辑：获取当前时间，用于和文件的时间差做比较
    // 获取当前时间并转换为std::chrono::system_clock::time_point类型
    auto currentTime = std::chrono::system_clock::now();
    while (!fileList.empty())
    {
        // 取出排在最前面的文件（当前认为是最旧的文件）
        std::string fileName = fileList.front();
        struct stat fileStat;
        if (stat(fileName.c_str(), &fileStat) < 0)
        {
            std::cerr << "获取文件时间属性失败，无法准确判断是否删除，文件名: " << fileName << std::endl;
            fileList.erase(fileList.begin());
            continue;
        }
        // 将文件的最后修改时间转换为可计算时间差的类型
        auto fileTimePoint = std::chrono::system_clock::from_time_t(fileStat.st_mtime);
        // 计算该文件距离当前时间的分钟数差，使用std::chrono库来更精确处理时间
        auto diff_minutes = std::chrono::duration_cast<std::chrono::minutes>(currentTime - fileTimePoint).count();
        // 将分钟数差换算为天数差，并添加详细调试输出，这里改为double类型计算
        double diff = diff_minutes / (24.0 * 60.0);
        std::cout << "正在判断文件 " << fileName << " 是否删除,文件时间戳(st_mtime)转换后的时间点（精确到纳秒）: " << std::chrono::duration_cast<std::chrono::nanoseconds>(fileTimePoint.time_since_epoch()).count() << " 纳秒" << std::endl;
        std::cout << "当前时间（精确到纳秒）: " << std::chrono::duration_cast<std::chrono::nanoseconds>(currentTime.time_since_epoch()).count() << std::endl;
        std::cout << "文件 " << fileName << " 距离当前时间换算后的天数差: " << diff << ", 分钟数差: " << diff_minutes << std::endl;
        // 新增逻辑：只有当文件距离当前时间的天数差大于等于设定的阈值时，才执行删除操作，这里将diff显式转换为double类型进行比较
        if (diff >= VIDEO_RETENTION_DAYS)
        {
            std::cout << "文件 " << fileName << " 满足删除条件，准备删除" << std::endl;
            std::stringstream fileNamesToDelete;
            const int batchSize = 1; // 每次批量删除的文件数量，可调整
            for (int i = 0; i < batchSize && !fileList.empty(); ++i)
            {
                fileNamesToDelete << fileList.front() << " ";
                fileList.erase(fileList.begin());
            }

            std::string deleteCommand = "sudo rm " + fileNamesToDelete.str();
            int deleteResult = std::system(deleteCommand.c_str());
            if (deleteResult == 0)
            {
                std::cout << "已删除文件: " << fileName << std::endl;
                // 文件删除成功后，添加适当延迟，确保文件系统完成更新
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            else
            {
                std::cerr << "删除文件 " << fileName << " 失败，错误码: " << deleteResult << ", 文件路径示例: " << fileNamesToDelete.str() << std::endl;
                // 将错误文件信息记录到日志文件
                std::ofstream errorLog("error.log", std::ios::app);
                if (errorLog.is_open())
                {
                    errorLog << "[" << std::time(nullptr) << "] 删除文件 " << fileName << " 失败: " << fileNamesToDelete.str() << ", 错误码: " << deleteResult << std::endl;
                    errorLog.close();
                }
                // 尝试最多3次重试删除文件
                int retryCount = 0;
                while (retryCount < 3)
                {
                    std::system(deleteCommand.c_str());
                    if (std::system(deleteCommand.c_str()) == 0)
                    {
                        break;
                    }
                    std::cerr << "重试删除文件 " << fileName << "，第 " << (retryCount + 1) << " 次失败" << std::endl;
                    retryCount++;
                }
                if (retryCount == 3)
                {
                    std::cerr << "多次重试后仍无法删除文件 " << fileName << "，放弃操作" << std::endl;
                }
            }
        }
        else
        {
            std::cout << "文件 " << fileName << " 未满足删除条件，跳过删除" << std::endl;
            // 如果文件未达到删除阈值，直接跳出循环，不再继续尝试删除其他文件
            break;
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------------
// 用于标记ffmpeg是否正在运行，用于主线程内简单判断录制状态
bool stopVideoStreamProcessing = false;
bool isFfmpegRunning = false;
// 全局变量用于标记第二路ffmpeg是否正在运行
bool isFfmpegRunning2 = false;
namespace JsonFile1
{
    // 定义网络配置结构体
    struct NetworkConfig
    {
        std::string ipAddress;
        std::string subnetMask;
        std::string gateway;
        std::vector<std::string> dnsServers;
    };
    //----------------------------------------------------------------------------------------------------------------------------
    // 全局变量存储网络配置
    NetworkConfig currentConfig;
    // 解析 JSON 数据并更新网络配置

    //----------------------------------------------------------------------------------------------------------------------------
    void updateNetworkConfig(const json &j)
    {
        currentConfig.ipAddress = j.value("ipAddress", "");
        currentConfig.subnetMask = j.value("subnetMask", "");
        currentConfig.gateway = j.value("gateway", "");
        currentConfig.dnsServers = j.value("dnsServers", std::vector<std::string>());
    }
    //----------------------------------------------------------------------------------------------------------------------------
    // 应用网络配置到开发板
    void applyNetworkConfigToBoard()
    {
        std::ofstream interfacesFile("/etc/network/interfaces");
        if (interfacesFile.is_open())
        {
            interfacesFile << "auto eth0\n";
            interfacesFile << "iface eth0 inet static\n";
            interfacesFile << "address " << currentConfig.ipAddress << "\n";
            interfacesFile << "netmask " << currentConfig.subnetMask << "\n";
            interfacesFile << "gateway " << currentConfig.gateway << "\n";
            interfacesFile << "dns-nameservers " << currentConfig.dnsServers[0];
            for (size_t i = 1; i < currentConfig.dnsServers.size(); ++i)
            {
                interfacesFile << " " << currentConfig.dnsServers[i];
            }
            interfacesFile.close();

            // 通过执行命令来重启网络服务使配置生效
            std::system("sudo /etc/init.d/networking restart");
            // 等待网络服务启动完成（这里可以根据实际情况调整等待时间或进行网络连接测试）
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        else
        {
            std::cerr << "Error opening /etc/network/interfaces for writing." << std::endl;
        }
    }
    //----------------------------------------------------------------------------------------------------------------------------
    // 处理网络配置保存的 POST 请求
    void handleSaveNetworkConfig(const Request &req, Response &res)
    {
        if (req.method == "POST" && req.path == "/save_network_config")
        {
            try
            {
                json reqJson = json::parse(req.body);
                updateNetworkConfig(reqJson);
                applyNetworkConfigToBoard();
                // 添加短暂延迟
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                res.status = 200;
                res.set_content("Configuration saved successfully and applied to board.", "text/plain");
            }
            catch (const std::exception &e)
            {
                res.status = 500;
                res.set_content("Error saving configuration: " + std::string(e.what()), "text/plain");
            }
        }
        else
        {
            res.status = 404;
        }
    }
}

// ----------------------------------------------------------------------------

// 定义多边形的点结构体
struct point
{
    int x, y;
};
//----------------------------------------------------------------------------------------------------------------------------
// 定义多边形结构体
struct polygon
{
    std::vector<struct point> points;
};
//----------------------------------------------------------------------------------------------------------------------------
// 存储多边形数据的全局向量
std::vector<struct polygon> polygon;

// 用于保护对配置文件操作的互斥锁
std::mutex confFileMutex;
//----------------------------------------------------------------------------------------------------------------------------
// 从配置文件读取多边形数据
void conf_polygon()
{
    std::ifstream f("conf.json");
    if (f.good())
    {
        try
        {
            json conf;
            f >> conf;
            for (const auto &polyObj : conf["polygon"])
            {
                std::vector<struct point> tmp_points;
                for (const auto &pointObj : polyObj["points"])
                {
                    tmp_points.push_back({pointObj["x"], pointObj["y"]});
                }
                polygon.push_back({tmp_points});
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error reading conf.json: " << e.what() << std::endl;
            polygon.clear();
        }
    }
    else
    {
        polygon.clear();
    }
}
//----------------------------------------------------------------------------------------------------------------------------
// 将多边形数据保存到配置文件
void savePolygonToFile()
{
    json polygonsJson;
    for (const auto &poly : polygon)
    {
        json polyJson;
        polyJson["points"] = json::array();
        for (const auto &point : poly.points)
        {
            polyJson["points"].push_back({{"x", point.x},
                                          {"y", point.y}});
        }
        polygonsJson["polygon"].push_back(polyJson);
    }
    std::ofstream outFile("conf.json");
    if (outFile.is_open())
    {
        try
        {
            outFile << polygonsJson.dump(4);
            outFile.close();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error saving polygon configuration to file: " << e.what() << ", file: conf.json" << std::endl;
        }
    }
    else
    {
        try
        {
            throw std::runtime_error("Error opening file conf.json for saving");
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error opening file conf.json for saving: " << e.what() << ", file: conf.json" << std::endl;
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------------

void route_polygon(Server &svr)
{
    conf_polygon();
    //----------------------------------------------------------------------------------------------------------------------------
    svr.Get("/stream/polygon/:id", [&](const Request &req, Response &res)
            {
        auto id = atoi(req.path_params.at("id").c_str());
        if (id < 0 || id >= polygon.size()) {
            res.status = 400;
            return;
        }

        json resj;
        for (auto it : polygon[id].points) {
            resj["points"].push_back({{"x", it.x}, {"y", it.y}});
        }
        res.set_content(resj.dump(), "application/json"); });
    //----------------------------------------------------------------------------------------------------------------------------
    svr.Post("/stream/polygon/:id", [&](const Request &req, Response &res)
             {
        auto id = atoi(req.path_params.at("id").c_str());
        if (id < 0 || id >= polygon.size()) {
            res.status = 400;
            return;
        }

        json reqj = json::parse(req.body);
        polygon[id].points.clear();
        for (auto it : reqj["points"]) {
            polygon[id].points.push_back({it.at("x"), it.at("y")});
        }
         // 保存多边形数据到文件
        savePolygonToFile();
        
        // 刷新缓存，重新读取更新后的配置文件
        polygon.clear();
        conf_polygon(); });
}

// ----------------------------------------------------------------------------

// 人员检测模块
namespace HumanDetection {
    // 人员检测状态结构体
    struct HumanDetectionStatus {
        bool channel20_detected = false;
        bool channel10_detected = false;
        std::string timestamp;
    };

    // 读取GPIO文件状态
    bool readGPIOStatus(const std::string& gpioPath) {
        std::ifstream gpioFile(gpioPath);
        if (gpioFile.is_open()) {
            std::string value;
            std::getline(gpioFile, value);
            gpioFile.close();
            return (value == "1");
        }
        return false;
    }

    // 获取当前时间戳
    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    // 处理human_detected API请求
    void handleHumanDetectedAPI(const Request& req, Response& res) {
        try {
            // 读取GPIO状态
            bool channel20_detected = readGPIOStatus("/sys/class/gpio/gpio429/value");
            bool channel10_detected = readGPIOStatus("/sys/class/gpio/gpio430/value");
            
            // 构建响应JSON
            json responseJson;
            responseJson["status"] = "success";
            responseJson["timestamp"] = getCurrentTimestamp();
            responseJson["channels"] = json::array();
            
            // 添加通道20的状态
            json channel20;
            channel20["channel_id"] = 20;
            channel20["human_detected"] = !channel20_detected;  // 反转逻辑：GPIO为0表示检测到人
            channel20["gpio_pin"] = 429;
            responseJson["channels"].push_back(channel20);
            
            // 添加通道10的状态
            json channel10;
            channel10["channel_id"] = 10;
            channel10["human_detected"] = !channel10_detected;  // 反转逻辑：GPIO为0表示检测到人
            channel10["gpio_pin"] = 430;
            responseJson["channels"].push_back(channel10);

            // 设置CORS头
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            
            // 返回JSON响应
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            errorJson["timestamp"] = getCurrentTimestamp();
            
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    }
}

void route(Server &svr) {}

// ----------------------------------------------------------------------------
std::string time_local()
{
    auto p = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(p);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%d/%b/%Y:%H:%M:%S %z");
    return ss.str();
}
//----------------------------------------------------------------------------------------------------------------------------
std::string log(const Request &req, const Response &res)
{
    auto remote_user = "-";
    auto request = req.method + " " + req.path + " " + req.version;
    auto body_bytes_sent = res.get_header_value("Content-Length");
    auto http_referer = "-";
    auto http_user_agent = req.get_header_value("User-Agent", "-");

    return "[" + time_local() + "] " + req.remote_addr + " " +
           std::to_string(res.status) + " " + req.method + " " + req.path;
}
//----------------------------------------------------------------------------------------------------------------------------
namespace JsonFile2
{
    // 加载 JSON 文件并更新全局变量
    void loadJsonData(json &data)
    {
        std::ifstream jsonFile("/data/sophon-stream/samples/yolov8/config/yolov8_classthresh_roi_example.json");
        if (jsonFile.is_open())
        {
            jsonFile >> data;
            jsonFile.close();
        }
        else
        {
            std::cerr << "Error opening JSON file for reading." << std::endl;
        }
    }
    //----------------------------------------------------------------------------------------------------------------------------
    // 更新 JSON 文件
    void updateJsonFile(const json &data)
    {
        std::ofstream jsonFile("/data/sophon-stream/samples/yolov8/config/yolov8_classthresh_roi_example.json");
        if (jsonFile.is_open())
        {
            jsonFile << data.dump(4);
            jsonFile.close();
            // 防止重复重启的静态标志
            static std::atomic<bool> isRestarting(false);
            
            // 启动一个新线程来延迟启动脚本
            if (!isRestarting.exchange(true)) {
                std::thread runScriptThread([]()
                                            {
                    // 等待一段时间，让前端有时间处理弹窗
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    std::cout << "自动重启开始..." << std::endl;
                    int result = std::system("/data/lintech/celectronicfence/run.sh");
                    if (result == 0) {
                        std::cout << "自动重启成功" << std::endl;
                    } else {
                        std::cout << "自动重启失败，返回码: " << result << std::endl;
                    }
                    // 重启完成后重置标志
                    isRestarting.store(false);
                });

                // 分离线程，使其在后台运行
                runScriptThread.detach();
            } else {
                std::cout << "重启已在进行中，跳过本次重启请求" << std::endl;
            }
        }
        else
        {
            std::cerr << "Error opening JSON file for writing." << std::endl;
        }
    }
    //----------------------------------------------------------------------------------------------------------------------------
    // 加载 JSON 文件并更新全局变量
    void loadJsonDataForDemo(json &data)
    {
        std::ifstream jsonFile("/data/sophon-stream/samples/yolov8/config/yolov8_demo.json");
        if (jsonFile.is_open())
        {
            jsonFile >> data;
            jsonFile.close();
        }
        else
        {
            std::cerr << "Error opening JSON file for reading." << std::endl;
        }
    }
    //----------------------------------------------------------------------------------------------------------------------------
    // 更新 JSON 文件
    void updateJsonFileForDemo(const json &data)
    {
        std::ofstream jsonFile("/data/sophon-stream/samples/yolov8/config/yolov8_demo.json");
        if (jsonFile.is_open())
        {
            jsonFile << data.dump(4);
            jsonFile.close();
        }
        else
        {
            std::cerr << "Error opening JSON file for writing." << std::endl;
        }
    }
}
//----------------------------------------------------------------------------------------------------------------------------
void videoStreamFetchThreadFunction()
{
    while (!stopVideoStreamProcessing)
    {
        // 发送GET请求获取视频流数据
        //...
        std::cout << "视频流获取线程正在运行，已获取一帧数据。" << std::endl;
    }
    std::cout << "收到停止信号，开始清理视频流获取线程资源。" << std::endl;
    // 清理资源代码
    std::cout << "视频流获取线程已停止。" << std::endl;
}
//----------------------------------------------------------------------------------------------------------------------------
// 启动ffmpeg命令进行录制，接收视频流地址、保存位置以及分段时间参数（以秒为单位）
void startRecording(const std::string &rtspStreamUrl = "", const std::string &saveLocation = "",
                    const std::string &rtspStreamUrl2 = "", const std::string &saveLocation2 = "")
{
        json defaultAddresses;
    readDefaultAddressesFromJson(defaultAddresses);
// 明确指定命名空间来访问defaultAddresses变量的成员
    std::string actualRtspStreamUrl = (rtspStreamUrl.empty())? nlohmann::json(defaultAddresses)["current"].value("defaultRtspStreamUrl", "") : rtspStreamUrl;
    std::string actualSaveLocation = (saveLocation.empty())? nlohmann::json(defaultAddresses)["current"].value("defaultSaveLocation", "") : saveLocation;
    std::string actualRtspStreamUrl2 = (rtspStreamUrl2.empty())? nlohmann::json(defaultAddresses)["current"].value("defaultRtspStreamUrl2", "") : rtspStreamUrl2;
    std::string actualSaveLocation2 = (saveLocation2.empty())? nlohmann::json(defaultAddresses)["current"].value("defaultSaveLocation2", "") : saveLocation2;
    // 使用默认的分段时间（因为前端不传分段时间参数，这里就直接用预设的默认值）
    int segmentTime = defaultSegmentTime;

    // 生成文件名相关逻辑
    std::string fileNameFormat = "%Y-%m-%d_%H-%M-%S.mp4";
    std::string fileNameFormat2 = "%Y-%m-%d_%H-%M-%S.mp4";

    // 构建ffmpeg命令字符串
    std::string ffmpegCommand = "sudo ffmpeg -rtsp_transport tcp -i " + actualRtspStreamUrl + " -c:v copy -c:a aac -strict experimental -f segment -segment_time " + std::to_string(segmentTime) + " -reset_timestamps 1 -strftime 1 -segment_format mp4 " + actualSaveLocation + "/" + fileNameFormat + "&";
    std::cout << "ffmpeg command: " << ffmpegCommand << std::endl;
    // 构建第二路视频流的ffmpeg命令字符串
    std::string ffmpegCommand2 = "sudo ffmpeg -rtsp_transport tcp -i " + actualRtspStreamUrl2 + " -c:v copy -c:a aac -strict experimental -f segment -segment_time " + std::to_string(segmentTime) + " -reset_timestamps 1 -strftime 1 -segment_format mp4 " + actualSaveLocation2 + "/" + fileNameFormat2 + "&";
    std::cout << "ffmpeg command for stream 2: " << ffmpegCommand2 << std::endl;

    // 创建一个新线程来执行ffmpeg命令
    std::thread ffmpegThread([ffmpegCommand]()
                             {
    // 标记ffmpeg正在运行
        isFfmpegRunning = true;
        // 更新开始录制时间为当前时间
        startRecordingTime = std::chrono::system_clock::now();
        // 使用system函数执行ffmpeg命令启动录制
        int result = system(ffmpegCommand.c_str());
        if (result == -1) {
            std::cerr << "Error starting ffmpeg process." << std::endl;
        // 设置全局错误标志位，表示ffmpeg启动失败
        // ffmpegStartError = true;
        } });
    std::thread ffmpegThread2([ffmpegCommand2]()
                              {
        // 标记第二路ffmpeg正在运行
        isFfmpegRunning2 = true;
        // 更新第二路开始录制时间为当前时间
        startRecordingTime = std::chrono::system_clock::now();
        // 使用system函数执行第二路ffmpeg命令启动录制
        int result2 = system(ffmpegCommand2.c_str());
        if (result2 == -1) {
            std::cerr << "Error starting ffmpeg process for stream 2." << std::endl;
            // 设置第二路全局错误标志位，表示ffmpeg启动失败
            // ffmpegStartError2 = true;
        } });

    std::vector<std::thread> threads;
    threads.push_back(std::move(ffmpegThread));
    threads.push_back(std::move(ffmpegThread2));

    for (auto &t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // 主线程等待ffmpeg线程执行完毕
    // 构建json对象并更新录制状态为true
    json statusJson;
    statusJson["recording_status"] = true;
    std::ofstream statusFile("recording_status.json");
    if (statusFile.is_open())
    {
        statusFile << statusJson.dump(4);
        statusFile.close();
    }
    else
    {
        std::cerr << "无法打开recording_status.json文件来更新录制状态。" << std::endl;
    }
}
//----------------------------------------------------------------------------------------------------------------------------
// 停止ffmpeg命令的录制
void stopRecording()
{
    // 使用pgrep查找ffmpeg进程ID
    std::string pgrepCommand = "pgrep ffmpeg";
    FILE *pipe = popen(pgrepCommand.c_str(), "r");
    if (!pipe)
    {
        std::cerr << "无法执行pgrep命令来查找ffmpeg进程。" << std::endl;
        return;
    }
    char buffer[128];
    std::string result = "";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL)
    {
        result += buffer;
    }
    pclose(pipe);

    // 处理获取到的进程ID字符串，可能包含多个进程ID，以换行符分隔
    size_t pos = 0;
    std::string delimiter = "\n";
    std::string token;
    while ((pos = result.find(delimiter)) != std::string::npos)
    {
        token = result.substr(0, pos);
        // 转换为整数类型的进程ID
        int pid = std::stoi(token);
        std::string killCommand = "sudo kill -2 " + std::to_string(pid);
        int killResult = system(killCommand.c_str());
        if (killResult == 0)
        {
            std::cout << "成功发送信号停止ffmpeg进程,进程ID: " << pid << std::endl;
        }
        else
        {
            std::cerr << "无法发送信号停止ffmpeg进程,进程ID: " << pid << std::endl;
        }
        result.erase(0, pos + delimiter.length());
    }

    // 标记ffmpeg已停止运行
    isFfmpegRunning = false;
    // 添加设置定时器线程结束标志位为true，通知定时器线程停止运行
    stopTimerThread.store(true);
    // 更新JSON文件中的录制状态为false
    json statusJson;
    statusJson["recording_status"] = false;
    std::ofstream statusFile("recording_status.json");
    if (statusFile.is_open())
    {
        statusFile << statusJson.dump(4);
        statusFile.close();
    }
    else
    {
        std::cerr << "无法打开recording_status.json文件来更新录制状态。" << std::endl;
    }
}
//----------------------------------------------------------------------------------------------------------------------------
// 告警远程上报功能

// 解析URL，提取host、port和path
struct ParsedUrl {
    std::string protocol; // http 或 https
    std::string host;
    int port;
    std::string path;
    bool valid;
};

// 读取远程上报地址配置
std::string getRemoteAlarmUrl()
{
    try {
        std::ifstream file("/data/lintech/celectronicfence/params.json");
        if (file.is_open()) {
            json paramsJson;
            file >> paramsJson;
            file.close();
            
            // 读取远程上报地址参数（支持多种可能的key名称）
            std::string url = "";
            
            // 优先读取 RemoteInfo（与前端参数标识一致）
            if (paramsJson.contains("RemoteInfo") && paramsJson["RemoteInfo"].is_string()) {
                url = paramsJson["RemoteInfo"].get<std::string>();
            }
            // 兼容旧版本的 remoteAlarmUrl
            else if (paramsJson.contains("remoteAlarmUrl") && paramsJson["remoteAlarmUrl"].is_string()) {
                url = paramsJson["remoteAlarmUrl"].get<std::string>();
            }
            
            // 如果URL不为空，返回它
            if (!url.empty()) {
                std::cout << "[远程上报] 读取到配置地址: " << url << std::endl;
                return url;
            } else {
                std::cout << "[远程上报] 配置地址为空" << std::endl;
            }
        } else {
            std::cerr << "[远程上报] 无法打开params.json文件" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[远程上报] 读取配置失败: " << e.what() << std::endl;
    }
    return ""; // 返回空字符串表示未配置
}

ParsedUrl parseUrl(const std::string& url)
{
    ParsedUrl result;
    result.valid = false;
    result.port = 80; // 默认端口
    
    if (url.empty()) {
        return result;
    }
    
    try {
        // 查找协议
        size_t protocolEnd = url.find("://");
        if (protocolEnd == std::string::npos) {
            // 如果没有协议，默认使用http
            result.protocol = "http";
            protocolEnd = 0;
        } else {
            result.protocol = url.substr(0, protocolEnd);
            protocolEnd += 3; // 跳过 "://"
        }
        
        // 设置默认端口
        if (result.protocol == "https") {
            result.port = 443;
        }
        
        // 查找路径开始位置
        size_t pathStart = url.find("/", protocolEnd);
        size_t hostEnd = (pathStart == std::string::npos) ? url.length() : pathStart;
        
        // 提取host和port
        std::string hostPort = url.substr(protocolEnd, hostEnd - protocolEnd);
        size_t colonPos = hostPort.find(":");
        if (colonPos != std::string::npos) {
            result.host = hostPort.substr(0, colonPos);
            result.port = std::stoi(hostPort.substr(colonPos + 1));
        } else {
            result.host = hostPort;
        }
        
        // 提取path
        if (pathStart != std::string::npos) {
            result.path = url.substr(pathStart);
        } else {
            result.path = "/";
        }
        
        result.valid = !result.host.empty();
    } catch (const std::exception& e) {
        std::cerr << "[远程上报] URL解析失败: " << e.what() << std::endl;
        result.valid = false;
    }
    
    return result;
}

// 将文件内容转换为base64编码
std::string fileToBase64(const std::string& filePath)
{
    try {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[远程上报] 无法打开图片文件: " << filePath << std::endl;
            return "";
        }
        
        // 读取文件内容
        std::string fileContent((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        file.close();
        
        if (fileContent.empty()) {
            std::cerr << "[远程上报] 图片文件为空: " << filePath << std::endl;
            return "";
        }
        
        // Base64编码表
        const std::string base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        std::string base64;
        int val = 0, valb = -6;
        for (unsigned char c : fileContent) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                base64.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) {
            base64.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        }
        while (base64.size() % 4) {
            base64.push_back('=');
        }
        
        std::cout << "[远程上报] 图片转换为base64成功，大小: " << base64.size() << " 字符" << std::endl;
        return base64;
    } catch (const std::exception& e) {
        std::cerr << "[远程上报] 图片转base64失败: " << e.what() << std::endl;
        return "";
    }
}

// 异步上报告警到远程地址
void reportAlarmToRemote(const json& alarmData)
{
    // 在独立线程中执行上报，避免阻塞主线程
    std::thread reportThread([alarmData]() {
        try {
            // 读取远程上报地址
            std::string remoteUrl = getRemoteAlarmUrl();
            if (remoteUrl.empty()) {
                std::cout << "[远程上报] 未配置远程上报地址，跳过上报" << std::endl;
                return;
            }
            
            std::cout << "[远程上报] 开始上报告警到: " << remoteUrl << std::endl;
            
            // 解析URL
            ParsedUrl parsed = parseUrl(remoteUrl);
            if (!parsed.valid) {
                std::cerr << "[远程上报] URL格式无效: " << remoteUrl << std::endl;
                return;
            }
            
            std::cout << "[远程上报] 解析结果 - 协议: " << parsed.protocol 
                      << ", 主机: " << parsed.host 
                      << ", 端口: " << parsed.port 
                      << ", 路径: " << parsed.path << std::endl;
            
            // 准备要发送的数据（使用最新的告警数据）
            json dataToSend = alarmData;
            
            // 1. 读取最新的告警状态（包括更新后的reportStatus）
            int alarmId = alarmData.value("id", 0);
            bool foundLatestStatus = false;
            if (alarmId > 0) {
                try {
                    std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
                    if (fileIn.is_open()) {
                        json alarmsJson;
                        fileIn >> alarmsJson;
                        fileIn.close();
                        
                        // 查找对应的告警记录，获取最新状态
                        for (const auto& alarm : alarmsJson) {
                            if (alarm.contains("id") && alarm["id"] == alarmId) {
                                // 更新为最新的状态
                                if (alarm.contains("reportStatus")) {
                                    dataToSend["reportStatus"] = alarm["reportStatus"];
                                }
                                foundLatestStatus = true;
                                break;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[远程上报] 读取最新状态失败: " << e.what() << std::endl;
                }
            }
            
            // 2. 将reportStatus映射为status值（无论是否读取到最新状态，都要映射）
            std::string reportStatus = dataToSend.value("reportStatus", "");
            std::string statusForRemote = "pending"; // 默认值
            
            if (reportStatus == "上报成功") {
                statusForRemote = "reported";
            } else if (reportStatus == "上报失败") {
                statusForRemote = "failed";
            } else if (reportStatus == "上报中" || reportStatus == "未上报" || reportStatus.empty()) {
                statusForRemote = "pending";
            }
            
            // 保存原始status（告警处理状态）到processStatus字段（如果存在）
            std::string originalProcessStatus = alarmData.value("status", "");
            if (originalProcessStatus == "未处理" || originalProcessStatus == "已处理") {
                dataToSend["processStatus"] = originalProcessStatus;
            }
            
            // 设置映射后的status（用于远程服务器，表示上报状态）
            dataToSend["status"] = statusForRemote;
            
            std::cout << "[远程上报] 准备发送数据: reportStatus=" 
                      << reportStatus
                      << ", status=" << statusForRemote << std::endl;
            
            // 2. 读取图片文件并转换为base64
            std::string imageUrl = dataToSend.value("imageUrl", "");
            if (!imageUrl.empty()) {
                // 将相对路径转换为绝对路径
                std::string imagePath;
                if (imageUrl[0] == '/') {
                    // 已经是绝对路径
                    imagePath = "/data/lintech/celectronicfence/static" + imageUrl;
                } else {
                    imagePath = "/data/lintech/celectronicfence/static/" + imageUrl;
                }
                
                std::cout << "[远程上报] 读取图片文件: " << imagePath << std::endl;
                std::string base64Image = fileToBase64(imagePath);
                if (!base64Image.empty()) {
                    // 添加base64图片数据到JSON
                    dataToSend["imageBase64"] = base64Image;
                    // 添加图片MIME类型（根据文件扩展名判断）
                    if (imagePath.find(".jpg") != std::string::npos || 
                        imagePath.find(".jpeg") != std::string::npos) {
                        dataToSend["imageMimeType"] = "image/jpeg";
                    } else if (imagePath.find(".png") != std::string::npos) {
                        dataToSend["imageMimeType"] = "image/png";
                    } else {
                        dataToSend["imageMimeType"] = "image/jpeg"; // 默认
                    }
                    std::cout << "[远程上报] 图片已转换为base64并添加到数据中" << std::endl;
                } else {
                    std::cerr << "[远程上报] 图片转base64失败，将只发送URL" << std::endl;
                }
            }
            
            // 创建HTTP/HTTPS客户端并发送POST请求
            std::string contentType = "application/json";
            bool requestSuccess = false;
            int statusCode = 0;
            std::string responseBody;
            
            if (parsed.protocol == "https") {
                // 使用HTTPS客户端
                #ifdef CPPHTTPLIB_OPENSSL_SUPPORT
                httplib::SSLClient cli(parsed.host.c_str(), parsed.port);
                cli.set_connection_timeout(10, 0);
                cli.set_read_timeout(10, 0);
                cli.enable_server_certificate_verification(false); // 禁用证书验证（可根据需要启用）
                auto res = cli.Post(parsed.path.c_str(), dataToSend.dump(), contentType.c_str());
                if (res) {
                    requestSuccess = true;
                    statusCode = res->status;
                    responseBody = res->body;
                }
                #else
                std::cerr << "[远程上报] 错误：当前编译版本不支持HTTPS，请使用HTTP或重新编译httplib with OpenSSL支持" << std::endl;
                return;
                #endif
            } else {
                // 使用HTTP客户端
                httplib::Client cli(parsed.host.c_str(), parsed.port);
                cli.set_connection_timeout(10, 0); // 10秒连接超时
                cli.set_read_timeout(10, 0);       // 10秒读取超时
                auto res = cli.Post(parsed.path.c_str(), dataToSend.dump(), contentType.c_str());
                if (res) {
                    requestSuccess = true;
                    statusCode = res->status;
                    responseBody = res->body;
                }
            }
            
            // 更新告警记录中的上报状态
            reportStatus = "上报失败";
            if (requestSuccess) {
                std::cout << "[远程上报] 上报成功，状态码: " << statusCode << std::endl;
                if (statusCode >= 200 && statusCode < 300) {
                    std::cout << "[远程上报] 告警已成功上报到远程服务器" << std::endl;
                    reportStatus = "上报成功";
                } else {
                    std::cerr << "[远程上报] 上报失败，状态码: " << statusCode 
                              << ", 响应: " << responseBody.substr(0, 200) << std::endl;
                    reportStatus = "上报失败";
                }
            } else {
                std::cerr << "[远程上报] HTTP请求失败：无响应或连接失败" << std::endl;
                reportStatus = "上报失败";
            }
            
            // 更新告警文件中的上报状态
            bool statusUpdated = false;
            try {
                std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
                json alarmsJson;
                if (fileIn.is_open()) {
                    fileIn >> alarmsJson;
                    fileIn.close();
                    
                    // 查找对应的告警记录并更新状态
                    int alarmId = alarmData.value("id", 0);
                    for (auto& alarm : alarmsJson) {
                        if (alarm.contains("id") && alarm["id"] == alarmId) {
                            alarm["reportStatus"] = reportStatus;
                            std::cout << "[远程上报] 更新告警ID " << alarmId << " 的上报状态为: " << reportStatus << std::endl;
                            statusUpdated = true;
                            break;
                        }
                    }
                    
                    // 保存更新后的告警列表
                    std::ofstream fileOut("/data/lintech/celectronicfence/alarms.json");
                    if (fileOut.is_open()) {
                        fileOut << alarmsJson.dump(4);
                        fileOut.close();
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[远程上报] 更新告警状态失败: " << e.what() << std::endl;
            }
            
            // 如果上报成功且状态更新成功，再次发送一次更新后的状态（只发送状态更新，不包含图片）
            if (requestSuccess && statusCode >= 200 && statusCode < 300 && statusUpdated) {
                try {
                    // 等待一小段时间，确保文件写入完成
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    
                    // 读取更新后的告警数据
                    std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
                    if (fileIn.is_open()) {
                        json alarmsJson;
                        fileIn >> alarmsJson;
                        fileIn.close();
                        
                        int alarmId = alarmData.value("id", 0);
                        json updatedAlarm;
                        bool found = false;
                        
                        // 查找更新后的告警记录
                        for (const auto& alarm : alarmsJson) {
                            if (alarm.contains("id") && alarm["id"] == alarmId) {
                                updatedAlarm = alarm;
                                found = true;
                                break;
                            }
                        }
                        
                        if (found) {
                            // 准备状态更新数据（只包含关键字段，不包含图片base64）
                            json statusUpdate;
                            statusUpdate["id"] = updatedAlarm.value("id", 0);
                            statusUpdate["reportStatus"] = updatedAlarm.value("reportStatus", "");
                            
                            // 将reportStatus映射为status值
                            std::string reportStatus = updatedAlarm.value("reportStatus", "");
                            std::string statusForRemote = "pending";
                            if (reportStatus == "上报成功") {
                                statusForRemote = "reported";
                            } else if (reportStatus == "上报失败") {
                                statusForRemote = "failed";
                            } else if (reportStatus == "上报中" || reportStatus == "未上报") {
                                statusForRemote = "pending";
                            }
                            statusUpdate["status"] = statusForRemote;
                            
                            statusUpdate["timestamp"] = updatedAlarm.value("timestamp", "");
                            statusUpdate["alarmType"] = updatedAlarm.value("alarmType", "");
                            statusUpdate["videoSourceId"] = updatedAlarm.value("videoSourceId", 0);
                            statusUpdate["videoSourceName"] = updatedAlarm.value("videoSourceName", "");
                            statusUpdate["reportUrl"] = updatedAlarm.value("reportUrl", "");
                            statusUpdate["imageUrl"] = updatedAlarm.value("imageUrl", "");
                            // 不包含imageBase64，只发送状态更新
                            
                            std::cout << "[远程上报] 发送状态更新: reportStatus=" 
                                      << statusUpdate.value("reportStatus", "")
                                      << ", status=" << statusForRemote << std::endl;
                            
                            // 发送状态更新
                            if (parsed.protocol == "https") {
                                #ifdef CPPHTTPLIB_OPENSSL_SUPPORT
                                httplib::SSLClient cli(parsed.host.c_str(), parsed.port);
                                cli.set_connection_timeout(10, 0);
                                cli.set_read_timeout(10, 0);
                                cli.enable_server_certificate_verification(false);
                                auto res = cli.Post(parsed.path.c_str(), statusUpdate.dump(), contentType.c_str());
                                if (res && res->status >= 200 && res->status < 300) {
                                    std::cout << "[远程上报] 状态更新发送成功" << std::endl;
                                }
                                #endif
                            } else {
                                httplib::Client cli(parsed.host.c_str(), parsed.port);
                                cli.set_connection_timeout(10, 0);
                                cli.set_read_timeout(10, 0);
                                auto res = cli.Post(parsed.path.c_str(), statusUpdate.dump(), contentType.c_str());
                                if (res && res->status >= 200 && res->status < 300) {
                                    std::cout << "[远程上报] 状态更新发送成功" << std::endl;
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[远程上报] 发送状态更新失败: " << e.what() << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[远程上报] 上报异常: " << e.what() << std::endl;
        }
    });
    
    // 分离线程，让它在后台运行
    reportThread.detach();
}

//----------------------------------------------------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    const std::string tfMountPath = "/mnt/tfcard";
 if (!waitForMountPoint(tfMountPath))
{
    return 1; // 根据实际情况返回错误码，这里简单地返回1表示出错了
}

// 解析命令行参数，判断是否有 -a 参数（标识开机自启）
int opt;
while ((opt = getopt(argc, argv, "a"))!= -1)
{
    switch (opt)
    {
    case 'a':
        isAutoStart = true;
        break;
    default:
        // 在这里可以添加错误处理的逻辑，比如打印帮助信息等，当前先省略不写
        break;
    }
}

json defaultAddresses;
readDefaultAddressesFromJson(defaultAddresses);

// 确保defaultAddresses中存在"current"节点，如果不存在则创建一个空对象作为"current"节点
if (!defaultAddresses.contains("current"))
{
    defaultAddresses["current"] = json::object();
}

// 获取当前默认的视频流地址和保存地址，用于开机自启录制（从"current"节点下获取）
std::string defaultRtspStreamUrl = defaultAddresses["current"].value("defaultRtspStreamUrl", "");
std::string defaultSaveLocation = defaultAddresses["current"].value("defaultSaveLocation", "");
std::string defaultRtspStreamUrl2 = defaultAddresses["current"].value("defaultRtspStreamUrl2", "");
std::string defaultSaveLocation2 = defaultAddresses["current"].value("defaultSaveLocation2", "");

if (isAutoStart)
{
    // 如果是开机自启情况，尝试启动录制，传入从配置中获取的当前默认地址
    try
    {
        startRecording(defaultRtspStreamUrl, defaultSaveLocation, defaultRtspStreamUrl2, defaultSaveLocation2);
    }
    catch (const std::exception &e)
    {
        std::cerr << "开机自动启动录制出错: " << e.what() << std::endl;
    }
}

    Server svr;

    auto ret = svr.set_mount_point("/", "./static");
    if (!ret)
    {
        printf("The./static directory doesn't exist...\n");
    }
    //----------------------------------------------------------------------------------------------------------------------------
    svr.set_logger([](const Request &req, const Response &res)
                   { std::cout << log(req, res) << std::endl; });

    //----------------------------------------------------------------------------------------------------------------------------
    //     //获取 TF 卡信息并显示在页面上
    svr.Get("/get_tf_info", [](const Request &req, Response &res)
            {
    std::cout << "接受到tf请求"<< std::endl;
    TFCardInfo tfCardInfo = getTFCardInfo();
    json responseJson;
    responseJson["mountPath"] = tfCardInfo.mountPath;
    responseJson["usedMemory"] = tfCardInfo.usedMemory;
    responseJson["freeMemory"] = tfCardInfo.freeMemory;
    res.set_content(responseJson.dump(), "application/json"); });
    //----------------------------------------------------------------------------------------------------------------------------
    // 添加网络配置保存的路由
    svr.Post("/save_network_config", JsonFile1::handleSaveNetworkConfig);
    //----------------------------------------------------------------------------------------------------------------------------
    // 添加对 JSON 文件的路由
    svr.Get("/get_json_file", [](const Request &req, Response &res)
            {
        json data;
        JsonFile2::loadJsonData(data);
        res.set_content(data.dump(), "application/json"); });
    svr.Post("/get_json_file", [](const Request &req, Response &res)
             {
        try {
            json reqJson = json::parse(req.body);
            std::cout << reqJson.dump(4) << std::endl;
            json data;
            JsonFile2::loadJsonData(data);
            std::cout << data.dump(4) << std::endl;
            
            // 原有的person功能保持不变
            data["configure"]["threshold_conf"]["person"] = reqJson.value("person", 0.3);
            
            // 新增：处理其他类别的批量更新
            for (auto it = reqJson.begin(); it != reqJson.end(); ++it) {
                const std::string& key = it.key();
                const json& value = it.value();
                if (value.is_number() && key != "person") {
                    data["configure"]["threshold_conf"][key] = value;
                }
            }
            
            JsonFile2::updateJsonFile(data);
            std::cout << data.dump(4) << std::endl;
            res.status = 200;
            res.set_content("Configuration updated successfully.", "text/plain");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error updating configuration: " + std::string(e.what()), "text/plain");
        } });
    //----------------------------------------------------------------------------------------------------------------------------
    // 处理视频流地址更新的路由
    svr.Post("/update_video_stream", [](const Request &req, Response &res)
             {
        try {
            json reqJson = json::parse(req.body);
            std::string videoStreamUrl = reqJson.value("videoStreamUrl", "");
            std::string videoStreamUrl2 = reqJson.value("videoStreamUrl2", "");
            json data;
            JsonFile2::loadJsonDataForDemo(data);
        // 在channels数组中查找对应channel_id的通道并更新其url字段
        for (auto& channel : data["channels"]) {
            if (channel["channel_id"] == 20 &&!videoStreamUrl.empty()) {
                channel["url"] = videoStreamUrl;
            } else if (channel["channel_id"] == 10 &&!videoStreamUrl2.empty()) {
                channel["url"] = videoStreamUrl2;
            }
        }
            // data["channels"][0]["url"] = videoStreamUrl;
            JsonFile2::updateJsonFileForDemo(data);

           // 直接启动重启线程，无需判断标志位
        std::thread scriptThread([]() {
            // 等待一段时间，让前端有时间处理弹窗
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cout << "即将执行脚本." << std::endl;
            int result = system("/data/lintech/celectronicfence/run.sh");
            if (result!= 0) {
                std::cerr << "Script execution failed with return code: " << result << std::endl;
            } else {
                std::cout << "Script executed successfully." << std::endl;
            }
        });
        // 分离线程，使其在后台运行
        scriptThread.detach();
        std::cout << "已分离线程并尝试启动重启线程." << std::endl;

            res.status = 200;
            res.set_content("Configuration updated successfully.", "text/plain");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content("Error updating configuration: " + std::string(e.what()), "text/plain");
        } });
    //----------------------------------------------------------------------------------------------------------------------------
    // 处理开始录制的路由
    svr.Post("/start_recording", [](const Request &req, Response &res)
             {
    std::cout<< "进入函数"<<std::endl;
    try {
        json reqJson = json::parse(req.body);
        std::string rtspStreamUrl = reqJson.value("rtspStreamUrl", "");
        std::string saveLocation = reqJson.value("saveLocation", "");
        std::string rtspStreamUrl2 = reqJson.value("rtspStreamUrl2", "");
        std::string saveLocation2 = reqJson.value("saveLocation2", "");

        std::cout<< "获取到参数"<<std::endl;

        // 获取TF卡信息
        TFCardInfo tfCardInfo = getTFCardInfo();
        double freeMemoryInGB = std::stod(tfCardInfo.freeMemory);
        // 检查TF卡可用内存是否足够
        if (freeMemoryInGB < 20.0) {
            res.status = 400;
            res.set_content("TF card has insufficient free memory. Please replace with a larger capacity TF card.", "text/plain");
            return;
        }

        std::cout<< "tf信息正确"<<std::endl;
        // 直接调用startRecording函数在主线程中启动录制
            startRecording(rtspStreamUrl, saveLocation,rtspStreamUrl2, saveLocation2);
        std::cout<< "函数启动成功"<<std::endl;
        //有问题，启动ffmpeg进程一直开着会把这个阻塞，前端无法弹出弹窗，先注释掉。
            res.status = 200;
        res.set_content("Recording started successfully.", "text/plain");
    } catch (const std::exception& e) {
        std::cout<< "报错"<<std::endl;
        res.status = 500;
        res.set_content("Error starting recording: " + std::string(e.what()), "text/plain");
    } });
    //----------------------------------------------------------------------------------------------------------------------------
    // 添加处理结束录制的路由
    svr.Post("/stop_recording", [](const Request &req, Response &res)
             {
    // 直接调用stopRecording函数在主线程中停止录制
        stopRecording();
    res.status = 200;
    res.set_content("Recording stopped successfully.", "text/plain"); });
    //----------------------------------------------------------------------------------------------------------------------------
    // 启动定时器线程
    std::thread timerThread(timerThreadFunction);
    timerThread.detach();
    //----------------------------------------------------------------------------------------------------------------------------
    // 读取json文件中的录制状态返回给前端
    svr.Get("/get_recording_status", [](const Request &req, Response &res)
            {
    std::ifstream statusFile("recording_status.json");
    if (statusFile.is_open()) {
        json statusJson;
        statusFile >> statusJson;
        statusFile.close();
        bool recordingStatus = statusJson.value("recording_status", false);
        res.set_content(std::to_string(recordingStatus), "application/json");
    } else {
        std::cerr << "无法打开recording_status.json文件来读取录制状态。" << std::endl;
        res.status = 500;
        res.set_content("Error reading recording status.", "text/plain");
    } });
    //----------------------------------------------------------------------------------------------------------------------------
    svr.Get("/get_all_default_addresses", [](const Request &req, Response &res)
            {
    json defaultAddresses;
    readDefaultAddressesFromJson(defaultAddresses);
    json responseJson = {
        {"defaultRtspStreamUrl", defaultAddresses["current"].value("defaultRtspStreamUrl", "")},
        {"defaultSaveLocation", defaultAddresses["current"].value("defaultSaveLocation", "")},
        {"defaultRtspStreamUrl2", defaultAddresses["current"].value("defaultRtspStreamUrl2", "")},
        {"defaultSaveLocation2", defaultAddresses["current"].value("defaultSaveLocation2", "")}
    };
    res.set_content(responseJson.dump(), "application/json"); });
    //----------------------------------------------------------------------------------------------------------------------------
svr.Post("/update_default_addresses", [](const Request &req, Response &res)
{
    try
    {
        json reqJson = json::parse(req.body);
        json defaultAddresses;
        readDefaultAddressesFromJson(defaultAddresses);

        // 确保defaultAddresses中存在"current"节点，如果不存在则创建一个空对象作为"current"节点
        if (!defaultAddresses.contains("current"))
        {
            defaultAddresses["current"] = json::object();
        }

        // 更新各个默认地址信息，只操作"current"节点下的字段
        defaultAddresses["current"]["defaultRtspStreamUrl"] = reqJson.value("rtspStreamUrl", "");
        defaultAddresses["current"]["defaultSaveLocation"] = reqJson.value("saveLocation", "");
        defaultAddresses["current"]["defaultRtspStreamUrl2"] = reqJson.value("rtspStreamUrl2", "");
        defaultAddresses["current"]["defaultSaveLocation2"] = reqJson.value("saveLocation2", "");

        updateDefaultAddressesInJson(defaultAddresses);

        res.status = 200;
        res.set_content("Default addresses updated successfully.", "text/plain");
    }
    catch (const std::exception& e)
    {
        res.status = 500;
        res.set_content("Error updating default addresses: " + std::string(e.what()), "text/plain");
    }
});
//----------------------------------------------------------------------------------------------------------------------------
// 处理设置默认地址（更新default.json文件中"initial"部分）的路由
svr.Post("/set_default_addresses", [](const Request &req, Response &res)
{
    try
    {
        json reqJson = json::parse(req.body);
        json defaultAddresses;
        readDefaultAddressesFromJson(defaultAddresses);

        // 确保defaultAddresses中存在"initial"节点，如果不存在则创建一个空对象作为"initial"节点
        if (!defaultAddresses.contains("initial"))
        {
            defaultAddresses["initial"] = json::object();
        }

        // 更新"initial"节点下的各个默认地址信息
        defaultAddresses["initial"]["defaultRtspStreamUrl"] = reqJson.value("rtspStreamUrl", "");
        defaultAddresses["initial"]["defaultSaveLocation"] = reqJson.value("saveLocation", "");
        defaultAddresses["initial"]["defaultRtspStreamUrl2"] = reqJson.value("rtspStreamUrl2", "");
        defaultAddresses["initial"]["defaultSaveLocation2"] = reqJson.value("saveLocation2", "");

        updateDefaultAddressesInJson(defaultAddresses);

        res.status = 200;
        res.set_content("默认地址已设置成功，并更新到 'initial' 部分。", "text/plain");
    }
    catch (const std::exception &e)
    {
        res.status = 500;
        res.set_content("默认地址设置出错: " + std::string(e.what()), "text/plain");
    }
});
//----------------------------------------------------------------------------------------------------------------------------
svr.Post("/init_default_addresses", [](const Request &req, Response &res)
{
    try
    {
        // 从default.json文件读取配置信息
        std::ifstream file("default.json");
        if (file.is_open())
        {
            nlohmann::json configData;
            file >> configData;

            // 获取初始的默认地址配置部分（从"initial"节点下获取）
            nlohmann::json initialAddresses = configData["initial"];

            // 确保"current"节点存在，如果不存在则创建一个空的对象作为"current"节点
            if (!configData.contains("current"))
            {
                configData["current"] = nlohmann::json::object();
            }

            // 将初始地址覆盖到"current"节点下对应的字段中
            configData["current"]["defaultRtspStreamUrl"] = initialAddresses["defaultRtspStreamUrl"];
            configData["current"]["defaultRtspStreamUrl2"] = initialAddresses["defaultRtspStreamUrl2"];
            configData["current"]["defaultSaveLocation"] = initialAddresses["defaultSaveLocation"];
            configData["current"]["defaultSaveLocation2"] = initialAddresses["defaultSaveLocation2"];

            updateDefaultAddressesInJson(configData);

            res.status = 200;
            res.set_content("Default addresses have been initialized.", "text/plain");
            file.close();
        }
        else
        {
            res.status = 500;
            res.set_content("Error opening default.json file.", "text/plain");
        }
    }
    catch (...)
    {
        res.status = 500;
        res.set_content("Error parsing default.json file during initialization.", "text/plain");
    }
});
    //----------------------------------------------------------------------------------------------------------------------------
    // 添加人员检测API路由
    svr.Get("/api/human_detected", HumanDetection::handleHumanDetectedAPI);
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 获取芯片温度API
    svr.Get("/api/chip_temp", [](const Request &req, Response &res)
    {
        try {
            // 读取芯片温度（单位为毫摄氏度）
            std::ifstream tempFile("/sys/class/thermal/thermal_zone0/temp");
            if (tempFile.is_open()) {
                std::string tempStr;
                std::getline(tempFile, tempStr);
                tempFile.close();
                
                // 转换为摄氏度
                double temp = std::stod(tempStr) / 1000.0;
                
                json responseJson;
                responseJson["status"] = "success";
                responseJson["temperature"] = temp;
                responseJson["unit"] = "celsius";
                
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(responseJson.dump(), "application/json");
            } else {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Unable to read temperature file";
                res.status = 500;
                res.set_content(errorJson.dump(), "application/json");
            }
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 获取系统资源使用率API
    svr.Get("/api/system_resources", [](const Request &req, Response &res)
    {
        try {
            // 获取CPU使用率
            std::string topOutput = executeCommand("top -bn1 | grep 'Cpu(s)' | awk '{print $2}' | cut -d'%' -f1");
            double cpuUsage = topOutput.empty() ? 0.0 : std::stod(topOutput);
            
            // 获取内存使用率
            std::string freeOutput = executeCommand("free | grep Mem | awk '{print ($3/$2) * 100.0}'");
            double memUsage = freeOutput.empty() ? 0.0 : std::stod(freeOutput);
            
            // NPU使用率（如果系统有的话，这里使用模拟数据）
            // 可以根据实际系统调整命令
            std::string npuOutput = executeCommand("cat /sys/kernel/debug/bm-sophon/npu_usage 2>/dev/null || echo '0'");
            double npuUsage = npuOutput.empty() ? 0.0 : std::stod(npuOutput);
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["cpu"] = cpuUsage;
            responseJson["memory"] = memUsage;
            responseJson["npu"] = npuUsage;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 获取设备信息API
    svr.Get("/api/device_info", [](const Request &req, Response &res)
    {
        try {
            // 获取设备ID（MAC地址或其他唯一标识）
            std::string deviceId = executeCommand("cat /sys/class/net/eth0/address 2>/dev/null || echo 'Unknown'");
            deviceId.erase(std::remove(deviceId.begin(), deviceId.end(), '\n'), deviceId.end());
            
            // 获取系统版本
            std::string systemVersion = executeCommand("uname -r");
            systemVersion.erase(std::remove(systemVersion.begin(), systemVersion.end(), '\n'), systemVersion.end());
            
            // 获取存储信息
            TFCardInfo tfCardInfo = getTFCardInfo();
            std::string storageInfo = "存储空间(" + tfCardInfo.usedMemory + "/" + tfCardInfo.freeMemory + ")";
            
            // 获取软件版本（可以从配置文件读取）
            std::string softwareVersion = "1.0.65 Patch 2";
            
            // GPS信息（如果有的话）
            std::string gpsInfo = "纬度(-) 经度(-) UTC时间(-)";
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["deviceId"] = deviceId;
            responseJson["systemVersion"] = systemVersion;
            responseJson["storage"] = storageInfo;
            responseJson["softwareVersion"] = softwareVersion;
            responseJson["gps"] = gpsInfo;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 获取通道列表API
    svr.Get("/api/channels", [](const Request &req, Response &res)
    {
        try {
            std::ifstream channelsFile("channels.json");
            json responseJson;
            
            if (channelsFile.is_open()) {
                json channelsData;
                channelsFile >> channelsData;
                channelsFile.close();
                
                responseJson["status"] = "success";
                responseJson["channels"] = channelsData["channels"];
            } else {
                // 如果文件不存在，返回空数组
                responseJson["status"] = "success";
                responseJson["channels"] = json::array();
            }
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 添加通道API
    svr.Post("/api/channels", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            
            // 读取现有通道
            json channelsData;
            std::ifstream channelsFile("channels.json");
            if (channelsFile.is_open()) {
                channelsFile >> channelsData;
                channelsFile.close();
            } else {
                channelsData["channels"] = json::array();
            }
            
            // 生成新ID
            int newId = 1;
            if (!channelsData["channels"].empty()) {
                for (const auto& channel : channelsData["channels"]) {
                    if (channel["id"] >= newId) {
                        newId = channel["id"].get<int>() + 1;
                    }
                }
            }
            
            // 创建新通道
            json newChannel;
            newChannel["id"] = newId;
            newChannel["name"] = requestData["name"];
            newChannel["url"] = requestData["url"];
            newChannel["channelAssignment"] = requestData["channelAssignment"];
            newChannel["description"] = requestData.value("description", "");
            newChannel["gb28181"] = requestData.value("gb28181", "");
            newChannel["status"] = requestData.value("status", "configured");
            
            // 添加到数组
            channelsData["channels"].push_back(newChannel);
            
            // 保存到文件
            std::ofstream outFile("channels.json");
            if (outFile.is_open()) {
                outFile << channelsData.dump(4);
                outFile.close();
                
                json responseJson;
                responseJson["status"] = "success";
                responseJson["message"] = "Channel added successfully";
                responseJson["channel"] = newChannel;
                
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(responseJson.dump(), "application/json");
            } else {
                throw std::runtime_error("Cannot write to channels.json");
            }
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 更新通道API
    svr.Put(R"(/api/channels/(\d+))", [](const Request &req, Response &res)
    {
        try {
            int channelId = std::stoi(req.matches[1]);
            json requestData = json::parse(req.body);
            
            // 读取现有通道
            std::ifstream channelsFile("channels.json");
            if (!channelsFile.is_open()) {
                throw std::runtime_error("Cannot read channels.json");
            }
            
            json channelsData;
            channelsFile >> channelsData;
            channelsFile.close();
            
            // 查找并更新通道
            bool found = false;
            for (auto& channel : channelsData["channels"]) {
                if (channel["id"] == channelId) {
                    channel["name"] = requestData["name"];
                    channel["url"] = requestData["url"];
                    channel["channelAssignment"] = requestData["channelAssignment"];
                    channel["description"] = requestData.value("description", "");
                    channel["gb28181"] = requestData.value("gb28181", "");
                    channel["status"] = requestData.value("status", channel["status"]);
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                throw std::runtime_error("Channel not found");
            }
            
            // 保存到文件
            std::ofstream outFile("channels.json");
            if (outFile.is_open()) {
                outFile << channelsData.dump(4);
                outFile.close();
                
                json responseJson;
                responseJson["status"] = "success";
                responseJson["message"] = "Channel updated successfully";
                
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(responseJson.dump(), "application/json");
            } else {
                throw std::runtime_error("Cannot write to channels.json");
            }
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 删除通道API
    svr.Delete(R"(/api/channels/(\d+))", [](const Request &req, Response &res)
    {
        try {
            int channelId = std::stoi(req.matches[1]);
            
            // 读取现有通道
            std::ifstream channelsFile("channels.json");
            if (!channelsFile.is_open()) {
                throw std::runtime_error("Cannot read channels.json");
            }
            
            json channelsData;
            channelsFile >> channelsData;
            channelsFile.close();
            
            // 删除通道
            auto& channels = channelsData["channels"];
            bool found = false;
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                if ((*it)["id"] == channelId) {
                    channels.erase(it);
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                throw std::runtime_error("Channel not found");
            }
            
            // 保存到文件
            std::ofstream outFile("channels.json");
            if (outFile.is_open()) {
                outFile << channelsData.dump(4);
                outFile.close();
                
                json responseJson;
                responseJson["status"] = "success";
                responseJson["message"] = "Channel deleted successfully";
                
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content(responseJson.dump(), "application/json");
            } else {
                throw std::runtime_error("Cannot write to channels.json");
            }
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 检测RTSP流地址是否可用API
    svr.Post("/api/channels/check", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            std::string url = requestData["url"];
            
            // 使用 sophon-ffprobe 检测流地址
            // 设置超时时间为10秒，避免长时间等待
            std::string ffprobePath = "/opt/sophon/sophon-ffmpeg-latest/bin/ffprobe";
            std::string checkCommand = "timeout 10 " + ffprobePath + " -v error -show_entries stream=codec_type -of default=noprint_wrappers=1 \"" + url + "\" 2>&1";
            std::string result = executeCommand(checkCommand.c_str());
            
            // 调试日志
            std::cout << "检测流地址: " << url << std::endl;
            std::cout << "ffprobe输出: " << result << std::endl;
            
            json responseJson;
            responseJson["status"] = "success";
            
            // 检查ffprobe输出 - 只要找到codec_type=video就认为流可用
            if (result.find("codec_type=video") != std::string::npos) {
                responseJson["streamStatus"] = "active";
                responseJson["message"] = "流地址正常";
                
                // 尝试获取分辨率信息
                std::string resCommand = "timeout 10 " + ffprobePath + " -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 \"" + url + "\" 2>&1";
                std::string resResult = executeCommand(resCommand.c_str());
                // 移除换行符和其他空白字符
                resResult.erase(std::remove(resResult.begin(), resResult.end(), '\n'), resResult.end());
                resResult.erase(std::remove(resResult.begin(), resResult.end(), '\r'), resResult.end());
                resResult.erase(std::remove(resResult.begin(), resResult.end(), ' '), resResult.end());
                
                if (!resResult.empty() && resResult.find("x") != std::string::npos) {
                    responseJson["resolution"] = resResult;
                } else {
                    responseJson["resolution"] = "";
                }
            } else if (result.find("Connection refused") != std::string::npos) {
                responseJson["streamStatus"] = "error";
                responseJson["message"] = "连接被拒绝";
                responseJson["resolution"] = "";
            } else if (result.find("Connection timed out") != std::string::npos || 
                       result.find("timed out") != std::string::npos) {
                responseJson["streamStatus"] = "error";
                responseJson["message"] = "连接超时";
                responseJson["resolution"] = "";
            } else if (result.find("Invalid data") != std::string::npos) {
                responseJson["streamStatus"] = "invalid";
                responseJson["message"] = "流数据无效";
                responseJson["resolution"] = "";
            } else if (result.find("Protocol not found") != std::string::npos) {
                responseJson["streamStatus"] = "invalid";
                responseJson["message"] = "协议不支持";
                responseJson["resolution"] = "";
            } else if (result.find("Server returned 404") != std::string::npos || 
                       result.find("not found") != std::string::npos) {
                responseJson["streamStatus"] = "invalid";
                responseJson["message"] = "流地址不存在";
                responseJson["resolution"] = "";
            } else {
                // 如果没有明确的错误但也没检测到视频，返回详细信息用于调试
                responseJson["streamStatus"] = "inactive";
                responseJson["message"] = "流地址无法访问";
                responseJson["resolution"] = "";
                responseJson["debug"] = result.substr(0, 200); // 返回前200字符用于调试
            }
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["streamStatus"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 获取已分配的通道地址API
    svr.Get("/api/channels/assigned", [](const Request &req, Response &res)
    {
        try {
            std::ifstream channelsFile("channels.json");
            json responseJson;
            responseJson["status"] = "success";
            responseJson["channel1"] = "";
            responseJson["channel2"] = "";
            
            if (channelsFile.is_open()) {
                json channelsData;
                channelsFile >> channelsData;
                channelsFile.close();
                
                // 查找分配给通道1和通道2的地址
                for (const auto& channel : channelsData["channels"]) {
                    if (channel["channelAssignment"] == "1") {
                        responseJson["channel1"] = channel["url"];
                    } else if (channel["channelAssignment"] == "2") {
                        responseJson["channel2"] = channel["url"];
                    }
                }
            }
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 任务管理API
    
    // 全局变量：记录最后一次停止任务的时间
    static std::chrono::time_point<std::chrono::system_clock> lastStopTime = std::chrono::system_clock::now() - std::chrono::seconds(60);
    static std::mutex stopTimeMutex;
    
    // 获取所有任务
    svr.Get("/api/tasks", [](const Request &req, Response &res)
    {
        try {
            std::ifstream file("/data/lintech/celectronicfence/tasks.json");
            json tasksJson;
            
            if (file.is_open()) {
                file >> tasksJson;
                file.close();
            } else {
                tasksJson = json::array();
            }
            
            // 检查是否在停止任务后的5秒内（避免自动同步覆盖停止状态）
            bool skipAutoSync = false;
            {
                std::lock_guard<std::mutex> lock(stopTimeMutex);
                auto timeSinceStop = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now() - lastStopTime).count();
                if (timeSinceStop < 5) {
                    skipAutoSync = true;
                    std::cout << "[自动同步] 跳过自动同步（停止任务后 " << timeSinceStop << " 秒）" << std::endl;
                }
            }
            
            // 检测实际运行的main进程并同步状态
            // 使用更精确的匹配：查找 ./main 或 /main 可执行文件，且带有 demo_config_path 参数
            FILE* pidPipe = popen("ps aux | grep '[/]main.*demo_config_path' | grep -v grep | awk '{print $2}' | head -1", "r");
            int actualPid = 0;
            if (pidPipe) {
                char pidBuffer[32];
                if (fgets(pidBuffer, sizeof(pidBuffer), pidPipe) != nullptr) {
                    actualPid = std::stoi(pidBuffer);
                    std::cout << "[自动同步] 检测到进程 PID: " << actualPid << std::endl;
                } else {
                    std::cout << "[自动同步] 未检测到运行中的进程" << std::endl;
                }
                pclose(pidPipe);
            }
            
            // 同步任务状态（如果不在禁用期内）
            bool taskUpdated = false;
            if (!skipAutoSync) {
                for (auto& task : tasksJson) {
                    int taskPid = task.value("pid", 0);
                    std::string taskStatus = task.value("status", "stopped");
                    
                    if (actualPid > 0) {
                        // 有进程在运行，且任务当前状态不是running或PID不匹配
                        if (taskStatus != "running" || taskPid != actualPid) {
                            // 只在任务状态为stopped或PID不匹配时才更新
                            // 避免在刚停止任务时立即同步回运行状态
                            if (taskStatus == "stopped" || (taskPid != actualPid && taskPid != 0)) {
                                task["status"] = "running";
                                task["pid"] = actualPid;
                                taskUpdated = true;
                                std::cout << "[自动同步] 检测到运行中的进程 PID: " << actualPid << "，更新状态" << std::endl;
                            }
                        }
                    } else {
                        // 没有进程运行，如果任务显示为运行中则更新为停止
                        if (taskStatus == "running") {
                            task["status"] = "stopped";
                            task["pid"] = 0;
                            taskUpdated = true;
                            std::cout << "[自动同步] 进程已停止，更新任务状态" << std::endl;
                        }
                    }
                }
            }
            
            // 如果状态有更新，保存到文件
            if (taskUpdated) {
                std::ofstream fileOut("/data/lintech/celectronicfence/tasks.json");
                if (fileOut.is_open()) {
                    fileOut << tasksJson.dump(4);
                    fileOut.close();
                }
            }
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["tasks"] = tasksJson;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 添加新任务
    svr.Post("/api/tasks", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            
            // 读取现有任务
            std::ifstream fileIn("/data/lintech/celectronicfence/tasks.json");
            json tasksJson;
            
            if (fileIn.is_open()) {
                fileIn >> tasksJson;
                fileIn.close();
            } else {
                tasksJson = json::array();
            }
            
            // 生成新ID
            int newId = 1;
            if (!tasksJson.empty()) {
                for (const auto& task : tasksJson) {
                    if (task.contains("id") && task["id"].is_number()) {
                        newId = std::max(newId, (int)task["id"] + 1);
                    }
                }
            }
            
            // 创建新任务
            json newTask;
            newTask["id"] = newId;
            newTask["taskNumber"] = requestData["taskNumber"];
            newTask["description"] = requestData.value("description", "");
            newTask["videoSourceId"] = requestData["videoSourceId"];
            newTask["algorithm"] = requestData["algorithm"];
            newTask["status"] = "stopped";
            newTask["pid"] = 0;
            
            tasksJson.push_back(newTask);
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/tasks.json");
            fileOut << tasksJson.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Task added successfully";
            responseJson["task"] = newTask;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 更新任务
    svr.Put("/api/tasks/:id", [](const Request &req, Response &res)
    {
        try {
            int taskId = std::stoi(req.path_params.at("id"));
            json requestData = json::parse(req.body);
            
            // 读取现有任务
            std::ifstream fileIn("/data/lintech/celectronicfence/tasks.json");
            json tasksJson;
            
            if (fileIn.is_open()) {
                fileIn >> tasksJson;
                fileIn.close();
            } else {
                tasksJson = json::array();
            }
            
            // 查找并更新任务
            bool found = false;
            for (auto& task : tasksJson) {
                if (task["id"] == taskId) {
                    task["taskNumber"] = requestData["taskNumber"];
                    task["description"] = requestData.value("description", "");
                    task["videoSourceId"] = requestData["videoSourceId"];
                    task["algorithm"] = requestData["algorithm"];
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Task not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/tasks.json");
            fileOut << tasksJson.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Task updated successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 删除任务
    svr.Delete("/api/tasks/:id", [](const Request &req, Response &res)
    {
        try {
            int taskId = std::stoi(req.path_params.at("id"));
            
            // 读取现有任务
            std::ifstream fileIn("/data/lintech/celectronicfence/tasks.json");
            json tasksJson;
            
            if (fileIn.is_open()) {
                fileIn >> tasksJson;
                fileIn.close();
            } else {
                tasksJson = json::array();
            }
            
            // 删除任务
            json newTasksJson = json::array();
            bool found = false;
            for (const auto& task : tasksJson) {
                if (task["id"] == taskId) {
                    // 如果任务正在运行，先停止它
                    if (task.contains("pid") && task["pid"].is_number() && task["pid"] > 0) {
                        std::string killCmd = "kill -9 " + std::to_string((int)task["pid"]);
                        std::system(killCmd.c_str());
                    }
                    found = true;
                } else {
                    newTasksJson.push_back(task);
                }
            }
            
            if (!found) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Task not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/tasks.json");
            fileOut << newTasksJson.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Task deleted successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 启动任务
    svr.Post("/api/tasks/:id/start", [](const Request &req, Response &res)
    {
        try {
            int taskId = std::stoi(req.path_params.at("id"));
            
            // 读取现有任务
            std::ifstream fileIn("/data/lintech/celectronicfence/tasks.json");
            json tasksJson;
            
            if (fileIn.is_open()) {
                fileIn >> tasksJson;
                fileIn.close();
            } else {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Tasks file not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 查找任务
            bool found = false;
            for (auto& task : tasksJson) {
                if (task["id"] == taskId) {
                    // 获取任务的videoSourceId
                    int videoSourceId = task.value("videoSourceId", 0);
                    std::cout << "[启动任务] 任务ID: " << taskId << ", 视频源ID: " << videoSourceId << std::endl;
                    
                    // 读取channels.json获取通道信息
                    std::ifstream channelsFile("/data/lintech/celectronicfence/channels.json");
                    if (channelsFile.is_open()) {
                        json channelsData;
                        channelsFile >> channelsData;
                        channelsFile.close();
                        
                        // 查找对应的通道
                        std::string channelUrl = "";
                        int channelId = 20; // 默认通道20
                        
                        if (channelsData.contains("channels")) {
                            for (const auto& channel : channelsData["channels"]) {
                                if (channel["id"] == videoSourceId) {
                                    channelUrl = channel.value("url", "");
                                    std::string channelAssignment = channel.value("channelAssignment", "1");
                                    // channelAssignment: "1" -> channel_id: 20, "2" -> channel_id: 10
                                    channelId = (channelAssignment == "2") ? 10 : 20;
                                    std::cout << "[启动任务] 找到通道: URL=" << channelUrl << ", channelId=" << channelId << std::endl;
                                    break;
                                }
                            }
                        }
                        
                        // 更新yolov8_demo.json配置，只保留选中的通道
                        if (!channelUrl.empty()) {
                            std::ifstream demoConfigIn("/data/sophon-stream/samples/yolov8/config/yolov8_demo.json");
                            json demoConfig;
                            if (demoConfigIn.is_open()) {
                                demoConfigIn >> demoConfig;
                                demoConfigIn.close();
                            } else {
                                // 如果读取失败，使用默认配置
                                demoConfig = {
                                    {"class_names", "../yolov8/data/coco.names"},
                                    {"download_image", false},
                                    {"draw_func_name", "draw_yolov5_results"},
                                    {"engine_config_path", "../yolov8/config/engine_group.json"}
                                };
                            }
                            
                            // 只更新channels数组，保留其他所有配置
                            json selectedChannel = {
                                {"channel_id", channelId},
                                {"fps", 3},
                                {"loop_num", 1},
                                {"sample_interval", 2},
                                {"source_type", "RTSP"},
                                {"url", channelUrl}
                            };
                            
                            demoConfig["channels"] = json::array();
                            demoConfig["channels"].push_back(selectedChannel);
                            
                            // 确保所有必要的字段都存在
                            if (!demoConfig.contains("class_names")) {
                                demoConfig["class_names"] = "../yolov8/data/coco.names";
                            }
                            if (!demoConfig.contains("download_image")) {
                                demoConfig["download_image"] = false;
                            }
                            if (!demoConfig.contains("draw_func_name")) {
                                demoConfig["draw_func_name"] = "draw_yolov5_results";
                            }
                            if (!demoConfig.contains("engine_config_path")) {
                                demoConfig["engine_config_path"] = "../yolov8/config/engine_group.json";
                            }
                            
                            // 保存更新后的配置
                            std::ofstream demoConfigOut("/data/sophon-stream/samples/yolov8/config/yolov8_demo.json");
                            if (demoConfigOut.is_open()) {
                                demoConfigOut << demoConfig.dump(4);
                                demoConfigOut.close();
                                std::cout << "[启动任务] 已更新yolov8_demo.json配置，使用通道" << channelId << std::endl;
                            }
                        } else {
                            std::cerr << "[启动任务] 警告：未找到视频源URL，使用默认配置" << std::endl;
                        }
                    } else {
                        std::cerr << "[启动任务] 警告：无法读取channels.json，使用默认配置" << std::endl;
                    }
                    
                    // 先终止已存在的程序
                    std::system("sudo killall -9 main 2>/dev/null");
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    // 设置环境变量并启动脚本
                    std::string scriptPath = "/data/sophon-stream/samples/yolov8/scripts/run_hdmi_show.sh";
                    std::string command = "export PYTHONPATH=$PYTHONPATH:/opt/sophon/sophon-opencv_1.9.0/opencv-python && "
                                        "cd /data/sophon-stream/samples/yolov8/scripts && "
                                        "nohup sudo " + scriptPath + " > /tmp/task_" + std::to_string(taskId) + ".log 2>&1 & echo $!";
                    
                    std::cout << "执行启动命令: " << command << std::endl;
                    
                    FILE* pipe = popen(command.c_str(), "r");
                    if (pipe) {
                        char buffer[128];
                        std::string result = "";
                        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                            result += buffer;
                        }
                        pclose(pipe);
                        
                        // 等待一下确保进程启动
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        
                        // 获取实际的main进程PID
                        // 使用精确匹配避免误匹配
                        FILE* pidPipe = popen("ps aux | grep '[/]main.*demo_config_path' | grep -v grep | awk '{print $2}' | head -1", "r");
                        int pid = 0;
                        if (pidPipe) {
                            char pidBuffer[32];
                            if (fgets(pidBuffer, sizeof(pidBuffer), pidPipe) != nullptr) {
                                pid = std::stoi(pidBuffer);
                            }
                            pclose(pidPipe);
                        }
                        
                        task["status"] = "running";
                        task["pid"] = pid;
                        
                        // 保存到文件
                        std::ofstream fileOut("/data/lintech/celectronicfence/tasks.json");
                        fileOut << tasksJson.dump(4);
                        fileOut.close();
                        
                        std::cout << "任务启动成功，PID: " << pid << std::endl;
                        found = true;
                    } else {
                        json errorJson;
                        errorJson["status"] = "error";
                        errorJson["message"] = "Failed to start task script";
                        res.status = 500;
                        res.set_content(errorJson.dump(), "application/json");
                        return;
                    }
                    break;
                }
            }
            
            if (!found) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Task not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Task started successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 停止任务
    svr.Post("/api/tasks/:id/stop", [](const Request &req, Response &res)
    {
        try {
            int taskId = std::stoi(req.path_params.at("id"));
            
            // 读取现有任务
            std::ifstream fileIn("/data/lintech/celectronicfence/tasks.json");
            json tasksJson;
            
            if (fileIn.is_open()) {
                fileIn >> tasksJson;
                fileIn.close();
            } else {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Tasks file not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 查找任务
            bool found = false;
            for (auto& task : tasksJson) {
                if (task["id"] == taskId) {
                    std::cout << "停止任务 " << taskId << std::endl;
                    
                    // 获取当前运行的算法进程PID
                    // 使用精确匹配避免误匹配
                    FILE* pidPipe = popen("ps aux | grep '[/]main.*demo_config_path' | grep -v grep | awk '{print $2}' | head -1", "r");
                    int pid = 0;
                    if (pidPipe) {
                        char pidBuffer[32];
                        if (fgets(pidBuffer, sizeof(pidBuffer), pidPipe) != nullptr) {
                            pid = std::stoi(pidBuffer);
                        }
                        pclose(pidPipe);
                    }
                    
                    // 只终止特定的算法进程（不是killall，避免误杀服务器）
                    if (pid > 0) {
                        std::string killCmd = "sudo kill -15 " + std::to_string(pid) + " 2>/dev/null";
                        std::system(killCmd.c_str());
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                        
                        // 检查进程是否还在运行
                        std::string checkCmd = "ps -p " + std::to_string(pid) + " > /dev/null 2>&1";
                        if (std::system(checkCmd.c_str()) == 0) {
                            // 进程还在，强制终止
                            killCmd = "sudo kill -9 " + std::to_string(pid) + " 2>/dev/null";
                            std::system(killCmd.c_str());
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                        
                        std::cout << "已终止进程 PID: " << pid << std::endl;
                    }
                    
                    // 等待进程完全终止，最多等待3秒
                    bool processTerminated = false;
                    for (int i = 0; i < 10; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        
                        // 检查进程是否还在运行
                        FILE* checkPipe = popen("ps aux | grep '[/]main.*demo_config_path' | grep -v grep | awk '{print $2}' | head -1", "r");
                        char checkBuffer[32];
                        bool processExists = false;
                        if (checkPipe) {
                            if (fgets(checkBuffer, sizeof(checkBuffer), checkPipe) != nullptr) {
                                processExists = true;
                            }
                            pclose(checkPipe);
                        }
                        
                        if (!processExists) {
                            processTerminated = true;
                            std::cout << "进程已完全终止" << std::endl;
                            break;
                        }
                    }
                    
                    if (!processTerminated) {
                        std::cout << "警告：进程可能未完全终止" << std::endl;
                    }
                    
                    task["status"] = "stopped";
                    task["pid"] = 0;
                    
                    // 保存到文件
                    std::ofstream fileOut("/data/lintech/celectronicfence/tasks.json");
                    fileOut << tasksJson.dump(4);
                    fileOut.close();
                    
                    // 更新最后停止时间，禁用自动同步5秒
                    {
                        std::lock_guard<std::mutex> lock(stopTimeMutex);
                        lastStopTime = std::chrono::system_clock::now();
                    }
                    
                    std::cout << "任务停止成功" << std::endl;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Task not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Task stopped successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 网络配置API
    
    // 获取网络配置
    svr.Get("/api/network/config", [](const Request &req, Response &res)
    {
        try {
            std::ifstream file("/etc/netplan/01-netcfg.yaml");
            std::string content;
            std::string line;
            
            if (file.is_open()) {
                while (std::getline(file, line)) {
                    content += line + "\n";
                }
                file.close();
            } else {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "无法读取网络配置文件";
                res.status = 500;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 解析YAML配置（简单解析）
            json config;
            json eth0Config, eth1Config;
            
            std::istringstream iss(content);
            std::string currentInterface = "";
            
            while (std::getline(iss, line)) {
                // 检测接口名称
                if (line.find("eth0:") != std::string::npos) {
                    currentInterface = "eth0";
                } else if (line.find("eth1:") != std::string::npos) {
                    currentInterface = "eth1";
                }
                
                if (currentInterface.empty()) continue;
                
                json* currentConfig = (currentInterface == "eth0") ? &eth0Config : &eth1Config;
                
                // 解析dhcp4
                if (line.find("dhcp4:") != std::string::npos) {
                    if (line.find("yes") != std::string::npos) {
                        (*currentConfig)["dhcp4"] = "yes";
                    } else {
                        (*currentConfig)["dhcp4"] = "no";
                    }
                }
                
                // 解析addresses
                if (line.find("addresses:") != std::string::npos) {
                    (*currentConfig)["addresses"] = json::array();
                    // 读取下一行的地址
                    if (std::getline(iss, line)) {
                        size_t bracketStart = line.find("[");
                        size_t bracketEnd = line.find("]");
                        if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
                            std::string addressesStr = line.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
                            // 移除引号和空格
                            addressesStr.erase(std::remove(addressesStr.begin(), addressesStr.end(), '"'), addressesStr.end());
                            addressesStr.erase(std::remove(addressesStr.begin(), addressesStr.end(), ' '), addressesStr.end());
                            if (!addressesStr.empty()) {
                                (*currentConfig)["addresses"].push_back(addressesStr);
                            }
                        }
                    }
                }
                
                // 解析gateway4
                if (line.find("gateway4:") != std::string::npos) {
                    size_t colonPos = line.find(":");
                    if (colonPos != std::string::npos) {
                        std::string gateway = line.substr(colonPos + 1);
                        gateway.erase(0, gateway.find_first_not_of(" \t"));
                        gateway.erase(gateway.find_last_not_of(" \t\r\n") + 1);
                        (*currentConfig)["gateway4"] = gateway;
                    }
                }
                
                // 解析nameservers
                if (line.find("nameservers:") != std::string::npos) {
                    (*currentConfig)["nameservers"] = json::object();
                    // 读取addresses行
                    if (std::getline(iss, line) && line.find("addresses:") != std::string::npos) {
                        if (std::getline(iss, line)) {
                            size_t bracketStart = line.find("[");
                            size_t bracketEnd = line.find("]");
                            if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
                                std::string dnsStr = line.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
                                json dnsArray = json::array();
                                
                                // 分割DNS服务器
                                size_t pos = 0;
                                while ((pos = dnsStr.find(",")) != std::string::npos) {
                                    std::string dns = dnsStr.substr(0, pos);
                                    dns.erase(std::remove(dns.begin(), dns.end(), ' '), dns.end());
                                    if (!dns.empty()) {
                                        dnsArray.push_back(dns);
                                    }
                                    dnsStr.erase(0, pos + 1);
                                }
                                dnsStr.erase(std::remove(dnsStr.begin(), dnsStr.end(), ' '), dnsStr.end());
                                if (!dnsStr.empty()) {
                                    dnsArray.push_back(dnsStr);
                                }
                                
                                (*currentConfig)["nameservers"]["addresses"] = dnsArray;
                            }
                        }
                    }
                }
            }
            
            config["eth0"] = eth0Config;
            config["eth1"] = eth1Config;
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["config"] = config;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 保存网络配置
    svr.Post("/api/network/config", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            json config = requestData["config"];
            
            // 生成netplan配置文件内容
            std::ostringstream yamlContent;
            yamlContent << "network:\n";
            yamlContent << "        version: 2\n";
            yamlContent << "        renderer: networkd\n";
            yamlContent << "        ethernets:\n";
            
            // eth0配置
            if (config.contains("eth0")) {
                json eth0 = config["eth0"];
                yamlContent << "                eth0:\n";
                
                if (eth0.contains("dhcp4")) {
                    yamlContent << "                        dhcp4: " << eth0["dhcp4"].get<std::string>() << "\n";
                }
                
                if (eth0.value("dhcp4", "no") == "no") {
                    if (eth0.contains("addresses") && eth0["addresses"].is_array() && !eth0["addresses"].empty()) {
                        yamlContent << "                        addresses: [" << eth0["addresses"][0].get<std::string>() << "]\n";
                    }
                    
                    yamlContent << "                        optional: yes\n";
                    
                    if (eth0.contains("gateway4")) {
                        yamlContent << "                        gateway4: " << eth0["gateway4"].get<std::string>() << "\n";
                    }
                    
                    if (eth0.contains("nameservers") && eth0["nameservers"].contains("addresses")) {
                        yamlContent << "                        nameservers:\n";
                        yamlContent << "                                addresses: [";
                        json dnsServers = eth0["nameservers"]["addresses"];
                        for (size_t i = 0; i < dnsServers.size(); ++i) {
                            if (i > 0) yamlContent << ", ";
                            yamlContent << dnsServers[i].get<std::string>();
                        }
                        yamlContent << "]\n";
                    }
                }
            }
            
            // eth1配置
            if (config.contains("eth1")) {
                json eth1 = config["eth1"];
                yamlContent << "                eth1:\n";
                
                if (eth1.contains("dhcp4")) {
                    yamlContent << "                        dhcp4: " << eth1["dhcp4"].get<std::string>() << "\n";
                }
                
                if (eth1.value("dhcp4", "no") == "no") {
                    if (eth1.contains("addresses") && eth1["addresses"].is_array() && !eth1["addresses"].empty()) {
                        yamlContent << "                        addresses: [" << eth1["addresses"][0].get<std::string>() << "]\n";
                    }
                    
                    yamlContent << "                        optional: yes\n";
                    
                    if (eth1.contains("gateway4")) {
                        yamlContent << "                        gateway4: " << eth1["gateway4"].get<std::string>() << "\n";
                    }
                    
                    if (eth1.contains("nameservers") && eth1["nameservers"].contains("addresses")) {
                        yamlContent << "                        nameservers:\n";
                        yamlContent << "                                addresses: [";
                        json dnsServers = eth1["nameservers"]["addresses"];
                        for (size_t i = 0; i < dnsServers.size(); ++i) {
                            if (i > 0) yamlContent << ", ";
                            yamlContent << dnsServers[i].get<std::string>();
                        }
                        yamlContent << "]\n";
                    }
                }
            }
            
            std::string yamlStr = yamlContent.str();
            std::cout << "生成的netplan配置:\n" << yamlStr << std::endl;
            
            // 先写入临时文件
            std::ofstream tempFile("/tmp/01-netcfg.yaml.tmp");
            if (!tempFile.is_open()) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "无法创建临时配置文件";
                res.status = 500;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            tempFile << yamlStr;
            tempFile.close();
            
            // 使用sudo复制到目标位置并应用
            std::string copyCmd = "sudo cp /tmp/01-netcfg.yaml.tmp /etc/netplan/01-netcfg.yaml";
            std::string applyCmd = "sudo netplan apply";
            
            int copyResult = std::system(copyCmd.c_str());
            if (copyResult != 0) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "无法保存网络配置文件";
                res.status = 500;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 应用配置
            std::system(applyCmd.c_str());
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "网络配置已保存并应用";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 检测网络状态
    svr.Get("/api/network/status", [](const Request &req, Response &res)
    {
        try {
            json interfaces;
            
            // 检测eth0状态
            std::string eth0CarrierCmd = "cat /sys/class/net/eth0/carrier 2>/dev/null";
            std::string eth0Carrier = executeCommand(eth0CarrierCmd.c_str());
            eth0Carrier.erase(std::remove(eth0Carrier.begin(), eth0Carrier.end(), '\n'), eth0Carrier.end());
            eth0Carrier.erase(std::remove(eth0Carrier.begin(), eth0Carrier.end(), '\r'), eth0Carrier.end());
            
            interfaces["eth0"]["connected"] = (eth0Carrier == "1");
            
            // 检测eth1状态
            std::string eth1CarrierCmd = "cat /sys/class/net/eth1/carrier 2>/dev/null";
            std::string eth1Carrier = executeCommand(eth1CarrierCmd.c_str());
            eth1Carrier.erase(std::remove(eth1Carrier.begin(), eth1Carrier.end(), '\n'), eth1Carrier.end());
            eth1Carrier.erase(std::remove(eth1Carrier.begin(), eth1Carrier.end(), '\r'), eth1Carrier.end());
            
            interfaces["eth1"]["connected"] = (eth1Carrier == "1");
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["interfaces"] = interfaces;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
//----------------------------------------------------------------------------------------------------------------------------
// 告警管理API

    // 获取所有告警
    svr.Get("/api/alarms", [](const Request &req, Response &res)
    {
        try {
            std::ifstream file("/data/lintech/celectronicfence/alarms.json");
            json alarmsJson;
            
            if (file.is_open()) {
                file >> alarmsJson;
                file.close();
            } else {
                alarmsJson = json::array();
            }
            
            // 确保每个告警都有reportStatus字段（兼容旧数据）
            // 同时将reportStatus映射为前端期望的status值
            for (auto& alarm : alarmsJson) {
                if (!alarm.contains("reportStatus")) {
                    // 如果没有reportStatus字段，根据reportUrl判断
                    std::string reportUrl = alarm.value("reportUrl", "");
                    if (reportUrl.empty()) {
                        alarm["reportStatus"] = "未上报";
                    } else {
                        alarm["reportStatus"] = "未知状态"; // 旧数据，状态未知
                    }
                }
                
                // 将reportStatus映射为前端期望的status值（用于显示上报状态）
                // 前端getStatusBadge函数期望: 'reported', 'failed', 'pending'
                std::string reportStatus = alarm.value("reportStatus", "");
                std::string statusForDisplay = "pending"; // 默认值
                
                if (reportStatus == "上报成功") {
                    statusForDisplay = "reported";
                } else if (reportStatus == "上报失败") {
                    statusForDisplay = "failed";
                } else if (reportStatus == "上报中") {
                    statusForDisplay = "pending";
                } else if (reportStatus == "未上报") {
                    statusForDisplay = "pending";
                } else {
                    // "未知状态"或其他值，保持原status或设为pending
                    statusForDisplay = "pending";
                }
                
                // 如果原status字段是告警处理状态（如"未处理"），保留它
                // 但为了兼容前端显示上报状态，我们需要一个临时字段
                // 由于前端使用alarm.status来显示上报状态，我们需要将上报状态映射到status
                // 但这样会覆盖告警处理状态，所以我们需要检查
                std::string originalStatus = alarm.value("status", "");
                if (originalStatus == "未处理" || originalStatus == "pending" || originalStatus == "已处理") {
                    // 这是告警处理状态，我们需要保留它，但也要让前端能显示上报状态
                    // 由于前端代码使用alarm.status，我们暂时将上报状态映射到status
                    // 更好的做法是修改前端代码，但为了快速修复，我们先这样做
                    alarm["status"] = statusForDisplay;
                    // 同时保留原始处理状态到新字段
                    alarm["processStatus"] = originalStatus;
                } else {
                    // 如果status已经是上报状态格式，直接使用
                    alarm["status"] = statusForDisplay;
                }
            }
            
            // 按时间戳倒序排序（新的在前，旧的在后）
            std::sort(alarmsJson.begin(), alarmsJson.end(), [](const json& a, const json& b) {
                std::string timestampA = a.value("timestamp", "");
                std::string timestampB = b.value("timestamp", "");
                
                // 如果时间戳格式为 "YYYY-MM-DD HH:MM:SS"，可以直接字符串比较
                // 因为这种格式的字符串排序就是时间顺序
                if (timestampA.empty() && timestampB.empty()) {
                    return false;
                }
                if (timestampA.empty()) {
                    return false; // 空时间戳排在后面
                }
                if (timestampB.empty()) {
                    return true; // 空时间戳排在后面
                }
                
                // 倒序：timestampA > timestampB 时返回true，使新的排在前面
                return timestampA > timestampB;
            });
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["alarms"] = alarmsJson;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 添加新告警
    svr.Post("/api/alarms", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            
            // 读取现有告警
            std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
            json alarmsJson;
            
            if (fileIn.is_open()) {
                fileIn >> alarmsJson;
                fileIn.close();
            } else {
                alarmsJson = json::array();
            }
            
            // 生成新ID
            int newId = 1;
            if (!alarmsJson.empty()) {
                for (const auto& alarm : alarmsJson) {
                    if (alarm.contains("id") && alarm["id"].is_number()) {
                        newId = std::max(newId, (int)alarm["id"] + 1);
                    }
                }
            }
            
            // 创建新告警
            json newAlarm;
            newAlarm["id"] = newId;
            newAlarm["taskId"] = requestData.value("taskId", 0);
            newAlarm["videoSourceId"] = requestData.value("videoSourceId", 0);
            newAlarm["videoSourceName"] = requestData.value("videoSourceName", "");
            newAlarm["alarmType"] = requestData.value("alarmType", "");
            newAlarm["imageUrl"] = requestData.value("imageUrl", "");
            
            // 读取远程上报地址配置，如果配置了则设置reportUrl和reportStatus
            std::string remoteUrl = getRemoteAlarmUrl();
            if (!remoteUrl.empty()) {
                newAlarm["reportUrl"] = remoteUrl;
                newAlarm["reportStatus"] = "上报中"; // 初始状态为"上报中"
            } else {
                newAlarm["reportUrl"] = requestData.value("reportUrl", "");
                newAlarm["reportStatus"] = "未上报"; // 未配置上报地址
            }
            
            newAlarm["status"] = requestData.value("status", "pending");
            newAlarm["description"] = requestData.value("description", "");
            
            // 获取当前时间
            auto now = std::chrono::system_clock::now();
            std::time_t now_c = std::chrono::system_clock::to_time_t(now);
            std::tm* now_tm = std::localtime(&now_c);
            char timeBuffer[32];
            std::strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H:%M:%S", now_tm);
            newAlarm["timestamp"] = std::string(timeBuffer);
            
            alarmsJson.push_back(newAlarm);
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/alarms.json");
            fileOut << alarmsJson.dump(4);
            fileOut.close();
            
            // 异步上报到远程地址（如果配置了）
            reportAlarmToRemote(newAlarm);
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Alarm added successfully";
            responseJson["alarm"] = newAlarm;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 删除告警
    svr.Delete("/api/alarms/:id", [](const Request &req, Response &res)
    {
        try {
            int alarmId = std::stoi(req.path_params.at("id"));
            
            // 读取现有告警
            std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
            json alarmsJson;
            
            if (fileIn.is_open()) {
                fileIn >> alarmsJson;
                fileIn.close();
            } else {
                alarmsJson = json::array();
            }
            
            // 删除告警
            json newAlarmsJson = json::array();
            bool found = false;
            for (const auto& alarm : alarmsJson) {
                if (alarm["id"] == alarmId) {
                    found = true;
                } else {
                    newAlarmsJson.push_back(alarm);
                }
            }
            
            if (!found) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Alarm not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/alarms.json");
            fileOut << newAlarmsJson.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Alarm deleted successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 批量删除告警
    svr.Post("/api/alarms/batch-delete", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            json idsToDelete = requestData["ids"];
            
            // 读取现有告警
            std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
            json alarmsJson;
            
            if (fileIn.is_open()) {
                fileIn >> alarmsJson;
                fileIn.close();
            } else {
                alarmsJson = json::array();
            }
            
            // 删除指定ID的告警
            json newAlarmsJson = json::array();
            for (const auto& alarm : alarmsJson) {
                bool shouldDelete = false;
                for (const auto& id : idsToDelete) {
                    if (alarm["id"] == id) {
                        shouldDelete = true;
                        break;
                    }
                }
                if (!shouldDelete) {
                    newAlarmsJson.push_back(alarm);
                }
            }
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/alarms.json");
            fileOut << newAlarmsJson.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Alarms deleted successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 手动触发告警上报到远程地址
    svr.Post("/api/alarms/:id/report", [](const Request &req, Response &res)
    {
        try {
            int alarmId = std::stoi(req.path_params.at("id"));
            
            // 读取现有告警
            std::ifstream fileIn("/data/lintech/celectronicfence/alarms.json");
            json alarmsJson;
            
            if (fileIn.is_open()) {
                fileIn >> alarmsJson;
                fileIn.close();
            } else {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Alarms file not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 查找指定的告警
            json targetAlarm;
            bool found = false;
            for (const auto& alarm : alarmsJson) {
                if (alarm["id"] == alarmId) {
                    targetAlarm = alarm;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "Alarm not found";
                res.status = 404;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 触发远程上报
            reportAlarmToRemote(targetAlarm);
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Alarm report triggered";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 上传告警图片
    svr.Post("/api/alarms/upload", [](const Request &req, Response &res)
    {
        try {
            auto file = req.get_file_value("image");
            if (file.content.empty()) {
                json errorJson;
                errorJson["status"] = "error";
                errorJson["message"] = "No image file uploaded";
                res.status = 400;
                res.set_content(errorJson.dump(), "application/json");
                return;
            }
            
            // 生成文件名
            auto now = std::chrono::system_clock::now();
            std::time_t now_c = std::chrono::system_clock::to_time_t(now);
            std::tm* now_tm = std::localtime(&now_c);
            char timeBuffer[32];
            std::strftime(timeBuffer, sizeof(timeBuffer), "%Y%m%d_%H%M%S", now_tm);
            
            std::string filename = "alarm_" + std::string(timeBuffer) + "_" + std::to_string(rand() % 10000) + ".jpg";
            std::string filepath = "/data/lintech/celectronicfence/static/upload/alarm/" + filename;
            
            // 保存文件
            std::ofstream ofs(filepath, std::ios::binary);
            ofs << file.content;
            ofs.close();
            
            std::string imageUrl = "/upload/alarm/" + filename;
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["imageUrl"] = imageUrl;
            responseJson["filename"] = filename;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 参数配置API
    
    // 获取所有参数
    svr.Get("/api/params", [](const Request &req, Response &res)
    {
        try {
            std::ifstream file("/data/lintech/celectronicfence/params.json");
            json paramsJson;
            
            if (file.is_open()) {
                file >> paramsJson;
                file.close();
            } else {
                paramsJson = json::object();
            }
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["params"] = paramsJson;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 保存所有参数
    svr.Post("/api/params", [](const Request &req, Response &res)
    {
        try {
            json requestData = json::parse(req.body);
            json params = requestData["params"];
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/params.json");
            fileOut << params.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Parameters saved successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 更新单个参数
    svr.Put("/api/params/:key", [](const Request &req, Response &res)
    {
        try {
            std::string paramKey = req.path_params.at("key");
            json requestData = json::parse(req.body);
            
            // 读取现有参数
            std::ifstream fileIn("/data/lintech/celectronicfence/params.json");
            json paramsJson;
            
            if (fileIn.is_open()) {
                fileIn >> paramsJson;
                fileIn.close();
            } else {
                paramsJson = json::object();
            }
            
            // 更新参数
            paramsJson[paramKey] = requestData["value"];
            
            // 保存到文件
            std::ofstream fileOut("/data/lintech/celectronicfence/params.json");
            fileOut << paramsJson.dump(4);
            fileOut.close();
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Parameter updated successfully";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 获取单个参数
    svr.Get("/api/params/:key", [](const Request &req, Response &res)
    {
        try {
            std::string paramKey = req.path_params.at("key");
            
            // 读取参数
            std::ifstream file("/data/lintech/celectronicfence/params.json");
            json paramsJson;
            
            if (file.is_open()) {
                file >> paramsJson;
                file.close();
            } else {
                paramsJson = json::object();
            }
            
            json responseJson;
            responseJson["status"] = "success";
            
            if (paramsJson.contains(paramKey)) {
                responseJson["value"] = paramsJson[paramKey];
            } else {
                responseJson["value"] = nullptr;
            }
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    // 获取NPU和VPU使用率（通过ion堆信息）
    svr.Get("/api/system/npu", [](const Request &req, Response &res)
    {
        try {
            int npuUsage = 0;
            int vpuUsage = 0;
            
            // 获取NPU堆使用情况（不使用head，获取完整输出）
            std::string npuCommand = "cat /sys/kernel/debug/ion/cvi_npu_heap_dump/summary 2>&1";
            std::string npuResult = executeCommand(npuCommand.c_str());
            
            std::cout << "=== NPU堆信息输出 ===" << std::endl;
            std::cout << npuResult << std::endl;
            std::cout << "===================" << std::endl;
            
            // 解析NPU使用率 - 查找 "used:XXX bytes"
            size_t npuSizePos = npuResult.find("heap size:");
            size_t npuUsedPos = npuResult.find("used:");
            
            if (npuSizePos != std::string::npos && npuUsedPos != std::string::npos) {
                // 提取heap size
                size_t sizeStart = npuSizePos + 10; // "heap size:" 长度
                size_t sizeEnd = npuResult.find(" bytes", sizeStart);
                
                // 提取used
                size_t usedStart = npuUsedPos + 5; // "used:" 长度
                size_t usedEnd = npuResult.find(" bytes", usedStart);
                
                if (sizeEnd != std::string::npos && usedEnd != std::string::npos) {
                    try {
                        std::string sizeStr = npuResult.substr(sizeStart, sizeEnd - sizeStart);
                        std::string usedStr = npuResult.substr(usedStart, usedEnd - usedStart);
                        
                        long long heapSize = std::stoll(sizeStr);
                        long long heapUsed = std::stoll(usedStr);
                        
                        if (heapSize > 0) {
                            npuUsage = (int)((heapUsed * 100) / heapSize);
                        }
                        
                        std::cout << "NPU堆大小: " << heapSize << " bytes, 已使用: " << heapUsed << " bytes" << std::endl;
                        std::cout << "NPU使用率: " << npuUsage << "%" << std::endl;
                    } catch (const std::exception& e) {
                        std::cout << "NPU数据解析失败: " << e.what() << std::endl;
                    }
                }
            }
            
            // 获取VPU堆使用情况（不使用head，获取完整输出）
            std::string vpuCommand = "cat /sys/kernel/debug/ion/cvi_vpp_heap_dump/summary 2>&1";
            std::string vpuResult = executeCommand(vpuCommand.c_str());
            
            std::cout << "=== VPU堆信息输出 ===" << std::endl;
            std::cout << vpuResult << std::endl;
            std::cout << "===================" << std::endl;
            
            // 解析VPU使用率 - 查找 "used:XXX bytes"
            size_t vpuSizePos = vpuResult.find("heap size:");
            size_t vpuUsedPos = vpuResult.find("used:");
            
            if (vpuSizePos != std::string::npos && vpuUsedPos != std::string::npos) {
                // 提取heap size
                size_t sizeStart = vpuSizePos + 10;
                size_t sizeEnd = vpuResult.find(" bytes", sizeStart);
                
                // 提取used
                size_t usedStart = vpuUsedPos + 5;
                size_t usedEnd = vpuResult.find(" bytes", usedStart);
                
                if (sizeEnd != std::string::npos && usedEnd != std::string::npos) {
                    try {
                        std::string sizeStr = vpuResult.substr(sizeStart, sizeEnd - sizeStart);
                        std::string usedStr = vpuResult.substr(usedStart, usedEnd - usedStart);
                        
                        long long heapSize = std::stoll(sizeStr);
                        long long heapUsed = std::stoll(usedStr);
                        
                        if (heapSize > 0) {
                            vpuUsage = (int)((heapUsed * 100) / heapSize);
                        }
                        
                        std::cout << "VPU堆大小: " << heapSize << " bytes, 已使用: " << heapUsed << " bytes" << std::endl;
                        std::cout << "VPU使用率: " << vpuUsage << "%" << std::endl;
                    } catch (const std::exception& e) {
                        std::cout << "VPU数据解析失败: " << e.what() << std::endl;
                    }
                }
            }
            
            json responseJson;
            responseJson["status"] = "success";
            responseJson["npuUsage"] = npuUsage;
            responseJson["vpuUsage"] = vpuUsage;
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
        } catch (const std::exception& e) {
            std::cout << "获取NPU/VPU使用率异常: " << e.what() << std::endl;
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            errorJson["npuUsage"] = 0;
            errorJson["vpuUsage"] = 0;
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    // 重启设备API
    svr.Post("/api/reboot", [](const Request &req, Response &res)
    {
        try {
            json responseJson;
            responseJson["status"] = "success";
            responseJson["message"] = "Device is rebooting...";
            
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(responseJson.dump(), "application/json");
            
            // 在后台线程中执行重启命令
            std::thread rebootThread([]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::system("sudo reboot");
            });
            rebootThread.detach();
        } catch (const std::exception& e) {
            json errorJson;
            errorJson["status"] = "error";
            errorJson["message"] = e.what();
            res.status = 500;
            res.set_content(errorJson.dump(), "application/json");
        }
    });
    
    //----------------------------------------------------------------------------------------------------------------------------
    route(svr);
    route_polygon(svr);

    // 加载 JSON 文件到全局变量
    json data;
    JsonFile2::loadJsonData(data);
    svr.listen("0.0.0.0", 8088);

    return 0;
}
