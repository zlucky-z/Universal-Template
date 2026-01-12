#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>
#include <streambuf>
#include <string>
#include "httplib.h"

bool simulatedReboot = false;

int main() {
    // 测试视频流更新后的重启逻辑
    {
        httplib::Client cli("localhost", 8088);
        std::string body = R"({
           "videoStreamUrl": "http://new.url"
       })";
        auto res = cli.Post("/update_video_stream", body, "application/json");
        if (res->status == 200 && res->body == "Configuration updated successfully.") {
            std::cout << "视频流更新请求成功发送." << std::endl;
        } else {
            std::cerr << "视频流更新请求失败." << std::endl;
            return -1;
        }

        // 检查是否输出了重启相关信息
        std::stringstream buffer;
        buffer << "线程已启动，准备重启..." << std::endl;
        buffer << "Set reboot flag to true." << std::endl;
        buffer << "已分离线程并尝试启动重启线程." << std::endl;

        std::string actualOutput = "";
        std::streambuf* coutBuffer = std::cout.rdbuf();
        std::stringbuf* tempBuffer = new std::stringbuf();
        std::cout.rdbuf(tempBuffer);

        // 模拟等待时间
        std::this_thread::sleep_for(std::chrono::seconds(4));

        actualOutput = tempBuffer->str();
        std::cout.rdbuf(coutBuffer);
        delete tempBuffer;

        // // 去除首尾空白
        // std::string expectedTrimmed = buffer.str();
        // expectedTrimmed.erase(expectedTrimmed.find_last_not_of(" \n\r\t") + 1);
        // expectedTrimmed.erase(0, expectedTrimmed.find_first_not_of(" \n\r\t"));

        // std::string actualTrimmed = actualOutput;
        // actualTrimmed.erase(actualTrimmed.find_last_not_of(" \n\r\t") + 1);
        // actualTrimmed.erase(0, actualTrimmed.find_first_not_of(" \n\r\t"));

        if (actualTrimmed == expectedTrimmed) {
            std::cout << "重启相关信息输出正确." << std::endl;
        } else {
            std::cerr << "重启相关信息输出错误." << std::endl;
            std::cerr << "预期输出：" << expectedTrimmed << std::endl;
            std::cerr << "实际输出：" << actualTrimmed << std::endl;
            return -1;
        }

        // 检查模拟重启标志是否被设置
        if (!simulatedReboot) {
            std::cout << "模拟未重启成功（测试通过）" << std::endl;
        } else {
            std::cerr << "模拟未重启失败（测试不通过）" << std::endl;
            return -1;
        }
    }

    return 0;
}