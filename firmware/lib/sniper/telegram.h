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
  bool force_reply = false;
  std::string placeholder;
  using Rows = std::vector<std::vector<std::pair<std::string, std::string>>>;
  static Keyboard none() { return Keyboard(); }
  static Keyboard of(Rows rows) {
    Keyboard k;
    k.set = true;
    k.rows = std::move(rows);
    return k;
  }
  static Keyboard empty() { return of({}); }
  static Keyboard reply(const std::string& text) {
    Keyboard k;
    k.force_reply = true;
    k.placeholder = text;
    return k;
  }
};

class Telegram {
 public:
  Telegram(Platform& p, const std::string& token, long long chat_id)
      : p_(p), web_(p, false), url_("https://api.telegram.org/bot" + token + "/"), chat_id_(chat_id) {}
  virtual ~Telegram() = default;
  long long chat_id() const { return chat_id_; }
  void release() { p_.release("api.telegram.org"); }
  virtual Result call(const char* method, const JsonDocument& params, JsonDocument& reply, int wait = 15);
  virtual Result send(const std::string& text, const Keyboard& kb, long long edit = 0);
  virtual Result send_plain(const std::string& text);
  virtual Result answer(const std::string& query_id, const std::string& text = "", bool alert = false);
  virtual Result updates(long long offset, int wait, int limit, JsonDocument& reply);

 protected:
  Platform& p_;

 private:
  Web web_;
  std::string url_;
  long long chat_id_;
};

}
