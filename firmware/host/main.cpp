#include <curl/curl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "bot.h"
#include "nativebid.h"
#include "pages.h"
#include "platform_host.h"
#include "telegram.h"
#include "text.h"

using namespace sniper;

static std::map<std::string, std::string> env;

static std::string find_env_dir() {
  char cwd[4096];
  if (!getcwd(cwd, sizeof cwd)) return ".";
  std::string dir = cwd;
  for (int i = 0; i < 4; i++) {
    if (std::ifstream(dir + "/.env")) return dir;
    size_t slash = dir.rfind('/');
    if (slash == std::string::npos || slash == 0) break;
    dir = dir.substr(0, slash);
  }
  return cwd;
}

static void load_env(const std::string& dir) {
  std::ifstream in(dir + "/.env");
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == value.back() && (value[0] == '"' || value[0] == '\'')) {
      value = value.substr(1, value.size() - 2);
    }
    if (!value.empty()) env[key] = value;
  }
  for (const char* key : {"NB_USER", "NB_PASS", "TG_TOKEN", "TG_CHAT_ID"}) {
    const char* v = std::getenv(key);
    if (v && *v) env[key] = v;
  }
}

static std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void describe(Platform& p, const Lot& lot, double left) {
  p.log("Lot " + lot.listing_id + ": " + lot.title);
  p.log("  " + (lot.status.empty() ? std::string("Status unknown") : lot.status) + " · current " + fmt(lot.price) +
        " · next bid " + fmt(lot.minimum) + " · " + (lot.bids >= 0 ? std::to_string(lot.bids) : "?") + " bids" +
        (std::isnan(lot.my_max) ? "" : " · your max " + fmt(lot.my_max)) + " · ends in " + duration(left));
  p.log("  " + lot.url);
}

static int usage() {
  std::fprintf(stderr,
               "usage: sniper bot\n"
               "       sniper info <lot>\n"
               "       sniper confirm <lot> <max>   (signs in and loads the confirmation; never bids)\n"
               "       sniper watch <lot>\n"
               "       sniper scan <lot|confirm|login> <file>\n");
  return 2;
}

static int scan(const std::string& kind, const std::string& path) {
  std::string body = slurp(path);
  auto run = [&](Scanner& s) {
    for (size_t i = 0; i < body.size() && !s.done(); i += CHUNK) s.feed(body.data() + i, std::min(CHUNK, body.size() - i));
    std::printf("read %zu of %zu bytes, buffer peak %zu\n", s.bytes(), body.size(), s.peak());
  };
  if (kind == "lot") {
    LotPage p;
    run(p);
    Lot l = p.lot(path);
    std::printf("listing %s · %s · %s · price %s · next %s · my max %s · %d bids · %zu messages · winning %d %s\n",
                l.listing_id.c_str(), l.title.c_str(), l.status.c_str(), fmt(l.price).c_str(), fmt(l.minimum).c_str(),
                fmt(l.my_max).c_str(), l.bids, p.messages.size(), l.winning, l.standing.c_str());
  } else if (kind == "confirm") {
    ConfirmPage p;
    run(p);
    std::printf("signed out %d · action %s · total %s · fields %s\n", p.signed_out, p.action.c_str(),
                p.get(ConfirmPage::R_TOTAL).c_str(), form_encode(p.fields).c_str());
  } else if (kind == "login") {
    LoginPage p;
    run(p);
    std::printf("error: %s\n", p.get(LoginPage::R_ERROR).c_str());
  } else {
    return usage();
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  if (cmd == "scan") return argc == 4 ? scan(argv[2], argv[3]) : usage();

  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::string dir = find_env_dir();
  load_env(dir);
  HostPlatform platform(dir + "/snipes.json");
  NativeBid nb(platform, env["NB_USER"], env["NB_PASS"]);

  if (cmd == "info" && argc == 3) {
    Lot lot;
    double left = 0;
    Result r = nb.lot(argv[2], lot);
    if (r.ok()) r = nb.time_remaining(lot.listing_id, left);
    if (!r.ok()) {
      platform.log("\xE2\x9C\x97 " + r.msg);
      return 1;
    }
    describe(platform, lot, left);
    return 0;
  }

  if (cmd == "watch" && argc == 3) {
    Lot lot;
    Result r = nb.login();
    if (r.ok()) r = nb.lot(argv[2], lot);
    if (r.ok()) r = nb.watch(lot.listing_id);
    if (!r.ok()) {
      platform.log("\xE2\x9C\x97 " + r.msg);
      return 1;
    }
    platform.log("Watching " + lot.title);
    return 0;
  }

  if (cmd == "confirm" && argc == 4) {
    Lot lot;
    double left = 0;
    std::string action, params, total;
    Result r = nb.lot(argv[2], lot);
    if (r.ok()) r = nb.time_remaining(lot.listing_id, left);
    if (r.ok()) describe(platform, lot, left);
    if (r.ok()) r = nb.confirm(lot, money(argv[3]), action, params, total);
    if (r.err == Err::NotSignedIn) {
      platform.log("Not signed in yet (as expected); signing in");
      r = nb.login();
      if (r.ok()) r = nb.confirm(lot, money(argv[3]), action, params, total);
    }
    if (!r.ok()) {
      platform.log("\xE2\x9C\x97 " + r.msg);
      return 1;
    }
    platform.log("Confirmation OK: " + action + "?" + params + " (up to " + total + " with fees). No bid placed.");
    return 0;
  }

  if (cmd == "bot" && argc == 2) {
    if (env["TG_TOKEN"].empty() || env["TG_CHAT_ID"].empty() || env["NB_USER"].empty() || env["NB_PASS"].empty()) {
      platform.log("\xE2\x9C\x97 Set TG_TOKEN, TG_CHAT_ID, NB_USER and NB_PASS in .env first.");
      return 1;
    }
    Telegram tg(platform, env["TG_TOKEN"], std::atoll(env["TG_CHAT_ID"].c_str()));
    Bot bot(platform, nb, tg);
    bot.run();
    return 1;
  }
  return usage();
}
