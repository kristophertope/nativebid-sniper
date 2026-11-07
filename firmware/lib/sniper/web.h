#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "platform.h"

namespace sniper {

class Scanner;

constexpr size_t SMALL = 8192;

struct Reply {
  bool ok = false;
  std::string error;
  int status = 0;
  std::string url;
  std::string location;
};

class Web {
 public:
  Web(Platform& p, bool cookies) : p_(p), cookies_(cookies) {}
  Reply request(std::string method, std::string url, std::string body, const std::vector<Header>& headers,
                int timeout, bool follow, const std::function<bool(const Reply&)>& want_body, const DataFn& sink);
  Reply scan(const std::string& url, Scanner& page, int timeout = 20);
  Reply text(const std::string& method, const std::string& url, const std::string& body,
             const std::vector<Header>& headers, std::string& out, size_t limit = SMALL, int timeout = 20);

 private:
  void store_cookie(const std::string& header);
  std::string cookie_header() const;
  Platform& p_;
  bool cookies_;
  std::map<std::string, std::string> jar_;
};

}
