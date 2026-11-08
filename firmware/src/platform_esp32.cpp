#include "platform_esp32.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

#include <cstdlib>

#include "scanner.h"
#include "text.h"

extern const uint8_t crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

namespace sniper {

namespace {

bool expired(uint32_t deadline) {
  return (int32_t)(millis() - deadline) >= 0;
}

class Reader {
 public:
  Reader(WiFiClientSecure& c, uint32_t deadline) : c_(c), deadline_(deadline) {}

  size_t received() const { return received_; }
  void extend(uint32_t deadline) { deadline_ = deadline; }

  bool line(std::string& out) {
    out.clear();
    for (;;) {
      int ch = get();
      if (ch < 0) return false;
      if (ch == '\n') break;
      if (ch != '\r') out += (char)ch;
      if (out.size() > 8192) return false;
    }
    return true;
  }

  size_t read(char* dst, size_t want) {
    if (pos_ == len_ && !fill()) return 0;
    size_t n = len_ - pos_ < want ? len_ - pos_ : want;
    memcpy(dst, buf_ + pos_, n);
    pos_ += n;
    return n;
  }

 private:
  int get() {
    if (pos_ == len_ && !fill()) return -1;
    return (uint8_t)buf_[pos_++];
  }

  bool fill() {
    while (!expired(deadline_)) {
      int avail = c_.available();
      if (avail > 0) {
        int n = c_.read((uint8_t*)buf_, avail < (int)sizeof buf_ ? avail : (int)sizeof buf_);
        if (n > 0) {
          len_ = n;
          pos_ = 0;
          received_ += n;
          return true;
        }
      } else if (!c_.connected()) {
        return false;
      } else {
        delay(2);
      }
      esp_task_wdt_reset();
    }
    return false;
  }

  WiFiClientSecure& c_;
  uint32_t deadline_;
  char buf_[512];
  size_t len_ = 0;
  size_t pos_ = 0;
  size_t received_ = 0;
};

bool split_url(const std::string& url, std::string& host, std::string& path) {
  if (!starts_with(url, "https://")) return false;
  size_t start = 8;
  size_t slash = url.find('/', start);
  host = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
  path = slash == std::string::npos ? "/" : url.substr(slash);
  return !host.empty();
}

}

constexpr size_t MAX_CONNECTIONS = 2;
constexpr size_t MAX_SKIP = 300 * 1024;
constexpr uint32_t SKIP_MS = 4000;

void EspPlatform::release(const std::string& host) {
  for (size_t i = 0; i < connections_.size(); i++) {
    if (connections_[i].host == host) {
      connections_[i].client->stop();
      connections_.erase(connections_.begin() + i);
      return;
    }
  }
}

void EspPlatform::release_all() {
  for (auto& c : connections_) c.client->stop();
  connections_.clear();
}

EspPlatform::Connection* EspPlatform::open(const std::string& host, bool& reused, std::string& error) {
  for (auto& c : connections_) {
    if (c.host == host && c.client->connected()) {
      reused = true;
      c.used = millis();
      return &c;
    }
  }
  release(host);
  if (connections_.size() >= MAX_CONNECTIONS) {
    size_t oldest = 0;
    for (size_t i = 1; i < connections_.size(); i++) {
      if ((int32_t)(connections_[i].used - connections_[oldest].used) < 0) oldest = i;
    }
    release(connections_[oldest].host);
  }
  Connection c;
  c.host = host;
  c.client.reset(new WiFiClientSecure());
  c.client->setCACertBundle(crt_bundle_start);
  c.client->setHandshakeTimeout(15);
  uint32_t started = millis();
  if (!c.client->connect(host.c_str(), 443)) {
    char msg[96];
    c.client->lastError(msg, sizeof msg);
    error = "couldn't connect to " + host + " (" + msg + ")";
    return nullptr;
  }
  char took[96];
  snprintf(took, sizeof took, "Connected to %s in %.1fs", host.c_str(), (millis() - started) / 1000.0);
  log(took);
  c.used = millis();
  connections_.push_back(std::move(c));
  reused = false;
  return &connections_.back();
}

bool EspPlatform::http(const HttpRequest& req, const HeadFn& on_head, const DataFn& on_data, std::string& error) {
  std::string host, path;
  if (!split_url(req.url, host, path)) {
    error = "only https URLs are supported";
    return false;
  }
  bool lit = contains(host, "nativebid.com");
  if (lit) digitalWrite(ACTIVITY_LED, HIGH);
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
    bool retry = false;
    ok = exchange(req, host, path, on_head, on_data, error, retry);
    if (!ok) release(host);
    if (!ok && !retry) break;
  }
  if (lit) digitalWrite(ACTIVITY_LED, LOW);
  return ok;
}

bool EspPlatform::exchange(const HttpRequest& req, const std::string& host, const std::string& path,
                           const HeadFn& on_head, const DataFn& on_data, std::string& error, bool& retry) {
  bool reused = false;
  Connection* conn = open(host, reused, error);
  if (!conn) return false;
  WiFiClientSecure& client = *conn->client;

  std::string head = req.method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: keep-alive\r\n";
  for (const auto& h : req.headers) head += h.name + ": " + h.value + "\r\n";
  if (req.method != "GET" || !req.body.empty()) head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
  head += "\r\n";
  bool sent = client.write((const uint8_t*)head.data(), head.size()) == head.size();
  if (sent && !req.body.empty()) {
    sent = client.write((const uint8_t*)req.body.data(), req.body.size()) == req.body.size();
  }
  if (!sent) {
    retry = reused;
    error = "couldn't send the request";
    return false;
  }

  Reader in(client, millis() + (uint32_t)req.timeout_s * 1000);
  std::string line;
  if (!in.line(line) || !starts_with(line, "HTTP/")) {
    retry = reused && in.received() == 0;
    error = in.received() ? "bad response from " + host : "no response from " + host;
    return false;
  }
  HttpHead result;
  size_t sp = line.find(' ');
  result.status = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
  long length = -1;
  bool chunked = false, closing = false;
  for (;;) {
    if (!in.line(line)) {
      error = "response headers cut off";
      return false;
    }
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    Header h{trim(line.substr(0, colon)), trim(line.substr(colon + 1))};
    std::string name = lower(h.name), value = lower(h.value);
    if (name == "content-length") length = std::atol(h.value.c_str());
    if (name == "transfer-encoding" && contains(value, "chunked")) chunked = true;
    if (name == "connection" && contains(value, "close")) closing = true;
    result.headers.push_back(std::move(h));
  }

  if (on_head && !on_head(result)) {
    release(host);
    return true;
  }

  char buf[CHUNK];
  bool skipping = false;
  size_t skipped = 0;
  uint32_t skip_started = 0;
  auto pump = [&](long count, bool until_close) {
    while (until_close || count > 0) {
      size_t want = until_close || count > (long)sizeof buf ? sizeof buf : (size_t)count;
      size_t got = in.read(buf, want);
      if (!got) return until_close ? 1 : 0;
      if (!until_close) count -= got;
      if (skipping) {
        skipped += got;
        if (skipped > MAX_SKIP) return 2;
      } else if (on_data && !on_data(buf, got)) {
        if (until_close) return 2;
        skipping = true;
        skip_started = millis();
        in.extend(skip_started + SKIP_MS);
      }
    }
    return 1;
  };

  int outcome = 1;
  if (chunked) {
    for (;;) {
      if (!in.line(line)) {
        outcome = 0;
        break;
      }
      long size = std::strtol(line.c_str(), nullptr, 16);
      if (size <= 0) {
        while (in.line(line) && !line.empty()) {
        }
        break;
      }
      outcome = pump(size, false);
      if (outcome != 1) break;
      in.line(line);
    }
  } else if (length >= 0) {
    outcome = pump(length, false);
  } else {
    outcome = pump(0, true);
    closing = true;
  }

  if (outcome == 0 && skipping) outcome = 2;
  if (outcome == 0) {
    error = "response cut off";
    return false;
  }
  if (skipping && outcome == 1) {
    char took[96];
    snprintf(took, sizeof took, "Skipped %u bytes in %.2fs to keep the connection", (unsigned)skipped,
             (millis() - skip_started) / 1000.0);
    log(took);
  }
  if (outcome == 2 || closing) release(host);
  if (outcome == 1 && !closing) {
    for (auto& c : connections_) {
      if (c.host == host) c.used = millis();
    }
  }
  return true;
}

double EspPlatform::now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

void EspPlatform::sleep(double seconds) {
  if (seconds <= 0) return;
  uint32_t left = (uint32_t)(seconds * 1000);
  while (left) {
    uint32_t step = left < 1000 ? left : 1000;
    esp_task_wdt_reset();
    delay(step);
    left -= step;
  }
  esp_task_wdt_reset();
}

void EspPlatform::log(const std::string& line) {
  Serial.printf("%s %s  [heap %u, min %u, block %u]\n", clock_time(now()).c_str(), line.c_str(), ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
}

bool EspPlatform::load_state(std::string& out) {
  Preferences prefs;
  if (!prefs.begin("sniper", true)) return false;
  size_t len = prefs.getBytesLength("state");
  if (len) {
    out.resize(len);
    prefs.getBytes("state", &out[0], len);
  }
  prefs.end();
  return len > 0;
}

bool EspPlatform::save_state(const std::string& data) {
  Preferences prefs;
  if (!prefs.begin("sniper", false)) return false;
  bool ok = prefs.putBytes("state", data.data(), data.size()) == data.size();
  prefs.end();
  return ok;
}

}
