#pragma once

#include <string>

#include "platform.h"

namespace sniper {

class HostPlatform : public Platform {
 public:
  explicit HostPlatform(std::string state_path) : state_path_(std::move(state_path)) {}
  bool http(const HttpRequest& req, const HeadFn& on_head, const DataFn& on_data, std::string& error) override;
  double now() override;
  void sleep(double seconds) override;
  void log(const std::string& line) override;
  bool load_state(std::string& out) override;
  bool save_state(const std::string& data) override;

 private:
  std::string state_path_;
};

}
