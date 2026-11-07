#include "nativebid.h"

#include <cctype>
#include <cmath>

#include "json.h"
#include "text.h"

namespace sniper {

static const char* BASE = "https://www.nativebid.com";

static bool success(const std::string& s) {
  std::string l = lower(s);
  return l == "ok" || l == "success" || l == "actionsuccess";
}

Result NativeBid::lot(const std::string& raw, Lot& out) {
  std::string ref = trim(raw);
  std::vector<std::string> candidates;
  if (starts_with(ref, "http") || starts_with(ref, "/")) {
    candidates.push_back(ref);
  } else {
    candidates.push_back("/Listing/Details/" + ref);
    candidates.push_back("/Event/LotDetails/" + ref);
  }
  for (const auto& path : candidates) {
    LotPage page;
    Reply r = web_.scan(url_join(BASE, path), page);
    if (!r.ok) return Result::network(r.error);
    if (r.status == 200 && page.has(LotPage::R_LISTING_ID)) {
      for (const auto& m : page.messages) messages[m.first] = m.second;
      out = page.lot(r.url);
      return {};
    }
  }
  return Result::site("Couldn\xE2\x80\x99t find a NativeBid lot for '" + ref + "'");
}

Result NativeBid::time_remaining(const std::string& listing_id, double& left) {
  std::string body;
  Reply r = web_.text("POST", std::string(BASE) + "/RealTime/GetTimeRemaining",
                      "{\"listingId\": " + listing_id + "}", {{"Content-Type", "application/json"}}, body, SMALL, 10);
  if (!r.ok) return Result::network(r.error);
  if (r.status != 200) return Result::site("GetTimeRemaining returned HTTP " + std::to_string(r.status));
  JsonDocument doc;
  if (deserializeJson(doc, body)) return Result::site("GetTimeRemaining sent something unexpected");
  std::string error = doc["error"] | "";
  if (!error.empty()) return Result::site("GetTimeRemaining error: " + error);
  JsonVariant seconds = doc["secondsRemaining"];
  if (!seconds.is<double>()) return Result::site("GetTimeRemaining didn\xE2\x80\x99t include the time left");
  left = seconds.as<double>();
  return {};
}

Result NativeBid::login() {
  if (user_.empty() || password_.empty()) return Result::site("Set NB_USER and NB_PASS first.");
  std::string form = form_encode({{"username", user_}, {"password", password_}, {"rememberMe", "false"}, {"returnUrl", ""}});
  LoginPage page;
  bool failed = false;
  Reply r = web_.request(
      "POST", std::string(BASE) + "/Account/LogOn", form, {{"Content-Type", "application/x-www-form-urlencoded"}}, 20,
      false,
      [&](const Reply& head) {
        bool moved = head.status >= 300 && head.status < 400 && !contains(head.location, "/Account/LogOn");
        failed = !moved;
        return failed;
      },
      [&](const char* d, size_t n) {
        page.feed(d, n);
        return !page.done();
      });
  if (!r.ok) return Result::network(r.error);
  if (failed) {
    std::string err = page.get(LoginPage::R_ERROR);
    return Result::site("Sign-in failed" + (err.empty() ? std::string(".") : ": " + strip_tags(err)));
  }
  p_.log("Signed in as " + user_);
  return {};
}

Result NativeBid::confirm(const Lot& lot, double amount, std::string& action, std::string& params, std::string& total) {
  char amt[32];
  std::snprintf(amt, sizeof amt, "%.2f", amount);
  std::string query = form_encode({{"ListingID", lot.listing_id}, {"ListingType", lot.type}, {"BidAmount", amt}});
  ConfirmPage page;
  Reply r = web_.scan(std::string(BASE) + "/Listing/ConfirmAction?" + query, page);
  if (!r.ok) return Result::network(r.error);
  if (r.status != 200) return Result::site("ConfirmAction returned HTTP " + std::to_string(r.status));
  if (page.signed_out) return Result::not_signed_in();
  if (page.action.empty()) {
    std::string err = page.get(ConfirmPage::R_ERROR);
    return Result::site("NativeBid didn\xE2\x80\x99t accept that bid" +
                        (err.empty() ? std::string(".") : ": " + strip_tags(err)));
  }
  std::string bid, listing;
  for (const auto& f : page.fields) {
    if (f.first == "BidAmount" && bid.empty()) bid = f.second;
    if (f.first == "ListingID" && listing.empty()) listing = f.second;
  }
  if (money(bid) != cents(amount) || listing != lot.listing_id) {
    return Result::site("NativeBid\xE2\x80\x99s confirmation didn\xE2\x80\x99t match this bid, so nothing was placed.");
  }
  action = page.action;
  params = form_encode(page.fields);
  total = page.get(ConfirmPage::R_TOTAL);
  return {};
}

Result NativeBid::place(const std::string& action, const std::string& params, bool& ok, std::string& message) {
  std::string body;
  Reply r = web_.text("GET", url_join(BASE, action + "?" + params), "",
                      {{"X-Requested-With", "XMLHttpRequest"}, {"Accept", "application/json"}}, body);
  if (!r.ok) return Result::network(r.error);
  body = trim(body);
  std::string result = body;
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    if (doc.is<JsonObject>()) {
      result.clear();
      for (const char* key : {"ReasonCode", "Status", "Message"}) {
        JsonVariant v = doc[key];
        if (v.isNull() || v == false || v == 0 || v == "") continue;
        if (v.is<const char*>()) {
          result = v.as<const char*>();
        } else {
          serializeJson(v, result);
        }
        break;
      }
      if (result.empty()) serializeJson(doc, result);
    } else if (doc.is<const char*>()) {
      result = doc.as<const char*>();
    } else if (!doc.isNull()) {
      result.clear();
      serializeJson(doc, result);
    }
  }
  if (r.status == 200 && success(result)) {
    ok = true;
    message = result;
    return {};
  }
  ok = false;
  auto known = messages.find(result);
  if (known != messages.end()) {
    message = known->second;
  } else if (!result.empty() && result.find_first_of("<{") == std::string::npos) {
    message = result;
  } else {
    message = "NativeBid didn\xE2\x80\x99t accept that bid (HTTP " + std::to_string(r.status) + ").";
  }
  return {};
}

Result wait_for(Platform& p, NativeBid& nb, const std::string& listing_id, double lead, double give_up, Wait& out) {
  bool signed_in_again = false;
  for (;;) {
    double left = 0;
    Result r = retrying(p, [&] { return nb.time_remaining(listing_id, left); });
    if (!r.ok()) return r;
    if (left <= 0) {
      out = Wait::Ended;
      return {};
    }
    double until_fire = left - lead;
    if (give_up >= 0 && until_fire > give_up) {
      out = Wait::Later;
      return {};
    }
    if (until_fire <= 1.5) {
      p.sleep(until_fire > 0 ? until_fire : 0);
      out = Wait::Fire;
      return {};
    }
    if (!signed_in_again && until_fire <= 360) {
      r = retrying(p, [&] { return nb.login(); });
      if (!r.ok()) return r;
      signed_in_again = true;
    }
    double pause = std::min(300.0, std::max(0.5, until_fire / 2));
    if (pause >= 5) p.log("Ends in " + duration(left) + "; bidding in " + duration(until_fire));
    p.sleep(pause);
  }
}

Result fire(Platform& p, NativeBid& nb, const std::string& url, double max_bid, bool& ok, std::string& message) {
  for (int attempt = 0; attempt < 3; attempt++) {
    Lot lot;
    Result r = retrying(p, [&] { return nb.lot(url, lot); });
    if (r.err == Err::Network) {
      p.log("Network error while bidding (" + r.msg + "); checking and retrying");
      p.sleep(1);
      continue;
    }
    if (!r.ok()) return r;
    if (!std::isnan(lot.my_max) && lot.my_max >= max_bid) {
      ok = true;
      message = "Your max is already " + fmt(lot.my_max);
      return {};
    }
    if (!std::isnan(lot.minimum) && lot.minimum > max_bid) {
      ok = false;
      message = "Next bid is " + fmt(lot.minimum) + ", above your max of " + fmt(max_bid);
      return {};
    }
    std::string action, params, total;
    r = nb.confirm(lot, max_bid, action, params, total);
    if (r.err == Err::NotSignedIn) {
      r = nb.login();
      if (r.ok()) r = nb.confirm(lot, max_bid, action, params, total);
    }
    if (r.err == Err::Network) {
      p.log("Network error while bidding (" + r.msg + "); checking and retrying");
      p.sleep(1);
      continue;
    }
    if (!r.ok()) return r;
    p.log("Placing max bid " + fmt(max_bid) + " (up to " + (total.empty() ? "?" : total) + " with fees)");
    r = nb.place(action, params, ok, message);
    if (r.err == Err::Network) {
      p.log("Network error while bidding (" + r.msg + "); checking and retrying");
      p.sleep(1);
      continue;
    }
    return r;
  }
  ok = false;
  message = "Couldn\xE2\x80\x99t reach NativeBid; the bid may not have gone through.";
  return {};
}

}
