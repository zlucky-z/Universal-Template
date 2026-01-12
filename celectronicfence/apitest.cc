#include <arpa/inet.h>
#include <iostream>
#include <netdb.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "json.hpp"

int http_get(std::string &response) {
  // 创建套接字
  int socket_desc = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_desc == -1) {
    std::cerr << "Could not create socket." << std::endl;
    return -1;
  }

  // 设定服务器地址和端口
  std::string server = "127.0.0.1";
  int port = 8088;

  // 解析服务器 IP 地址
  struct hostent *host = gethostbyname(server.c_str());
  if (host == nullptr) {
    std::cerr << "Could not resolve hostname." << std::endl;
    return -1;
  }
  struct in_addr address;
  memcpy(&address, host->h_addr_list[0], sizeof(struct in_addr));

  // 设置服务器地址结构
  struct sockaddr_in server_addr {};
  server_addr.sin_addr = address;
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  // 连接服务器
  if (connect(socket_desc, (struct sockaddr *)&server_addr,
              sizeof(server_addr)) < 0) {
    std::cerr << "Could not connect to server." << std::endl;
    return -1;
  }

  // 构建HTTP请求
  std::string request = "GET /stream/polygon/0 HTTP/1.1\r\n"
                        "Host: " +
                        server +
                        "\r\n"
                        "User-Agent: C++ HTTP Client\r\n"
                        "Connection: close\r\n\r\n";

  // 发送HTTP请求
  if (send(socket_desc, request.c_str(), request.length(), 0) < 0) {
    std::cerr << "Failed to send HTTP request." << std::endl;
    return -1;
  }

  // 接收并打印服务器响应

  char buffer[4096];
  while (true) {
    memset(buffer, 0, sizeof(buffer));
    int bytes_received = recv(socket_desc, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
      break;
    }
    response += buffer;
  }

  // std::cout << response << std::endl;

  // 关闭套接字
  close(socket_desc);

  return 0;
}

std::vector<std::pair<int, int>> getCustomPolygonPoints() {
  std::vector<std::pair<int, int>> points;

  std::string response;
  http_get(response);
  response = response.substr(response.find("\r\n\r\n") + 4);

  printf("%s\n", response.c_str());

  auto resj = nlohmann::json::parse(response);
  for (auto it : resj["points"]) {
    points.push_back(make_pair(it.at("x"), it.at("y")));
  }

  return points;
}

int main() {
  getCustomPolygonPoints();
  return 0;
}