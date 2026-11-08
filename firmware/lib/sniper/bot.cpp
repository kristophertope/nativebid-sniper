#include "bot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

#include "json.h"
#include "text.h"

namespace sniper {

static const char* HELP =
    "<b>Commands</b>\n"
    "/snipe or /s <i>lot max</i> – set or change a snipe\n"
    "   add <code>-now</code> to bid right away\n"
    "/list or /l – your snipes\n"
    "/cancel or /c <i># or lot</i> – cancel a snipe\n"
    "/info or /i <i>lot</i> – price and time left\n\n"
    "<i>lot</i> is a lot link or number, e.g.\n"
    "<code>/snipe 266333831 10</code>";

static const std::map<std::string, std::pair<size_t, std::string>> USAGE = {
    {"/snipe", {2, "/snipe <lot> <max>, e.g. /snipe 266333831 10"}},
    {"/list", {0, "/list"}},
    {"/cancel", {1, "/cancel <# or lot>, e.g. /cancel 2 or /cancel 266333831"}},
    {"/info", {1, "/info <lot>"}},
};

static const std::map<std::string, std::string> ALIASES = {
    {"/s", "/snipe"}, {"/l", "/list"}, {"/c", "/cancel"}, {"/i", "/info"}};

static const std::string ASK = "New max for snipe #";
static const std::string ASK_LOT = "Max for lot ";

static std::string digits_after(const std::string& s, size_t start) {
  size_t end = start;
  while (end < s.size() && std::isdigit((unsigned char)s[end])) end++;
  return s.substr(start, end - start);
}

static std::string esc(const std::string& s) { return html_escape(s); }

static std::string link(const std::string& url, const std::string& title) {
  std::string t = prefix(title, 100);
  return "<a href=\"" + html_escape(url, true) + "\">" + esc(t.empty() ? "Lot" : t) + "</a>";
}

static std::pair<std::string, std::string> button(const std::string& label, const std::string& data) {
  return {label, data};
}

static std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

static std::string prices(const Lot& lot) {
  std::string bids = lot.bids >= 0 ? std::to_string(lot.bids) : "?";
  return "Current <b>" + fmt(lot.price) + "</b> · Next bid <b>" + fmt(lot.minimum) + "</b> · " + bids + " bids";
}

static double check_after(double left) {
  return std::min(300.0, std::max(5.0, (left - LEAD - BID_WINDOW) / 2));
}

std::string plain(const std::string& html) {
  std::string out;
  for (size_t i = 0; i < html.size(); i++) {
    size_t close = html[i] == '<' ? html.find('>', i + 1) : std::string::npos;
    if (close != std::string::npos && close > i + 1) {
      i = close;
    } else {
      out += html[i];
    }
  }
  return html_unescape(out);
}

Bot::Bot(Platform& p, NativeBid& nb, Telegram& tg) : p_(p), nb_(nb), tg_(tg) { load(); }

void Bot::load() {
  std::string data;
  if (!p_.load_state(data)) return;
  JsonDocument doc;
  if (deserializeJson(doc, data)) {
    p_.log("Couldn\xE2\x80\x99t read saved snipes; starting empty");
    return;
  }
  offset_ = doc["offset"].isNull() ? -1 : doc["offset"].as<long long>();
  next_id_ = doc["next_id"] | 1L;
  for (JsonObject o : doc["snipes"].as<JsonArray>()) {
    Snipe s;
    s.id = o["id"] | 0L;
    s.url = o["url"] | "";
    s.listing_id = o["listing_id"] | "";
    s.title = o["title"] | "";
    s.state = o["state"] | "pending";
    s.total = o["total"] | "";
    s.max = o["max"] | 0.0;
    s.ends = o["ends"] | 0.0;
    s.check_at = o["check_at"] | 0.0;
    s.now = o["now"] | false;
    snipes.push_back(s);
  }
}

void Bot::save() {
  JsonDocument doc;
  if (offset_ >= 0) {
    doc["offset"] = offset_;
  } else {
    doc["offset"] = nullptr;
  }
  doc["next_id"] = next_id_;
  JsonArray arr = doc["snipes"].to<JsonArray>();
  for (const auto& s : snipes) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = s.id;
    o["url"] = s.url;
    o["listing_id"] = s.listing_id;
    o["title"] = s.title;
    o["max"] = s.max;
    if (s.total.empty()) {
      o["total"] = nullptr;
    } else {
      o["total"] = s.total;
    }
    o["state"] = s.state;
    o["ends"] = s.ends;
    o["check_at"] = s.check_at;
    if (s.now) o["now"] = true;
  }
  std::string out;
  serializeJson(doc, out);
  if (out == saved_) return;
  if (p_.save_state(out)) {
    saved_ = out;
  } else {
    p_.log("Couldn\xE2\x80\x99t save snipes");
  }
}

Snipe* Bot::get(long id) {
  for (auto& s : snipes) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

void Bot::drop(long id) {
  snipes.erase(std::remove_if(snipes.begin(), snipes.end(), [&](const Snipe& s) { return s.id == id; }), snipes.end());
  failing_since_.erase(id);
  save();
}

size_t Bot::pending() const {
  return std::count_if(snipes.begin(), snipes.end(), [](const Snipe& s) { return s.state == "pending"; });
}

void Bot::say(const std::string& text, const Keyboard& kb, long long edit) {
  std::string line = plain(text);
  std::string flat;
  for (char c : line) {
    if (c == '\n') {
      flat += " | ";
    } else {
      flat += c;
    }
  }
  p_.log("\xE2\x86\x92 " + flat);
  outbox_.push_back({text, kb, edit});
}

Result Bot::flush() {
  while (!outbox_.empty()) {
    const Outgoing& m = outbox_.front();
    Result r = tg_.send(m.text, m.kb, m.edit);
    if (!r.ok()) {
      if (r.err != Err::Site || !contains(r.msg, "HTTP 400")) return r;
      p_.log(r.msg + "; sending as plain text");
      r = tg_.send_plain(plain(m.text));
      if (!r.ok()) return r;
    }
    outbox_.pop_front();
  }
  return {};
}

bool Bot::start() {
  Result r = retrying(p_, [&] { return nb_.login(); });
  if (!r.ok()) {
    p_.log("\xE2\x9C\x97 " + r.msg);
    return false;
  }
  size_t count = pending();
  say("🤖 <b>Bot started</b> · " + std::to_string(count) + " snipe" + (count == 1 ? "" : "s") + " pending\n\n" + HELP);
  return true;
}

void Bot::run() {
  if (!start()) return;
  for (;;) {
    Result r = tick();
    if (!r.ok()) {
      p_.log("Error: " + r.msg);
      p_.sleep(5);
    }
  }
}

Result Bot::tick() {
  Result r = refresh();
  if (!r.ok()) return r;
  double now = p_.now();
  const Snipe* due = nullptr;
  for (const auto& s : snipes) {
    if (s.state != "pending") continue;
    if (s.now) {
      due = &s;
      break;
    }
    if (now >= s.ends - LEAD - BID_WINDOW && (!due || s.ends < due->ends)) due = &s;
  }
  if (due) return bid_mode(due->id);
  r = flush();
  if (!r.ok()) return r;
  double wait = next_event() - p_.now();
  if (wait < 1) {
    p_.sleep(wait > 0 ? wait : 0);
    return {};
  }
  return poll((int)std::min(25.0, wait));
}

double Bot::next_event() {
  double t = p_.now() + 25;
  for (const auto& s : snipes) {
    if (s.state == "pending") {
      t = std::min({t, s.ends - LEAD - BID_WINDOW, s.check_at});
    } else if (s.state == "placed") {
      t = std::min(t, s.check_at);
    }
  }
  return t;
}

void Bot::missed(long id) {
  Snipe* s = get(id);
  if (!s) return;
  say("⚠️ <b>Snipe missed</b> · #" + std::to_string(id) + "\n" + link(s->url, s->title) +
      "\n\nThe lot ended before the bid went out.");
  drop(id);
}

void Bot::retry_later(long id, const Result& r) {
  Snipe* s = get(id);
  if (!s) return;
  double now = p_.now();
  auto since = failing_since_.emplace(id, now).first->second;
  if (now - since >= GIVE_UP_AFTER) {
    say("⚠️ <b>Gave up on snipe #" + std::to_string(id) + "</b>\n" + link(s->url, s->title) +
        "\n\nNativeBid hasn’t answered about this lot for a day (" + esc(r.msg) + "), so I stopped tracking it.");
    drop(id);
    return;
  }
  p_.log("Couldn’t check #" + std::to_string(id) + " (" + r.msg + "); trying again in 30s");
  s->check_at = now + 30;
}

Result Bot::refresh() {
  std::vector<long> ids;
  for (const auto& s : snipes) ids.push_back(s.id);
  for (long id : ids) {
    Snipe* s = get(id);
    if (!s || p_.now() < s->check_at) continue;
    double left = 0;
    Result r = nb_.time_remaining(s->listing_id, left);
    if (!r.ok()) {
      retry_later(id, r);
      continue;
    }
    failing_since_.erase(id);
    double now = p_.now();
    s = get(id);
    if (s->state == "pending") {
      if (left <= 0) {
        missed(id);
        continue;
      }
      s->ends = now + left;
      s->check_at = now + check_after(left);
    } else if (left > 0) {
      s->ends = now + left;
      s->check_at = now + std::min(30.0, std::max(1.0, left / 2));
      continue;
    } else {
      p_.sleep(3);
      Lot final;
      r = nb_.lot(s->url, final);
      if (!r.ok()) {
        retry_later(id, r);
        continue;
      }
      s = get(id);
      say("🏁 <b>Lot ended</b> · #" + std::to_string(id) + "\n" + link(s->url, s->title) + "\n\n<b>" +
          esc(final.status.empty() ? "Ended" : final.status) + "</b> at <b>" + fmt(final.price) + "</b>\nYour max: " +
          fmt(s->max));
      drop(id);
      continue;
    }
  }
  return {};
}

Result Bot::bid_mode(long id) {
  Snipe snap = *get(id);
  tg_.release();
  p_.log("Bid mode for #" + std::to_string(id) + "; Telegram paused");
  Wait w = Wait::Fire;
  Result r;
  if (!snap.now) r = wait_for(p_, nb_, snap.listing_id, LEAD, BID_WINDOW * 2, w);
  if (!r.ok()) return r;
  if (w == Wait::Later) {
    p_.log("#" + std::to_string(id) + " was extended; leaving bid mode");
    get(id)->check_at = 0;
    return {};
  }
  if (w == Wait::Ended) {
    missed(id);
    return {};
  }
  bool ok = false;
  std::string message;
  r = fire(p_, nb_, snap.url, snap.max, ok, message);
  if (!r.ok()) {
    int attempt = ++failures_[id];
    if (attempt < 3) {
      p_.log("Bid attempt " + std::to_string(attempt) + " for #" + std::to_string(id) + " failed (" + r.msg +
             "); trying again");
      p_.sleep(2);
      return {};
    }
    ok = false;
    message = r.msg;
  }
  failures_.erase(id);
  p_.log((ok ? "\xE2\x9C\x93 " : "\xE2\x9C\x97 ") + message);
  std::string title = link(snap.url, snap.title);
  if (ok) {
    say("✅ <b>Bid placed</b> · #" + std::to_string(id) + "\n" + title + "\n\nYour max: <b>" + fmt(snap.max) +
        "</b>\n<i>NativeBid: " + esc(message) + "</i>");
    Snipe* s = get(id);
    s->state = "placed";
    s->check_at = p_.now() + LEAD;
    save();
  } else {
    say("❌ <b>Bid failed</b> · #" + std::to_string(id) + "\n" + title + "\n\n<b>" + esc(message) + "</b>");
    drop(id);
  }
  return {};
}

Result Bot::poll(int wait) {
  JsonDocument doc;
  Result r = tg_.updates(offset_, wait, 1, doc);
  if (!r.ok()) return r;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    offset_ = u["update_id"].as<long long>() + 1;
    save();
    JsonObject q = u["callback_query"];
    if (!q.isNull()) {
      long long from = q["from"]["id"] | 0LL;
      std::string data = q["data"] | "";
      if (from != tg_.chat_id()) {
        p_.log("Ignored a button press from " + std::to_string(from));
        continue;
      }
      p_.log("\xE2\x86\x90 [button] " + data);
      press(q["id"] | "", data, q["message"]["message_id"] | 0LL);
    } else {
      JsonObject m = u["message"];
      long long chat = m["chat"]["id"] | 0LL;
      if (chat != tg_.chat_id()) {
        p_.log("Ignored a message from chat " + std::to_string(chat));
        continue;
      }
      std::string text = m["text"] | "";
      p_.log("\xE2\x86\x90 " + text);
      handle(text, m["reply_to_message"]["text"] | "");
    }
    r = flush();
    if (!r.ok()) return r;
  }
  return {};
}

void Bot::report(const Result& r) {
  if (r.ok()) return;
  if (r.err == Err::Network) {
    say("❌ Couldn’t reach NativeBid (" + esc(r.msg) + "). Try again.");
  } else {
    say("❌ " + esc(r.msg));
  }
}

bool Bot::answer_prompt(const std::string& text, const std::string& reply_to) {
  size_t snipe_at = reply_to.find(ASK), lot_at = reply_to.find(ASK_LOT);
  if ((snipe_at == std::string::npos && lot_at == std::string::npos) || starts_with(trim(text), "/")) return false;
  double amount = money(text);
  if (std::isnan(amount) || amount <= 0) {
    report(Result::site("\"" + trim(text) + "\" isn’t a dollar amount. Reply with just the number, like 15."));
    return true;
  }
  if (snipe_at == std::string::npos) {
    report(add(digits_after(reply_to, lot_at + ASK_LOT.size()), amount));
    return true;
  }
  long id = 0;
  Result r = find(digits_after(reply_to, snipe_at + ASK.size()), id);
  if (r.ok()) r = add(get(id)->url, amount);
  report(r);
  return true;
}

void Bot::handle(const std::string& text, const std::string& reply_to) {
  if (answer_prompt(text, reply_to)) return;
  std::vector<std::string> parts = split_ws(text);
  std::string cmd = parts.empty() ? "" : lower(parts[0].substr(0, parts[0].find('@')));
  auto alias = ALIASES.find(cmd);
  if (alias != ALIASES.end()) cmd = alias->second;
  std::vector<std::string> args;
  bool now = false;
  for (size_t i = 1; i < parts.size(); i++) {
    std::string a = lower(parts[i]);
    if (a == "-now" || a == "--now" || a == "\xE2\x80\x94now" || a == "\xE2\x80\x93now") {
      now = true;
    } else {
      args.push_back(parts[i]);
    }
  }
  if (cmd == "/start" || cmd == "/help" || cmd.empty() || cmd[0] != '/') {
    say(HELP);
    return;
  }
  auto usage = USAGE.find(cmd);
  if (usage == USAGE.end()) {
    say("❌ I don’t know <b>" + esc(parts[0]) + "</b>.\n\n" + HELP);
    return;
  }
  size_t count = usage->second.first;
  const std::string& form = usage->second.second;
  if (args.size() != count) {
    std::string extra;
    if (args.size() > count) {
      std::string rest;
      for (size_t i = count; i < args.size(); i++) rest += (rest.empty() ? "" : " ") + args[i];
      extra = " I didn’t understand \"" + rest + "\".";
    }
    report(Result::site("Usage: " + form + "." + extra));
    return;
  }
  if (cmd == "/snipe") {
    double max_bid = money(args[1]);
    if (std::isnan(max_bid) || max_bid <= 0) {
      report(Result::site("\"" + args[1] + "\" isn’t a dollar amount. Usage: " + form));
      return;
    }
    report(add(args[0], max_bid, 0, now));
  } else if (cmd == "/list") {
    report(show());
  } else if (cmd == "/cancel") {
    report(cancel(args[0]));
  } else {
    report(info(args[0]));
  }
}

void Bot::press(const std::string& query_id, const std::string& data, long long msg) {
  size_t colon = data.find(':');
  std::string kind = data.substr(0, colon);
  std::string rest = colon == std::string::npos ? "" : data.substr(colon + 1);
  std::string note;
  Result r;
  if (kind == "edit") {
    long id = 0;
    r = find(rest, id);
    if (r.ok()) {
      const Snipe& s = *get(id);
      say("✏️ <b>" + ASK + std::to_string(id) + "?</b>\n" + link(s.url, s.title) + "\n\nIt’s " + fmt(s.max) +
              " now. Reply with the new amount, like <code>15</code>.",
          Keyboard::reply("New max, e.g. 15"));
    }
  } else if (kind == "new") {
    Lot lot;
    r = nb_.lot(rest, lot);
    if (r.ok()) {
      say("✏️ <b>" + ASK_LOT + rest + "?</b>\n" + link(lot.url, lot.title) + "\n\nNext bid is " + fmt(lot.minimum) +
              ". Reply with your max, like <code>15</code>.",
          Keyboard::reply("Your max, e.g. 15"));
    }
  } else if (kind == "card" || kind == "open") {
    long id = 0;
    r = find(rest, id);
    if (r.ok()) r = card(id, nullptr, 0, "", NAN, kind == "card" ? msg : 0);
  } else if (kind == "cancel") {
    r = cancel(rest, msg);
    note = "Snipe #" + rest + " cancelled";
  } else if (kind == "listcancel") {
    r = cancel(rest, 0, true);
    if (r.ok()) r = show(msg);
    note = "Snipe #" + rest + " cancelled";
  } else if (kind == "list") {
    r = show(msg);
  } else if (kind == "info") {
    r = info(rest, msg);
  }
  Result a;
  if (r.ok()) {
    a = tg_.answer(query_id, note);
  } else if (r.err == Err::Network) {
    a = tg_.answer(query_id, "Couldn’t reach NativeBid (" + r.msg + "). Try again.", true);
  } else {
    a = tg_.answer(query_id, r.msg, true);
  }
  if (!a.ok()) p_.log("Couldn’t answer the button press: " + a.msg);
}

Result Bot::find(const std::string& raw, long& id) {
  std::string ref = trim(raw);
  bool hash = starts_with(ref, "#");
  std::string sid = hash ? ref.substr(1) : ref;
  id = 0;
  for (const auto& s : snipes) {
    if (std::to_string(s.id) == sid) id = s.id;
  }
  if (!id && !hash) {
    Result r = by_lot(ref, id);
    if (!r.ok()) return r;
  }
  if (!id) return Result::site("There’s no snipe matching " + ref + ". Send /list to see your snipes.");
  if (get(id)->state != "pending") {
    return Result::site("Snipe #" + std::to_string(id) + "’s bid has already been placed.");
  }
  return {};
}

static std::vector<std::string> numbers(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
    if (j - i >= 5) out.push_back(s.substr(i, j - i));
    i = j > i ? j : i + 1;
  }
  return out;
}

Result Bot::by_lot(const std::string& ref, long& id) {
  std::vector<std::string> wanted = numbers(ref);
  for (const auto& s : snipes) {
    std::vector<std::string> have = numbers(s.url);
    have.push_back(s.listing_id);
    for (const auto& w : wanted) {
      if (std::find(have.begin(), have.end(), w) != have.end()) {
        id = s.id;
        return {};
      }
    }
  }
  if (wanted.empty() && !starts_with(ref, "http") && !starts_with(ref, "/")) return {};
  Lot lot;
  Result r = nb_.lot(ref, lot);
  if (r.err == Err::Network) return r;
  if (!r.ok()) return {};
  for (const auto& s : snipes) {
    if (s.listing_id == lot.listing_id) id = s.id;
  }
  return {};
}

Result Bot::confirm(const Lot& lot, double amount, std::string& total) {
  std::string action, params;
  Result r = nb_.confirm(lot, amount, action, params, total);
  if (r.err != Err::NotSignedIn) return r;
  r = nb_.login();
  if (!r.ok()) return r;
  return nb_.confirm(lot, amount, action, params, total);
}

Result Bot::add(const std::string& ref, double max_bid, long long edit, bool now_flag) {
  if (std::isnan(max_bid) || max_bid <= 0) return Result::site("Usage: " + USAGE.at("/snipe").second);
  max_bid = cents(max_bid);
  Lot lot;
  Result r = nb_.lot(ref, lot);
  if (!r.ok()) return r;
  double left = 0;
  r = nb_.time_remaining(lot.listing_id, left);
  if (!r.ok()) return r;
  if (left <= 0) return Result::site("That lot has already ended.");
  if (!std::isnan(lot.minimum) && max_bid < lot.minimum) {
    return Result::site("Your max " + fmt(max_bid) + " is below the next bid " + fmt(lot.minimum) + ".");
  }
  Snipe* same = nullptr;
  for (auto& s : snipes) {
    if (s.listing_id == lot.listing_id) same = &s;
  }
  if (same && same->state != "pending") {
    return Result::site("Snipe #" + std::to_string(same->id) + "’s bid of " + fmt(same->max) +
                        " has already been placed, so it can’t be changed here.");
  }
  if (same && cents(same->max) == max_bid && !now_flag) {
    return Result::site("Snipe #" + std::to_string(same->id) + " is already set to " + fmt(max_bid) + ".");
  }
  long same_id = same ? same->id : 0;
  std::string total;
  r = confirm(lot, max_bid, total);
  if (!r.ok()) return r;
  double now = p_.now();
  if (same_id) {
    Snipe* s = get(same_id);
    double old = s->max;
    s->max = max_bid;
    s->total = total;
    s->ends = now + left;
    s->now = s->now || now_flag;
    save();
    return card(same_id, &lot, left, "✏️ <b>Snipe #" + std::to_string(same_id) + " updated</b>", old, edit);
  }
  Snipe s;
  s.id = next_id_++;
  s.url = lot.url;
  s.listing_id = lot.listing_id;
  s.title = lot.title;
  s.max = max_bid;
  s.total = total;
  s.ends = now + left;
  s.check_at = now + check_after(left);
  s.now = now_flag;
  snipes.push_back(s);
  save();
  return card(s.id, &lot, left, "🎯 <b>Snipe #" + std::to_string(s.id) + " scheduled</b>", NAN, edit);
}

Result Bot::card(long id, const Lot* lot, double left, const std::string& heading, double old, long long edit) {
  Lot fetched;
  if (!lot) {
    Result r = nb_.lot(get(id)->url, fetched);
    if (!r.ok()) return r;
    r = nb_.time_remaining(get(id)->listing_id, left);
    if (!r.ok()) return r;
    get(id)->ends = p_.now() + left;
    lot = &fetched;
  }
  const Snipe& s = *get(id);
  std::string sid = std::to_string(id);
  std::string mx = "<b>" + fmt(s.max) + "</b>";
  if (!std::isnan(old)) mx = "<s>" + fmt(old) + "</s> → " + mx;
  std::string text = (heading.empty() ? "🎯 <b>Snipe #" + sid + "</b>" : heading) + "\n" + link(s.url, s.title) +
                     "\n\nYour max: " + mx + "\n<i>Up to " + esc(s.total.empty() ? "?" : s.total) +
                     " with fees</i>\n\n" + prices(*lot) + "\nEnds <b>" + when(s.ends) + "</b>\nIn " + duration(left) +
                     (s.now ? " · <b>bidding right away</b>" : " · bidding " + num(LEAD) + "s before the end");
  Keyboard kb = Keyboard::empty();
  kb.rows.push_back({button("✏️ Change max", "edit:" + sid)});
  kb.rows.push_back({button("🔄 Refresh", "card:" + sid), button("✕ Cancel", "cancel:" + sid)});
  say(text, kb, edit);
  return {};
}

Result Bot::show(long long edit) {
  auto refresh = std::vector<std::pair<std::string, std::string>>{button("🔄 Refresh", "list")};
  if (snipes.empty()) {
    say("📋 <b>No snipes</b>\n\nSet one with <code>/snipe lot max</code>.", Keyboard::of({refresh}), edit);
    return {};
  }
  double now = p_.now();
  std::vector<const Snipe*> sorted;
  for (const auto& s : snipes) sorted.push_back(&s);
  std::stable_sort(sorted.begin(), sorted.end(), [](const Snipe* a, const Snipe* b) { return a->ends < b->ends; });
  std::string text = "📋 <b>Your snipes</b>";
  Keyboard kb = Keyboard::empty();
  for (const Snipe* s : sorted) {
    std::string sid = std::to_string(s->id);
    text += "\n\n<b>#" + sid + "</b> · " + link(s->url, prefix(s->title, 60));
    if (s->state == "pending") {
      text += "\nMax <b>" + fmt(s->max) + "</b> · ends in <b>" + duration(s->ends - now) + "</b>";
      kb.rows.push_back({button("⚙️ Edit #" + sid, "open:" + sid), button("✕ Cancel #" + sid, "listcancel:" + sid)});
    } else {
      text += "\nMax <b>" + fmt(s->max) + "</b> · ✅ bid placed, waiting for the end";
    }
  }
  kb.rows.push_back(refresh);
  say(text, kb, edit);
  return {};
}

Result Bot::cancel(const std::string& ref, long long edit, bool quiet) {
  long id = 0;
  Result r = find(ref, id);
  if (!r.ok()) return r;
  Snipe s = *get(id);
  drop(id);
  if (!quiet) {
    say("🗑 <b>Snipe #" + std::to_string(id) + " cancelled</b>\n" + link(s.url, s.title) + "\n\nMax was " + fmt(s.max) +
            ".",
        Keyboard::empty(), edit);
  }
  return {};
}

Result Bot::info(const std::string& ref, long long edit) {
  Lot lot;
  Result r = nb_.lot(ref, lot);
  if (!r.ok()) return r;
  double left = 0;
  r = nb_.time_remaining(lot.listing_id, left);
  if (!r.ok()) return r;
  std::string ends = left > 0 ? "Ends <b>" + when(p_.now() + left) + "</b>\nIn " + duration(left) : "<b>Ended</b>";
  std::string text = "ℹ️ " + link(lot.url, lot.title) + "\n\nStatus: <b>" +
                     esc(lot.status.empty() ? "unknown" : lot.status) + "</b>\n" + prices(lot) + "\n" + ends;
  if (!std::isnan(lot.my_max)) text += "\n\nYour max on the site: <b>" + fmt(lot.my_max) + "</b>";
  const Snipe* mine = nullptr;
  for (const auto& s : snipes) {
    if (s.listing_id == lot.listing_id) mine = &s;
  }
  if (mine) text += "\n\n🎯 Snipe #" + std::to_string(mine->id) + " is set at <b>" + fmt(mine->max) + "</b>";
  std::vector<std::pair<std::string, std::string>> row;
  if (mine && mine->state == "pending") {
    row.push_back(button("✏️ Change max", "edit:" + std::to_string(mine->id)));
  } else if (!mine && left > 0) {
    std::vector<std::string> lot_numbers = numbers(lot.url);
    row.push_back(button("✏️ Set max", "new:" + (lot_numbers.empty() ? lot.listing_id : lot_numbers[0])));
  }
  row.push_back(button("🔄 Refresh", "info:" + lot.listing_id));
  say(text, Keyboard::of({row}), edit);
  return {};
}

}
