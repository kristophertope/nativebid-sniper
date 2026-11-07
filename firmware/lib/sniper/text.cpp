#include "text.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace sniper {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) a++;
  while (b > a && is_space(s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && is_space(s[i])) i++;
    size_t j = i;
    while (j < s.size() && !is_space(s[j])) j++;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

double money(const std::string& s) {
  std::string t;
  for (char c : s) {
    if (c != ',' && c != '$') t += c;
  }
  t = trim(t);
  if (t.empty()) return NAN;
  char* end = nullptr;
  double v = std::strtod(t.c_str(), &end);
  if (*end || !std::isfinite(v)) return NAN;
  return v;
}

double cents(double v) {
  return std::round(v * 100.0) / 100.0;
}

std::string fmt(double v) {
  if (std::isnan(v)) return "\xE2\x80\x94";
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.2f", std::fabs(v));
  std::string s(buf);
  size_t dot = s.find('.');
  std::string whole = s.substr(0, dot), out;
  for (size_t i = 0; i < whole.size(); i++) {
    if (i && (whole.size() - i) % 3 == 0) out += ',';
    out += whole[i];
  }
  return std::string(v < 0 ? "-$" : "$") + out + s.substr(dot);
}

std::string duration(double seconds) {
  long s = seconds > 0 ? (long)seconds : 0;
  long d = s / 86400;
  s %= 86400;
  long h = s / 3600;
  s %= 3600;
  long m = s / 60;
  s %= 60;
  std::vector<std::string> parts;
  if (d) parts.push_back(std::to_string(d) + "d");
  if (h) parts.push_back(std::to_string(h) + "h");
  if (m) parts.push_back(std::to_string(m) + "m");
  parts.push_back(std::to_string(s) + "s");
  return parts.size() > 1 ? parts[0] + " " + parts[1] : parts[0];
}

static struct tm local(double t) {
  time_t tt = (time_t)t;
  struct tm tm {};
  localtime_r(&tt, &tm);
  return tm;
}

std::string clock_time(double t) {
  struct tm tm = local(t);
  char buf[16];
  std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
  return buf;
}

std::string when(double t) {
  struct tm tm = local(t);
  char day[16], buf[64];
  std::strftime(day, sizeof day, "%a %b", &tm);
  int h = tm.tm_hour % 12;
  if (!h) h = 12;
  std::snprintf(buf, sizeof buf, "%s %d, %d:%02d:%02d %s", day, tm.tm_mday, h, tm.tm_min, tm.tm_sec,
                tm.tm_hour < 12 ? "AM" : "PM");
  return buf;
}

static void append_utf8(std::string& out, long cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

static long entity(const std::string& e) {
  if (e.size() > 1 && e[0] == '#') {
    bool hex = e[1] == 'x' || e[1] == 'X';
    std::string digits = e.substr(hex ? 2 : 1);
    if (digits.empty()) return -1;
    char* end = nullptr;
    long v = std::strtol(digits.c_str(), &end, hex ? 16 : 10);
    return *end ? -1 : v;
  }
  if (e == "amp") return '&';
  if (e == "lt") return '<';
  if (e == "gt") return '>';
  if (e == "quot") return '"';
  if (e == "apos") return '\'';
  if (e == "nbsp") return 0xA0;
  return -1;
}

std::string html_unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    size_t semi = s.find(';', i);
    long cp = semi == std::string::npos || semi - i > 10 ? -1 : entity(s.substr(i + 1, semi - i - 1));
    if (cp <= 0 || cp > 0x10FFFF) {
      out += '&';
      continue;
    }
    append_utf8(out, cp);
    i = semi;
  }
  return out;
}

std::string html_escape(const std::string& s, bool quote) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (quote && c == '"') out += "&quot;";
    else if (quote && c == '\'') out += "&#x27;";
    else out += c;
  }
  return out;
}

std::string strip_tags(const std::string& s) {
  std::string untagged;
  for (size_t i = 0; i < s.size(); i++) {
    size_t close = s[i] == '<' ? s.find('>', i + 1) : std::string::npos;
    if (close != std::string::npos && close > i + 1) {
      untagged += ' ';
      i = close;
    } else {
      untagged += s[i];
    }
  }
  std::string text = html_unescape(untagged), out;
  bool gap = false;
  for (size_t i = 0; i < text.size(); i++) {
    bool nbsp = (unsigned char)text[i] == 0xC2 && i + 1 < text.size() && (unsigned char)text[i + 1] == 0xA0;
    if (is_space(text[i]) || nbsp) {
      gap = true;
      if (nbsp) i++;
      continue;
    }
    if (gap && !out.empty()) out += ' ';
    gap = false;
    out += text[i];
  }
  return out;
}

std::string js_unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) i++;
    out += s[i];
  }
  return out;
}

std::string prefix(const std::string& s, size_t codepoints) {
  size_t i = 0, n = 0;
  while (i < s.size() && n < codepoints) {
    unsigned char c = s[i];
    i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    n++;
  }
  return s.substr(0, i < s.size() ? i : s.size());
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '_' || c == '.' || c == '-' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string form_encode(const Fields& fields) {
  std::string out;
  for (const auto& f : fields) {
    if (!out.empty()) out += '&';
    out += url_encode(f.first) + "=" + url_encode(f.second);
  }
  return out;
}

static std::string origin(const std::string& url) {
  size_t scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  size_t slash = url.find('/', scheme + 3);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string url_join(const std::string& base, const std::string& path) {
  if (starts_with(path, "http://") || starts_with(path, "https://")) return path;
  if (path.empty()) return base;
  if (starts_with(path, "//")) return base.substr(0, base.find("://") + 1) + path;
  if (path[0] == '/') return origin(base) + path;
  std::string b = base.substr(0, base.find_first_of("?#"));
  size_t slash = b.rfind('/');
  size_t scheme = b.find("://");
  if (slash == std::string::npos || (scheme != std::string::npos && slash < scheme + 3)) {
    return origin(base) + "/" + path;
  }
  return b.substr(0, slash + 1) + path;
}

}
