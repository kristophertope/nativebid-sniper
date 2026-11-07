#include "pages.h"

#include <cctype>
#include <cstdlib>

namespace sniper {

#define NUMBER "%s%?$%s<span class=\"NumberPart\">%n</span>"

LotPage::LotPage() {
  rule(R_MESSAGE, {"contextMessageDictionary[\"%w\"]%s=%s\"%q\""}, true);
  rule(R_LISTING_ID, {"id=\"ListingID\"%*>value=\"%d\"", "data-listingid=\"%d\""});
  rule(R_TYPE, {"id=\"ListingType\"%*>value=\"%w\""});
  rule(R_TITLE, {"<h3 class=\"detail__title\">%^<<"});
  rule(R_STATUS, {"detail__status-label%*>>%s<span class=\"label%*\"\">%^<<"});
  rule(R_PRICE, {"awe-rt-CurrentPrice\"%*>>" NUMBER, "Winning Bid:" NUMBER});
  rule(R_MINIMUM, {"Bidding_Listing_MinPrice%*\"\"%*>>" NUMBER});
  rule(R_BIDDERMAX, {"detail__biddermax%^\"\""});
  rule(R_MAX_BID, {"id=\"MaxBid\"%*>>" NUMBER});
  rule(R_BIDS, {">%s%d%s</span>%bBid(s)", ">%s%d%bBid(s)"});
  needed_ = {R_LISTING_ID, R_TYPE, R_TITLE, R_STATUS, R_PRICE, R_MINIMUM, R_BIDDERMAX, R_MAX_BID, R_BIDS};
}

void LotPage::hit(int id, const Match& m, size_t at, const std::string& buf) {
  if (id == R_MESSAGE) {
    messages[m.caps[0]] = js_unescape(m.caps[1]);
  } else if (id == R_BIDDERMAX) {
    size_t q = buf.rfind('"', m.start);
    std::string before = q == std::string::npos ? std::string() : buf.substr(q + 1, m.start - q - 1);
    found_[id] = before + "detail__biddermax" + m.caps[0];
  } else {
    Scanner::hit(id, m, at, buf);
  }
}

Lot LotPage::lot(const std::string& url) const {
  Lot l;
  l.url = url;
  l.listing_id = get(R_LISTING_ID);
  if (has(R_TYPE)) l.type = get(R_TYPE);
  l.title = strip_tags(get(R_TITLE));
  l.status = strip_tags(get(R_STATUS));
  l.price = money(get(R_PRICE));
  l.minimum = money(get(R_MINIMUM));
  bool shown = has(R_BIDDERMAX) && !contains(get(R_BIDDERMAX), "awe-hidden");
  double mine = shown ? money(get(R_MAX_BID)) : NAN;
  l.my_max = mine == 0 ? NAN : mine;
  if (has(R_BIDS)) l.bids = std::atoi(get(R_BIDS).c_str());
  return l;
}

ConfirmPage::ConfirmPage() {
  rule(R_SIGNED_IN, {"/Account/Logoff"});
  rule(R_FORM, {"<form%*>action=\"%^\"\""}, true);
  rule(R_FORM_END, {"</form>"}, true);
  rule(R_INPUT, {"<input%^>>"}, true);
  rule(R_TOTAL, {"including%*<up to%s$%n"});
  rule(R_ERROR, {"class=\"%*\"alert%*\"\"%*>>%c</div>", "class=\"%*\"validation-summary-errors%*\"\"%*>>%c</div>"});
}

bool ConfirmPage::done() const {
  return signed_out || (has(R_SIGNED_IN) && ended_ && has(R_TOTAL));
}

bool ConfirmPage::inside(size_t at) const {
  return started_ && at > start_ && (!ended_ || at < end_);
}

void ConfirmPage::hit(int id, const Match& m, size_t at, const std::string& buf) {
  if (id == R_FORM) {
    const std::string& target = m.caps[0];
    if (contains(target, "/Account/LogOn")) signed_out = true;
    if (!started_ && contains(target, "/Listing/Action")) {
      started_ = true;
      start_ = at;
      action = target;
    }
  } else if (id == R_FORM_END) {
    if (inside(at) && !ended_) {
      ended_ = true;
      end_ = at;
    }
  } else if (id == R_INPUT) {
    if (inside(at)) add_input(m.caps[0]);
  } else if (id == R_TOTAL) {
    found_[id] = "$" + m.caps[0];
  } else {
    Scanner::hit(id, m, at, buf);
  }
}

void ConfirmPage::add_input(const std::string& s) {
  std::map<std::string, std::string> a;
  size_t i = 0, n = s.size();
  while (i < n) {
    while (i < n && (is_space(s[i]) || s[i] == '/')) i++;
    size_t k = i;
    while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '-')) i++;
    if (i == k) {
      i++;
      continue;
    }
    std::string name = lower(s.substr(k, i - k)), value;
    size_t j = i;
    while (j < n && is_space(s[j])) j++;
    if (j < n && s[j] == '=') {
      j++;
      while (j < n && is_space(s[j])) j++;
      if (j < n && (s[j] == '"' || s[j] == '\'')) {
        char q = s[j++];
        size_t e = s.find(q, j);
        if (e == std::string::npos) e = n;
        value = s.substr(j, e - j);
        j = e + 1;
      } else {
        size_t e = j;
        while (e < n && !is_space(s[e])) e++;
        value = s.substr(j, e - j);
        j = e;
      }
      i = j;
    }
    a[name] = html_unescape(value);
  }
  std::string kind = lower(a.count("type") ? a["type"] : "text");
  if (kind.empty()) kind = "text";
  if (!a.count("name") || a["name"].empty()) return;
  if (kind == "submit" || kind == "button" || kind == "image" || kind == "file" || kind == "reset") return;
  if ((kind == "checkbox" || kind == "radio") && !a.count("checked")) return;
  fields.emplace_back(a["name"], a.count("value") ? a["value"] : "");
}

LoginPage::LoginPage() {
  rule(R_ERROR, {"validation-summary-errors%*>>%.<li%*>>%c</li>"});
  needed_ = {R_ERROR};
}

}
