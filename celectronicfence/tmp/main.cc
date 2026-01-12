#include "httplib.hpp"
#include "json.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;
using namespace httplib;

// ----------------------------------------------------------------------------

int ltw_linux_popen(const char *cmd, char *msg, int *len, int size) {
  *len = 0;
  FILE *fp = popen(cmd, "r");
  if (fp == NULL)
    return -1;
  while (fgets(msg + *len, size - *len, fp) != NULL)
    *len += strlen(msg + *len);
  int status = pclose(fp);
  if (status < 0)
    return -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    return -WTERMSIG(status);

  return 0;
}

// ----------------------------------------------------------------------------

void route_polygon(Server &svr) {}

// ----------------------------------------------------------------------------

struct rect {
  double x, y, w, h;
};
std::vector<struct rect> rect;

const std::string streamServerRoiPath = "yolov8_classthresh_roi_example.json";

void killStreamServer() {}

void startStreamServer() {}

void updateStreamServer(int id) {
  std::string json_tmp;

  std::fstream f(streamServerRoiPath);
  if (f.good()) {
    try {
      json conf = json::parse(f);
      int x = rect[id].x;
      int y = rect[id].y;
      int w = rect[id].w;
      int h = rect[id].h;

      if (x < 0)
        x = 0;
      if (y < 0)
        y = 0;
      if (x > 1920 - 50)
        x = 1920 - 50;
      if (y > 1080 - 50)
        y = 1080 - 50;
      if (x + w > 1920) {
        w = 1920 - x;
      }
      if (y + h > 1080) {
        h = 1080 - y;
      }

      conf["configure"]["roi"]["left"] = x;
      conf["configure"]["roi"]["top"] = y;
      conf["configure"]["roi"]["width"] = w;
      conf["configure"]["roi"]["height"] = h;

      json_tmp.append(conf.dump(4));

      std::printf("updateSteamServer: x %d y %d w %d h %d\n", x, y, w, h);
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
    }
  }
  f.close();

  if (json_tmp.size() > 0) {
    std::ofstream of(streamServerRoiPath, std::fstream::trunc);
    of << json_tmp << std::endl;
    of.close();
  }
}

void route_rect(Server &svr) {
  std::ifstream f("conf.json");
  if (f.good()) {
    try {
      json conf = json::parse(f);
      for (auto it : conf.at("rect")) {
        rect.push_back({it.at("x"), it.at("y"), it.at("w"), it.at("h")});
      }
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
      rect.clear();
      rect.push_back({0, 0, 1920, 1080});
      rect.push_back({0, 0, 1920, 1080});
    }
  } else {
    rect.clear();
    rect.push_back({0, 0, 1920, 1080});
    rect.push_back({0, 0, 1920, 1080});
  }
  f.close();

  updateStreamServer(0);
  killStreamServer();
  startStreamServer();

  svr.Get("/stream/rect/:id", [&](const Request &req, Response &res) {
    auto id = atoi(req.path_params.at("id").c_str());

    if (id < 0 || id >= rect.size()) {
      res.status = 400;
      return;
    }

    json resj = {
        {"x", rect[id].x},
        {"y", rect[id].y},
        {"w", rect[id].w},
        {"h", rect[id].h},
    };
    res.set_content(resj.dump(), "application/json");
  });

  svr.Post("/stream/rect/:id", [&](const Request &req, Response &res) {
    auto id = atoi(req.path_params.at("id").c_str());

    if (id < 0 || id >= rect.size()) {
      res.status = 400;
      return;
    }

    json reqj = json::parse(req.body);
    rect[id] = {reqj.at("x"), reqj.at("y"), reqj.at("w"), reqj.at("h")};

    updateStreamServer(id);
    killStreamServer();
    startStreamServer();
  });
}

// ----------------------------------------------------------------------------

std::string time_local() {
  auto p = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(p);

  std::stringstream ss;
  ss << std::put_time(std::localtime(&t), "%d/%b/%Y:%H:%M:%S %z");
  return ss.str();
}

std::string log(const Request &req, const Response &res) {
  auto remote_user = "-"; // TODO:
  auto request = req.method + " " + req.path + " " + req.version;
  auto body_bytes_sent = res.get_header_value("Content-Length");
  auto http_referer = "-"; // TODO:
  auto http_user_agent = req.get_header_value("User-Agent", "-");

  // NOTE: From NGINX defualt access log format
  // log_format combined '$remote_addr - $remote_user [$time_local] '
  //                     '"$request" $status $body_bytes_sent '
  //                     '"$http_referer" "$http_user_agent"';

  // return req.remote_addr + " - " + remote_user + " [" + time_local() + "] \""
  // +
  //        request + "\" " + std::to_string(res.status) + " " + body_bytes_sent
  //        + " \"" + http_referer + "\" \"" + http_user_agent + "\"";

  return "[" + time_local() + "] " + req.remote_addr + " " +
         std::to_string(res.status) + " " + req.method + " " + req.path;
}

// ----------------------------------------------------------------------------

void route(Server &svr) {
  svr.Get("/hi", [](const Request &req, Response &res) {
    res.set_content("Hello World!", "text/plain");
  });

  svr.Post("/testjson", [](const Request &req, Response &res) {
    json reqj = json::parse(req.body, nullptr, false, false);

    json resj = {{"message", "Hello World!"}};
    res.set_content(resj.dump(), "application/json");
  });

  svr.Post("/func/system", [](const Request &req, Response &res) {
    int ret = system(req.body.c_str()); // ? 失败不返回0
    if (ret != 0) {
      res.status = 400;
    }
  });

  svr.Post("/func/popen", [](const Request &req, Response &res) {
    char msg[10000];
    int len, ret;
    ret = ltw_linux_popen(req.body.c_str(), msg, &len, sizeof(msg));

    res.set_content(msg, "text/plain");
  });
}

int main() {
  Server svr;

  auto ret = svr.set_mount_point("/", "./static");
  if (!ret) {
    printf("The ./static directory doesn't exist...\n");
  }

  svr.set_logger([](const Request &req, const Response &res) {
    std::cout << log(req, res) << std::endl;
  });

  route(svr);
  route_rect(svr);
  route_polygon(svr);

  svr.listen("0.0.0.0", 8088);
}