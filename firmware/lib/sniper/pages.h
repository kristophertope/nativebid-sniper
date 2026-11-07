#pragma once

#include <cmath>
#include <map>
#include <string>

#include "scanner.h"
#include "text.h"

namespace sniper {

struct Lot {
  std::string url;
  std::string listing_id;
  std::string type = "Auction";
  std::string title;
  std::string status;
  double price = NAN;
  double minimum = NAN;
  double my_max = NAN;
  int bids = -1;
};

class LotPage : public Scanner {
 public:
  enum { R_MESSAGE, R_LISTING_ID, R_TYPE, R_TITLE, R_STATUS, R_PRICE, R_MINIMUM, R_BIDDERMAX, R_MAX_BID, R_BIDS };
  LotPage();
  Lot lot(const std::string& url) const;
  std::map<std::string, std::string> messages;

 protected:
  void hit(int id, const Match& m, size_t at, const std::string& buf) override;
};

class ConfirmPage : public Scanner {
 public:
  enum { R_SIGNED_IN, R_FORM, R_FORM_END, R_INPUT, R_TOTAL, R_ERROR };
  ConfirmPage();
  bool done() const override;
  bool signed_out = false;
  std::string action;
  Fields fields;

 protected:
  void hit(int id, const Match& m, size_t at, const std::string& buf) override;

 private:
  bool inside(size_t at) const;
  void add_input(const std::string& attrs);
  bool started_ = false;
  bool ended_ = false;
  size_t start_ = 0;
  size_t end_ = 0;
};

class LoginPage : public Scanner {
 public:
  enum { R_ERROR };
  LoginPage();
};

}
