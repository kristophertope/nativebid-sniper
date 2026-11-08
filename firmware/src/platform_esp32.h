#pragma once

#include <WiFiClientSecure.h>

#include <memory>
#include <string>

#include "platform.h"

namespace sniper {

class EspPlatform : public Platform {
 public:
  bool http(const HttpRequest& req, const HeadFn& on_head, const DataFn& on_data, std::string& error) override;
  double now() override;
  void sleep(double seconds) override;
  void log(const std::string& line) override;
  bool load_state(std::string& out) override;
  bool save_state(const std::string& data) override;
  void drop_connection();

 private:
  bool exchange(const HttpRequest& req, const std::string& host, const std::string& path, const HeadFn& on_head,
                const DataFn& on_data, std::string& error, bool& retry);
  std::unique_ptr<WiFiClientSecure> client_;
  std::string host_;
};

}
