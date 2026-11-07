#include "platform_host.h"

#include <curl/curl.h>
#include <sys/time.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#include "scanner.h"
#include "text.h"

namespace sniper {

namespace {

struct Transfer {
  CURL* curl;
  const HeadFn* on_head;
  const DataFn* on_data;
  HttpHead head;
  bool head_done = false;
  bool want = true;
  bool stopped = false;
};

void deliver_head(Transfer* t) {
  if (t->head_done) return;
  t->head_done = true;
  long code = 0;
  curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &code);
  t->head.status = (int)code;
  t->want = !*t->on_head || (*t->on_head)(t->head);
}

size_t on_header(char* buf, size_t size, size_t n, void* user) {
  auto* t = static_cast<Transfer*>(user);
  std::string line = trim(std::string(buf, size * n));
  if (starts_with(line, "HTTP/")) {
    t->head.headers.clear();
  } else {
    size_t colon = line.find(':');
    if (colon != std::string::npos) t->head.headers.push_back({trim(line.substr(0, colon)), trim(line.substr(colon + 1))});
  }
  return size * n;
}

size_t on_write(char* buf, size_t size, size_t n, void* user) {
  auto* t = static_cast<Transfer*>(user);
  deliver_head(t);
  if (!t->want) {
    t->stopped = true;
    return 0;
  }
  size_t total = size * n;
  for (size_t off = 0; off < total; off += CHUNK) {
    size_t len = total - off < CHUNK ? total - off : CHUNK;
    if (*t->on_data && !(*t->on_data)(buf + off, len)) {
      t->stopped = true;
      return 0;
    }
  }
  return total;
}

}

bool HostPlatform::http(const HttpRequest& req, const HeadFn& on_head, const DataFn& on_data, std::string& error) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    error = "couldn't start curl";
    return false;
  }
  Transfer t{curl, &on_head, &on_data, {}};
  curl_slist* headers = curl_slist_append(nullptr, "Expect:");
  for (const auto& h : req.headers) headers = curl_slist_append(headers, (h.name + ": " + h.value).c_str());
  curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)req.timeout_s);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, (long)CHUNK);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
  if (req.method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
  } else if (req.method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  }
  CURLcode rc = curl_easy_perform(curl);
  bool ok = rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && t.stopped);
  if (ok) {
    deliver_head(&t);
  } else {
    error = curl_easy_strerror(rc);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return ok;
}

double HostPlatform::now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

void HostPlatform::sleep(double seconds) {
  if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

void HostPlatform::log(const std::string& line) {
  std::printf("%s %s\n", clock_time(now()).c_str(), line.c_str());
  std::fflush(stdout);
}

bool HostPlatform::load_state(std::string& out) {
  std::ifstream in(state_path_, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool HostPlatform::save_state(const std::string& data) {
  std::string tmp = state_path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    if (!out) return false;
  }
  return std::rename(tmp.c_str(), state_path_.c_str()) == 0;
}

}
