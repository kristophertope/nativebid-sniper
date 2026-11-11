#include "pages.h"

#include <cctype>
#include <cstdlib>

namespace sniper {

#define NUMBER "%s%?$%s<span class=\"NumberPart\">%n</span>"

LotPage::LotPage() {
  static const auto message = compile_all({"contextMessageDictionary[\"%w\"]%s=%s\"%q\""});
  rule(R_MESSAGE, message, true);
  static const auto listing_id = compile_all({"id=\"ListingID\"%*>value=\"%d\"", "data-listingid=\"%d\""});
  rule(R_LISTING_ID, listing_id);
  static const auto type = compile_all({"id=\"ListingType\"%*>value=\"%w\""});
  rule(R_TYPE, type);
  static const auto title = compile_all({"<h3 class=\"detail__title\">%^<<"});
  rule(R_TITLE, title);
  static const auto status = compile_all({"detail__status-label%*>>%s<span class=\"label%*\"\">%^<<"});
  rule(R_STATUS, status);
  static const auto price = compile_all({"awe-rt-CurrentPrice\"%*>>" NUMBER, "Winning Bid:" NUMBER});
  rule(R_PRICE, price);
  static const auto minimum = compile_all({"Bidding_Listing_MinPrice%*\"\"%*>>" NUMBER});
  rule(R_MINIMUM, minimum);
  static const auto biddermax = compile_all({"detail__biddermax%^\"\""});
  rule(R_BIDDERMAX, biddermax);
  static const auto max_bid = compile_all({"id=\"MaxBid\"%*>>" NUMBER});
  rule(R_MAX_BID, max_bid);
  static const auto bids = compile_all({">%s%d%s</span>%bBid(s)", ">%s%d%bBid(s)"});
  rule(R_BIDS, bids);
  static const auto standing = compile_all({"ContextualStatus\"%*>>%s<div class=\"alert alert-%w\">%s%^<<"});
  rule(R_STANDING, standing);
  needed_ = {R_LISTING_ID, R_TYPE, R_TITLE, R_STATUS, R_PRICE, R_MINIMUM, R_BIDDERMAX, R_MAX_BID, R_BIDS};
}

void LotPage::hit(int id, const Match& m, size_t at, const std::string& buf) {
  if (id == R_MESSAGE) {
    messages[m.caps[0]] = js_unescape(m.caps[1]);
  } else if (id == R_STANDING) {
    found_[id] = m.caps[0] + " " + m.caps[1];
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
  if (has(R_STANDING)) {
    std::string s = get(R_STANDING);
    size_t space = s.find(' ');
    std::string kind = s.substr(0, space);
    l.standing = trim(html_unescape(s.substr(space + 1)));
    if (kind == "success") l.winning = 1;
    if (kind == "danger") l.winning = 0;
  }
  return l;
}

ConfirmPage::ConfirmPage() {
  static const auto signed_in = compile_all({"/Account/Logoff"});
  rule(R_SIGNED_IN, signed_in);
  static const auto form = compile_all({"<form%*>action=\"%^\"\""});
  rule(R_FORM, form, true);
  static const auto form_end = compile_all({"</form>"});
  rule(R_FORM_END, form_end, true);
  static const auto input = compile_all({"<input%^>>"});
  rule(R_INPUT, input, true);
  static const auto total = compile_all({"including%*<up to%s$%n"});
  rule(R_TOTAL, total);
  static const auto error = compile_all({"class=\"%*\"alert%*\"\"%*>>%c</div>", "class=\"%*\"validation-summary-errors%*\"\"%*>>%c</div>"});
  rule(R_ERROR, error);
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
  static const auto error = compile_all({"validation-summary-errors%*>>%.<li%*>>%c</li>"});
  rule(R_ERROR, error);
  needed_ = {R_ERROR};
}

}
