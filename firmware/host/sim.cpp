#include <cmath>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bot.h"
#include "text.h"

using namespace sniper;

static int failures = 0;

static void expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
  if (!ok) failures++;
}

struct Clock : Platform {
  double t = 1000000;
  std::string state;
  bool quiet = true;
  bool http(const HttpRequest&, const HeadFn&, const DataFn&, std::string& e) override {
    e = "no network in sim";
    return false;
  }
  double now() override { return t; }
  void sleep(double s) override { t += s > 0 ? s : 0; }
  void log(const std::string& line) override {
    if (!quiet) std::printf("    [%7.1f] %s\n", t - 1000000, line.c_str());
  }
  bool load_state(std::string& out) override {
    out = state;
    return !state.empty();
  }
  int saves = 0;
  std::vector<std::string> released;
  void release(const std::string& host) override { released.push_back(host); }
  bool save_state(const std::string& d) override {
    saves++;
    state = d;
    return true;
  }
};

struct FakeLot {
  std::string id;
  double ends;
  double minimum;
  std::function<void(FakeLot&, double left)> on_check;
  bool placed = false;
  double placed_left = -1;
};

struct FakeNB : NativeBid {
  Clock& c;
  std::map<std::string, FakeLot> lots;
  std::vector<std::string> events;
  std::map<std::string, int> checks;
  std::set<std::string> broken;
  explicit FakeNB(Clock& c) : NativeBid(c, "u", "p"), c(c) {}
  static std::string url(const std::string& key) {
    return key == "X" ? "https://www.nativebid.com/Event/LotDetails/266333831/x" : "url" + key;
  }
  FakeLot* by_ref(const std::string& ref) {
    for (auto& kv : lots) {
      if (ref == kv.first || ref == kv.second.id || ref == url(kv.first)) return &kv.second;
      if (ref.size() >= 5 && url(kv.first).find("/" + ref + "/") != std::string::npos) return &kv.second;
    }
    return nullptr;
  }
  double slow_until = 0;
  Result lot(const std::string& ref, Lot& out) override {
    if (c.t - 1000000 < slow_until && ref.rfind("url", 0) != 0) c.sleep(8);
    FakeLot* f = by_ref(ref);
    if (!f) return Result::site("no lot " + ref);
    std::string key;
    for (auto& kv : lots) {
      if (&kv.second == f) key = kv.first;
    }
    bool ended = c.t >= f->ends;
    out = Lot();
    out.url = url(key);
    out.listing_id = f->id;
    out.title = key == "X" ? "Rug <Blue> & Gold 5'x7\" (2-Piece)" : "Lot " + key;
    out.price = f->minimum - 1;
    out.minimum = ended ? NAN : f->minimum;
    out.status = ended ? "Ended" : "Active";
    out.bids = 3;
    return {};
  }
  Result time_remaining(const std::string& id, double& left) override {
    checks[id]++;
    if (broken.count(id)) return Result::site("GetTimeRemaining error: ERROR_NOT_FOUND");
    for (auto& kv : lots) {
      if (kv.second.id == id) {
        left = std::max(0.0, kv.second.ends - c.t);
        if (kv.second.on_check) kv.second.on_check(kv.second, left);
        left = std::max(0.0, kv.second.ends - c.t);
        return {};
      }
    }
    return Result::site("unknown id");
  }
  Result login() override {
    events.push_back("login");
    return {};
  }
  int confirms = 0;
  std::string confirm_error;
  Result confirm(const Lot& lot, double amount, std::string& action, std::string& params, std::string& total) override {
    (void)lot;
    if (!confirm_error.empty() && c.t - 1000000 > 30) {
      confirms++;
      return Result::site(confirm_error);
    }
    action = "/Listing/Action";
    params = "x";
    char buf[32];
    std::snprintf(buf, sizeof buf, "$%.2f", amount * 1.15 + 1);
    total = buf;
    return {};
  }
  int places = 0;
  Result place(const std::string&, const std::string&, bool& ok, std::string& message) override {
    places++;
    for (auto& kv : lots) {
      double left = kv.second.ends - c.t;
      if (!kv.second.placed && left > 0 && left < 62) {
        kv.second.placed = true;
        kv.second.placed_left = left;
        events.push_back("placed " + kv.first);
      }
    }
    ok = true;
    message = "ActionSuccess";
    return {};
  }
};

struct Incoming {
  double at;
  long long chat;
  std::string text;
};

struct FakeTG : Telegram {
  Clock& c;
  std::vector<Incoming> inbox;
  std::vector<std::pair<double, std::string>> sent;
  std::vector<std::pair<double, int>> polls;
  std::map<long long, std::pair<std::string, Keyboard>> messages;
  std::vector<std::string> answers;
  long long next_id = 1;
  long long update = 100;
  explicit FakeTG(Clock& c) : Telegram(c, "token", 42), c(c) {}
  Result send(const std::string& text, const Keyboard& kb, long long edit) override {
    if (!edit) edit = next_id++;
    messages[edit] = {text, kb};
    sent.push_back({c.t - 1000000, text});
    return {};
  }
  Result send_plain(const std::string& text) override {
    sent.push_back({c.t - 1000000, text});
    return {};
  }
  Result answer(const std::string&, const std::string& text, bool alert) override {
    answers.push_back(text + (alert ? " (alert)" : ""));
    return {};
  }
  int max_batch = 0;
  Result updates(long long, int wait, int limit, JsonDocument& reply) override {
    reply.clear();
    JsonArray result = reply["result"].to<JsonArray>();
    for (size_t i = 0; i < inbox.size() && (int)result.size() < limit;) {
      if (inbox[i].at <= c.t - 1000000) {
        JsonObject u = result.add<JsonObject>();
        u["update_id"] = update++;
        u["message"]["chat"]["id"] = inbox[i].chat;
        u["message"]["text"] = inbox[i].text;
        inbox.erase(inbox.begin() + i);
      } else {
        i++;
      }
    }
    max_batch = std::max(max_batch, (int)result.size());
    if (result.size()) return {};
    polls.push_back({c.t - 1000000, wait});
    double next = c.t + wait;
    for (const auto& m : inbox) next = std::min(next, m.at + 1000000);
    c.t = std::max(c.t, next);
    return {};
  }
};

static void run_until(Bot& bot, Clock& c, double until) {
  while (c.t - 1000000 < until) {
    Result r = bot.tick();
    if (!r.ok()) {
      std::printf("    tick error: %s\n", r.msg.c_str());
      c.sleep(5);
    }
  }
}

static bool sent_with(const FakeTG& tg, const std::string& needle, double* when = nullptr) {
  for (const auto& s : tg.sent) {
    if (contains(s.second, needle)) {
      if (when) *when = s.first;
      return true;
    }
  }
  return false;
}

static void timing() {
  std::printf("\nscenario: two snipes, commands, a late extension after bidding\n");
  Clock c;
  FakeNB nb(c);
  FakeTG tg(c);
  nb.lots["A"] = {"101", c.t + 300, 8.0, nullptr};
  bool extended = false;
  nb.lots["B"] = {"202", c.t + 320, 5.0, [&](FakeLot& l, double left) {
                    if (l.placed && left < 50 && !extended) {
                      l.ends += 120;
                      extended = true;
                    }
                  }};
  nb.lots["C"] = {"303", c.t + 2000, 1.0, nullptr};
  tg.inbox = {{5, 42, "/start"},        {6, 42, "/snipe A 10"},   {7, 42, "/snipe B 4"},
              {8, 42, "/s B 9"},        {9, 42, "/snipe A 11"},   {10, 999, "/snipe C 50"},
              {11, 42, "/snipe C 50"},  {12, 42, "/l"},           {13, 42, "/cancel 9"},
              {14, 42, "/c #3"},        {15, 42, "/info A"},      {16, 42, "hello"}};
  Bot bot(c, nb, tg);
  bot.start();
  run_until(bot, c, 700);
  for (const auto& s : tg.sent) {
    std::string line = plain(s.second);
    for (auto& ch : line) {
      if (ch == '\n') ch = '|';
    }
    std::printf("    [%5.0fs] %s\n", s.first, prefix(line, 110).c_str());
  }
  expect(nb.lots["A"].placed && std::fabs(nb.lots["A"].placed_left - 60) < 0.01, "A bid placed exactly 60s before the end");
  expect(nb.lots["B"].placed && std::fabs(nb.lots["B"].placed_left - 60) < 0.01, "B bid placed exactly 60s before the end");
  expect(!nb.lots["C"].placed, "cancelled snipe C never bid");
  expect(!sent_with(tg, "Snipe #4"), "message from another chat ignored");
  expect(sent_with(tg, "<s>$10.00</s> → <b>$11.00</b>"), "/snipe on the same lot updates the max");
  expect(sent_with(tg, "below the next bid $5.00"), "max below next bid rejected");
  double ended_b = 0;
  expect(sent_with(tg, "Lot ended</b> · #2", &ended_b) && ended_b > 440, "B's ended message waits for the extension");
  bool overlap = false;
  for (const auto& p : tg.polls) {
    for (double fire : {300.0 - LEAD, 320.0 - LEAD}) {
      if (p.first < fire && p.first + p.second > fire - BID_WINDOW) overlap = true;
    }
  }
  expect(!overlap, "no Telegram poll runs into a bid window");
  expect(std::count(c.released.begin(), c.released.end(), "api.telegram.org") >= 2,
         "bid mode closes the Telegram connection");
  std::printf("    %d state writes over 700s\n", c.saves);
}

static void extension_and_restart() {
  std::printf("\nscenario: extension during bid mode, lot ending in 40s, restart mid-wait\n");
  Clock c;
  FakeNB nb(c);
  nb.lots["D"] = {"1", c.t + 300, 2.0, [&](FakeLot& l, double left) {
                    static bool done = false;
                    if (!done && left < 70) {
                      l.ends += 200;
                      done = true;
                    }
                  }};
  nb.lots["E"] = {"2", c.t + 40, 2.0, nullptr};
  {
    FakeTG tg(c);
    tg.inbox = {{1, 42, "/snipe D 5"}, {2, 42, "/snipe E 5"}};
    Bot bot(c, nb, tg);
    bot.start();
    run_until(bot, c, 150);
    expect(nb.lots["E"].placed, "lot ending in 40s bids right away");
    expect(sent_with(tg, "Lot ended</b> · #2"), "E's ended message sent");
  }
  FakeTG tg(c);
  Bot bot(c, nb, tg);
  bot.start();
  bot.flush();
  expect(sent_with(tg, "1 snipe pending"), "restart reloads the pending snipe");
  run_until(bot, c, 800);
  expect(nb.lots["D"].placed && std::fabs(nb.lots["D"].placed_left - 60) < 0.01,
         "after the extension, D bids 60s before the new end");
  expect(sent_with(tg, "Lot ended</b> · #1"), "D's ended message sent");
}

struct TagCheck {
  bool ok = true;
  std::vector<std::string> stack;
};

static bool valid_html(const std::string& s) {
  static const std::vector<std::string> allowed = {"b", "i", "s", "u", "a", "code", "pre", "blockquote"};
  std::vector<std::string> stack;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '<') continue;
    size_t close = s.find('>', i);
    if (close == std::string::npos) return false;
    std::string tag = s.substr(i + 1, close - i - 1);
    bool end = !tag.empty() && tag[0] == '/';
    std::string name = tag.substr(end ? 1 : 0, tag.find(' ') == std::string::npos ? std::string::npos : tag.find(' ') - (end ? 1 : 0));
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) return false;
    if (end) {
      if (stack.empty() || stack.back() != name) return false;
      stack.pop_back();
    } else {
      stack.push_back(name);
    }
    i = close;
  }
  return stack.empty();
}

static void buttons_and_html() {
  std::printf("\nscenario: buttons, HTML and command parsing\n");
  Clock c;
  FakeNB nb(c);
  FakeTG tg(c);
  nb.lots["X"] = {"266333832", c.t + 200000, 8.0, nullptr};
  Bot bot(c, nb, tg);
  auto msg = [&](const std::string& t) {
    bot.handle(t);
    bot.flush();
  };
  auto press = [&](long long id, const std::string& label) {
    for (const auto& row : tg.messages[id].second.rows) {
      for (const auto& b : row) {
        if (b.first == label) {
          bot.press("q", b.second, id);
          bot.flush();
          return;
        }
      }
    }
    expect(false, "button " + label + " exists on message " + std::to_string(id));
  };
  auto reply = [&](long long prompt, const std::string& t) {
    bot.handle(t, plain(tg.messages[prompt].first));
    bot.flush();
  };
  msg("/help");
  msg("/snipe X 10");
  long long card = tg.next_id - 1;
  press(card, "✏️ Change max");
  long long prompt = tg.next_id - 1;
  expect(tg.messages[prompt].second.force_reply && contains(tg.messages[prompt].first, "New max for snipe #1?"),
         "✏️ asks for the new max with the reply keyboard open");
  reply(prompt, "15");
  expect(contains(tg.messages[tg.next_id - 1].first, "<s>$10.00</s> → <b>$15.00</b>"), "replying 15 updates the max");
  reply(prompt, "$4");
  expect(contains(plain(tg.sent.back().second), "Your max $4.00 is below the next bid $8.00."), "too-low reply explained");
  reply(prompt, "lots");
  expect(contains(plain(tg.sent.back().second), "\"lots\" isn’t a dollar amount"), "non-number reply explained");
  reply(prompt, "/l");
  expect(contains(plain(tg.sent.back().second), "Your snipes"), "a command sent as a reply still runs as a command");
  reply(prompt, "10");
  msg("/info X");
  long long info = tg.next_id - 1;
  expect(contains(tg.messages[info].first, "Snipe #1 is set at <b>$10.00</b>"), "/info shows the snipe");
  press(info, "✏️ Change max");
  expect(contains(tg.messages[tg.next_id - 1].first, "New max for snipe #1?"), "/info ✏️ on a sniped lot asks for its new max");
  msg("/list");
  long long list = tg.next_id - 1;
  press(list, "⚙️ Edit #1");
  expect(tg.next_id - 1 == list + 1, "Edit opens a new card");
  press(list, "✕ Cancel #1");
  expect(contains(tg.messages[list].first, "No snipes"), "Cancel from the list re-renders it");
  press(card, "✏️ Change max");
  expect(contains(tg.answers.back(), "no snipe matching 1"), "stale card button explains the snipe is gone");
  reply(prompt, "20");
  expect(contains(plain(tg.sent.back().second), "no snipe matching 1"), "reply to a stale prompt explains the snipe is gone");
  msg("/i X");
  long long fresh = tg.next_id - 1;
  press(fresh, "✏️ Set max");
  long long ask = tg.next_id - 1;
  expect(tg.messages[ask].second.force_reply && contains(tg.messages[ask].first, "Max for lot 266333831?"),
         "/info ✏️ on an unsniped lot asks for a max");
  reply(ask, "12");
  expect(contains(tg.messages[tg.next_id - 1].first, "scheduled</b>") &&
             contains(tg.messages[tg.next_id - 1].first, "<b>$12.00</b>"),
         "replying schedules a new snipe at that max");
  reply(ask, "13");
  expect(contains(tg.messages[tg.next_id - 1].first, "<s>$12.00</s> → <b>$13.00</b>"),
         "replying again to the same prompt updates that snipe");
  msg("/c 266333831");
  nb.lots["Z"] = {"999", c.t - 10, 1.0, nullptr};
  msg("/i Z");
  bool pencil = false;
  for (const auto& row : tg.messages[tg.next_id - 1].second.rows) {
    for (const auto& b : row) pencil = pencil || contains(b.first, "✏️");
  }
  expect(contains(tg.messages[tg.next_id - 1].first, "<b>Ended</b>") && !pencil, "/info on an ended lot has no ✏️");
  msg("/snipe X 10");
  msg("/cancel https://www.nativebid.com/Event/LotDetails/266333831/x");
  expect(contains(tg.sent.back().second, "cancelled"), "/cancel by URL");
  msg("/s https://www.nativebid.com/Event/LotDetails/266818742/Lamp -now");
  expect(contains(plain(tg.sent.back().second), "Missing your max. Put it after the lot, e.g.\n/s 266818742 10 -now"),
         "a missing max is explained with the lot number");
  msg("/snipe 1 10 later");
  expect(contains(plain(tg.sent.back().second), "I didn’t understand \"later\""), "extra words explained");
  msg("/s 1 ten");
  expect(contains(plain(tg.sent.back().second), "\"ten\" isn’t a dollar amount"), "bad amount explained");
  msg("/bogus");
  expect(contains(plain(tg.sent.back().second), "I don’t know /bogus"), "unknown command explained");
  bool html_ok = true, data_ok = true;
  for (const auto& s : tg.sent) html_ok = html_ok && valid_html(s.second);
  for (const auto& m : tg.messages) {
    html_ok = html_ok && valid_html(m.second.first);
    for (const auto& row : m.second.second.rows) {
      for (const auto& b : row) data_ok = data_ok && b.second.size() <= 64;
    }
  }
  expect(html_ok, "every message is valid Telegram HTML (title has < > & \" ')");
  expect(data_ok, "every button's data fits Telegram's 64-byte limit");
  std::printf("    sample card:\n");
  std::string sample = plain(tg.messages[card].first);
  std::printf("      %s\n", sample.c_str());
}

static void broken_snipe() {
  std::printf("\nscenario: one snipe's lot starts failing while another is due\n");
  Clock c;
  FakeNB nb(c);
  FakeTG tg(c);
  nb.lots["F"] = {"1", c.t + 100, 2.0, nullptr};
  nb.lots["G"] = {"2", c.t + 400, 2.0, nullptr};
  tg.inbox = {{1, 42, "/snipe F 5"}, {2, 42, "/snipe G 5"}};
  Bot bot(c, nb, tg);
  bot.start();
  run_until(bot, c, 50);
  expect(nb.lots["F"].placed, "F's bid placed before it breaks");
  nb.broken.insert("1");
  int before = nb.checks["1"];
  run_until(bot, c, 600);
  expect(nb.lots["G"].placed && std::fabs(nb.lots["G"].placed_left - 60) < 0.01,
         "G still bids on time while F's checks fail");
  int calls = nb.checks["1"] - before;
  expect(calls > 0 && calls <= 20, "F's failing check backs off to every 30s (" + std::to_string(calls) + " tries in 550s)");
  bool f_kept = false;
  for (const auto& s : bot.snipes) f_kept = f_kept || s.id == 1;
  expect(f_kept, "F is kept while it has failed for less than a day");
  run_until(bot, c, 600 + 24 * 3600 + 60);
  expect(sent_with(tg, "Gave up on snipe #1"), "after a day of failures F is given up on");
  bool f_gone = true;
  for (const auto& s : bot.snipes) f_gone = f_gone && s.id != 1;
  expect(f_gone, "F is removed from the saved snipes");
}

static void failing_bid() {
  std::printf("\nscenario: NativeBid rejects the bid at bid time\n");
  Clock c;
  FakeNB nb(c);
  FakeTG tg(c);
  nb.lots["H"] = {"1", c.t + 300, 2.0, nullptr};
  nb.confirm_error = "NativeBid didn’t accept that bid: Bidding is closed";
  tg.inbox = {{1, 42, "/snipe H 5"}};
  Bot bot(c, nb, tg);
  bot.start();
  run_until(bot, c, 400);
  expect(nb.confirms == 3, "tries the bid 3 times (" + std::to_string(nb.confirms) + ")");
  expect(sent_with(tg, "Bid failed</b> · #1") && sent_with(tg, "Bidding is closed"), "reports Bid failed with the reason");
  expect(!sent_with(tg, "Snipe missed"), "doesn't claim the snipe was missed");
  expect(bot.snipes.empty(), "the failed snipe is removed");
}

static void bid_now() {
  std::printf("\nscenario: -now bids right away, survives a restart, other snipes still wait\n");
  Clock c;
  FakeNB nb(c);
  FakeTG tg(c);
  nb.lots["N"] = {"1", c.t + 3600, 2.0, nullptr};
  nb.lots["W"] = {"2", c.t + 7200, 2.0, nullptr};
  nb.lots["R"] = {"3", c.t + 3600, 2.0, nullptr};
  tg.inbox = {{1, 42, "/s W 5"}, {2, 42, "/snipe N 5 -NOW"}};
  Bot bot(c, nb, tg);
  bot.start();
  run_until(bot, c, 60);
  double at = -1;
  expect(sent_with(tg, "bidding right away"), "the card says it's bidding right away");
  expect(sent_with(tg, "Bid placed</b> · #2", &at) && at < 10, "bid placed within seconds (at " + std::to_string(at) + ")");
  expect(nb.places == 1, "only the -now snipe bid (" + std::to_string(nb.places) + ")");
  expect(bot.snipes.size() == 2 && !bot.snipes[1].now && bot.snipes[1].state == "placed", "the -now snipe is placed");
  expect(bot.snipes[0].state == "pending", "the other snipe still waits");
  bot.handle("/l");
  bot.flush();
  const Keyboard& list_kb = tg.messages.rbegin()->second.second;
  bool edit2 = false;
  for (const auto& row : list_kb.rows) {
    for (const auto& b : row) edit2 = edit2 || b.second == "open:2";
  }
  expect(edit2, "/list offers Edit for a placed snipe");
  bot.handle("/s N 4");
  bot.flush();
  expect(contains(tg.sent.back().second, "can’t lower it"), "lowering a placed bid is explained");
  bot.handle("/s N 5");
  bot.flush();
  expect(bot.snipes[1].state == "placed" && contains(tg.sent.back().second, "can’t lower it"), "the same max is refused too");
  bot.handle("/s N 7 -now");
  bot.flush();
  expect(contains(tg.sent.back().second, "Snipe #2 raised"), "raising a placed bid is accepted");
  run_until(bot, c, c.t - 1000000 + 20);
  expect(nb.places == 2 && bot.snipes[1].state == "placed" && bot.snipes[1].max == 7, "the raise with -now bids right away");
  bot.handle("/s N 8");
  bot.flush();
  run_until(bot, c, c.t - 1000000 + 600);
  expect(nb.places == 2 && bot.snipes[1].state == "pending", "a raise without -now waits for the end");

  Clock c2;
  c2.t = c.t;
  FakeNB nb2(c2);
  FakeTG tg2(c2);
  nb2.lots["R"] = {"3", c2.t + 3600, 2.0, nullptr};
  Bot first(c2, nb2, tg2);
  first.start();
  first.handle("/snipe R 5 \xE2\x80\x94now");
  expect(first.snipes.size() == 1 && first.snipes[0].now, "an em-dash \"—now\" also counts");
  Bot second(c2, nb2, tg2);
  expect(second.snipes.size() == 1 && second.snipes[0].now, "the -now flag is saved");
  tg2.inbox.clear();
  second.start();
  run_until(second, c2, c2.t - 1000000 + 30);
  expect(nb2.places == 1 && second.snipes[0].state == "placed", "after a restart it still bids right away");
}

static void command_burst() {
  std::printf("\nscenario: five slow commands arrive just before a bid window\n");
  Clock c;
  FakeNB nb(c);
  FakeTG tg(c);
  nb.lots["J"] = {"1", c.t + 300, 2.0, nullptr};
  tg.inbox = {{1, 42, "/snipe J 5"}};
  for (int i = 0; i < 5; i++) tg.inbox.push_back({220, 42, "/info J"});
  Bot bot(c, nb, tg);
  bot.start();
  run_until(bot, c, 10);
  nb.slow_until = 300;
  run_until(bot, c, 400);
  expect(tg.max_batch == 1, "reads one Telegram update at a time");
  expect(nb.lots["J"].placed && std::fabs(nb.lots["J"].placed_left - 60) < 0.01,
         "bid still lands 60s before the end despite 8s-per-request commands");
}

struct Canned : Clock {
  std::string body;
  bool http(const HttpRequest&, const HeadFn& head, const DataFn& data, std::string&) override {
    HttpHead h;
    h.status = 200;
    if (head && !head(h)) return true;
    if (data) data(body.data(), body.size());
    return true;
  }
};

static void time_replies() {
  std::printf("\nscenario: odd GetTimeRemaining replies\n");
  Canned c;
  NativeBid nb(c, "u", "p");
  auto check = [&](const std::string& body, bool ok, double want) {
    c.body = body;
    double left = -1;
    Result r = nb.time_remaining("1", left);
    expect(r.ok() == ok && (!ok || std::fabs(left - want) < 1e-3),
           body + " -> " + (r.ok() ? "left " + std::to_string(left) : "error: " + r.msg));
  };
  check("{\"secondsRemaining\": 706.7, \"error\": \"\"}", true, 706.7);
  check("{\"secondsRemaining\": 5, \"error\": \"\"}", true, 5);
  check("{\"secondsRemaining\": 0, \"error\": \"\"}", true, 0);
  check("{\"error\": \"\"}", false, 0);
  check("{\"secondsRemaining\": null, \"error\": \"\"}", false, 0);
  check("{\"secondsRemaining\": \"70\", \"error\": \"\"}", false, 0);
  check("{\"secondsRemaining\": 0, \"error\": \"ERROR_NOT_FOUND\"}", false, 0);
}

int main() {
  bid_now();
  time_replies();
  command_burst();
  failing_bid();
  broken_snipe();
  timing();
  extension_and_restart();
  buttons_and_html();
  std::printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
  return failures ? 1 : 0;
}
