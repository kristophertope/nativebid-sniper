#pragma once

#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "nativebid.h"
#include "telegram.h"

namespace sniper {

constexpr double LEAD = 60;
constexpr double BID_WINDOW = 15;

struct Snipe {
  long id = 0;
  std::string url;
  std::string listing_id;
  std::string title;
  std::string state = "pending";
  std::string total;
  double max = 0;
  double ends = 0;
  double check_at = 0;
};

class Bot {
 public:
  Bot(Platform& p, NativeBid& nb, Telegram& tg);
  bool start();
  void run();
  Result tick();
  void handle(const std::string& text, const std::string& reply_to = "");
  void press(const std::string& query_id, const std::string& data, long long message_id);
  Result flush();
  void say(const std::string& text, const Keyboard& kb = Keyboard::none(), long long edit = 0);
  std::vector<Snipe> snipes;

 private:
  struct Outgoing {
    std::string text;
    Keyboard kb;
    long long edit;
  };
  void load();
  void save();
  Snipe* get(long id);
  void drop(long id);
  size_t pending() const;
  double next_event();
  Result refresh();
  void retry_later(long id, const Result& r);
  Result bid_mode(long id);
  void missed(long id);
  Result poll(int wait);
  void report(const Result& r);
  bool answer_prompt(const std::string& text, const std::string& reply_to);
  Result find(const std::string& ref, long& id);
  Result by_lot(const std::string& ref, long& id);
  Result confirm(const Lot& lot, double amount, std::string& total);
  Result add(const std::string& ref, double max_bid, long long edit = 0);
  Result card(long id, const Lot* lot = nullptr, double left = 0, const std::string& heading = "", double old = NAN,
              long long edit = 0);
  Result show(long long edit = 0);
  Result cancel(const std::string& ref, long long edit = 0, bool quiet = false);
  Result info(const std::string& ref, long long edit = 0);

  Platform& p_;
  NativeBid& nb_;
  Telegram& tg_;
  std::deque<Outgoing> outbox_;
  long long offset_ = -1;
  long next_id_ = 1;
};

std::string plain(const std::string& html);

}
