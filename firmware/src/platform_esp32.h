#pragma once

#include <WiFiClientSecure.h>

#include <memory>
#include <string>
#include <vector>

#include "platform.h"

namespace sniper {

constexpr int ACTIVITY_LED = 2;

class EspPlatform : public Platform {
 public:
  bool http(const HttpRequest& req, const HeadFn& on_head, const DataFn& on_data, std::string& error) override;
  double now() override;
  void sleep(double seconds) override;
  void log(const std::string& line) override;
  bool load_state(std::string& out) override;
  bool save_state(const std::string& data) override;
  void release(const std::string& host) override;
  void release_all();

 private:
  struct Connection {
    std::string host;
    std::unique_ptr<WiFiClientSecure> client;
    uint32_t used = 0;
  };
  bool exchange(const HttpRequest& req, const std::string& host, const std::string& path, const HeadFn& on_head,
                const DataFn& on_data, std::string& error, bool& retry);
  Connection* open(const std::string& host, bool& reused, std::string& error);
  std::vector<Connection> connections_;
};

}
