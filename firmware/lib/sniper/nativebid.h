#pragma once

#include <map>
#include <string>

#include "pages.h"
#include "platform.h"
#include "web.h"

namespace sniper {

class NativeBid {
 public:
  NativeBid(Platform& p, std::string user, std::string password)
      : p_(p), web_(p, true), user_(std::move(user)), password_(std::move(password)) {}
  virtual ~NativeBid() = default;
  virtual Result lot(const std::string& ref, Lot& out);
  virtual Result time_remaining(const std::string& listing_id, double& left);
  virtual Result login();
  virtual Result watch(const std::string& listing_id);
  virtual Result confirm(const Lot& lot, double amount, std::string& action, std::string& params, std::string& total);
  virtual Result place(const std::string& action, const std::string& params, bool& ok, std::string& message);
  std::map<std::string, std::string> messages;

 protected:
  Platform& p_;

 private:
  Web web_;
  std::string user_;
  std::string password_;
};

enum class Wait { Fire, Ended, Later };

Result wait_for(Platform& p, NativeBid& nb, const std::string& listing_id, double lead, double give_up, Wait& out);
Result fire(Platform& p, NativeBid& nb, const std::string& url, double max_bid, bool& ok, std::string& message);

template <class F>
Result retrying(Platform& p, F fn, int attempts = 5, double delay = 2) {
  for (int i = 0;; i++) {
    Result r = fn();
    if (r.err != Err::Network || i == attempts - 1) return r;
    p.log("Network error (" + r.msg + "); retrying");
    p.sleep(delay);
  }
}

}
