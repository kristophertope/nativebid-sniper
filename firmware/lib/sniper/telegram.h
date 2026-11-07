#pragma once

#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "platform.h"
#include "web.h"

namespace sniper {

struct Keyboard {
  bool set = false;
  std::vector<std::vector<std::pair<std::string, std::string>>> rows;
  static Keyboard none() { return {}; }
  static Keyboard empty() { return {true, {}}; }
};

class Telegram {
 public:
  Telegram(Platform& p, const std::string& token, long long chat_id)
      : p_(p), web_(p, false), url_("https://api.telegram.org/bot" + token + "/"), chat_id_(chat_id) {}
  virtual ~Telegram() = default;
  long long chat_id() const { return chat_id_; }
  virtual Result call(const char* method, const JsonDocument& params, JsonDocument& reply, int wait = 15);
  virtual Result send(const std::string& text, const Keyboard& kb, long long edit = 0);
  virtual Result send_plain(const std::string& text);
  virtual Result answer(const std::string& query_id, const std::string& text = "", bool alert = false);
  virtual Result updates(long long offset, int wait, JsonDocument& reply);

 protected:
  Platform& p_;

 private:
  Web web_;
  std::string url_;
  long long chat_id_;
};

}
