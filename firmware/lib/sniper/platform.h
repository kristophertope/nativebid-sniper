#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sniper {

struct Header {
  std::string name;
  std::string value;
};

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::string body;
  std::vector<Header> headers;
  int timeout_s = 20;
};

struct HttpHead {
  int status = 0;
  std::vector<Header> headers;
};

using HeadFn = std::function<bool(const HttpHead&)>;
using DataFn = std::function<bool(const char*, size_t)>;

class Platform {
 public:
  virtual ~Platform() = default;
  virtual bool http(const HttpRequest& req, const HeadFn& on_head, const DataFn& on_data, std::string& error) = 0;
  virtual double now() = 0;
  virtual void sleep(double seconds) = 0;
  virtual void log(const std::string& line) = 0;
  virtual bool load_state(std::string& out) = 0;
  virtual bool save_state(const std::string& data) = 0;
};

enum class Err { None, Network, Site, NotSignedIn };

struct Result {
  Err err = Err::None;
  std::string msg;
  bool ok() const { return err == Err::None; }
  static Result network(const std::string& m) { return {Err::Network, m}; }
  static Result site(const std::string& m) { return {Err::Site, m}; }
  static Result not_signed_in() { return {Err::NotSignedIn, "Not signed in"}; }
};

}
