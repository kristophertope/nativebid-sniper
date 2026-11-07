#include "web.h"

#include <cstdlib>
#include <ctime>

#include "scanner.h"
#include "text.h"

namespace sniper {

static const char* UA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";

static bool redirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

Reply Web::request(std::string method, std::string url, std::string body, const std::vector<Header>& headers,
                   int timeout, bool follow, const std::function<bool(const Reply&)>& want_body,
                   const DataFn& sink) {
  Reply reply;
  for (int hop = 0; hop < 6; hop++) {
    HttpRequest req;
    req.method = method;
    req.url = url;
    req.body = body;
    req.timeout_s = timeout;
    req.headers.push_back({"User-Agent", UA});
    for (const auto& h : headers) req.headers.push_back(h);
    std::string cookie = cookies_ ? cookie_header() : "";
    if (!cookie.empty()) req.headers.push_back({"Cookie", cookie});

    reply = Reply();
    reply.url = url;
    bool hopping = false;
    auto on_head = [&](const HttpHead& head) {
      reply.status = head.status;
      for (const auto& h : head.headers) {
        std::string name = lower(h.name);
        if (name == "location") reply.location = trim(h.value);
        if (name == "set-cookie" && cookies_) store_cookie(h.value);
      }
      hopping = follow && redirect(head.status) && !reply.location.empty();
      return !hopping && (!want_body || want_body(reply));
    };
    std::string error;
    if (!p_.http(req, on_head, sink, error)) {
      reply.error = error.empty() ? "connection failed" : error;
      return reply;
    }
    reply.ok = true;
    if (!hopping) return reply;
    url = url_join(url, reply.location);
    if (reply.status == 303 || ((reply.status == 301 || reply.status == 302) && method == "POST")) {
      method = "GET";
      body.clear();
    }
  }
  reply.ok = false;
  reply.error = "too many redirects";
  return reply;
}

Reply Web::scan(const std::string& url, Scanner& page, int timeout) {
  return request("GET", url, "", {}, timeout, true, nullptr, [&](const char* d, size_t n) {
    page.feed(d, n);
    return !page.done();
  });
}

Reply Web::text(const std::string& method, const std::string& url, const std::string& body,
                const std::vector<Header>& headers, std::string& out, size_t limit, int timeout) {
  out.clear();
  return request(method, url, body, headers, timeout, true, nullptr, [&](const char* d, size_t n) {
    size_t room = limit > out.size() ? limit - out.size() : 0;
    out.append(d, n < room ? n : room);
    return out.size() < limit;
  });
}

void Web::store_cookie(const std::string& header) {
  size_t semi = header.find(';');
  std::string pair = header.substr(0, semi);
  size_t eq = pair.find('=');
  if (eq == std::string::npos) return;
  std::string name = trim(pair.substr(0, eq)), value = trim(pair.substr(eq + 1));
  if (name.empty()) return;
  bool expired = value.empty();
  std::string attrs = lower(semi == std::string::npos ? "" : header.substr(semi));
  size_t age = attrs.find("max-age=");
  if (age != std::string::npos && std::atol(attrs.c_str() + age + 8) <= 0) expired = true;
  size_t exp = attrs.find("expires=");
  if (exp != std::string::npos) {
    time_t now = (time_t)p_.now();
    struct tm tm {};
    gmtime_r(&now, &tm);
    for (size_t i = exp + 8; i + 4 <= attrs.size() && attrs[i] != ';'; i++) {
      if (isdigit((unsigned char)attrs[i]) && i + 4 <= attrs.size() && isdigit((unsigned char)attrs[i + 1]) &&
          isdigit((unsigned char)attrs[i + 2]) && isdigit((unsigned char)attrs[i + 3])) {
        if (std::atoi(attrs.substr(i, 4).c_str()) < tm.tm_year + 1900) expired = true;
        break;
      }
    }
  }
  if (expired) {
    jar_.erase(name);
  } else {
    jar_[name] = value;
  }
}

std::string Web::cookie_header() const {
  std::string out;
  for (const auto& c : jar_) {
    if (!out.empty()) out += "; ";
    out += c.first + "=" + c.second;
  }
  return out;
}

}
