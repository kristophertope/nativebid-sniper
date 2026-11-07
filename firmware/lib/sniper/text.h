#pragma once

#include <string>
#include <utility>
#include <vector>

namespace sniper {

using Fields = std::vector<std::pair<std::string, std::string>>;

bool is_space(char c);
std::string trim(const std::string& s);
std::string lower(std::string s);
bool contains(const std::string& haystack, const std::string& needle);
bool starts_with(const std::string& s, const std::string& prefix);
std::vector<std::string> split_ws(const std::string& s);

double money(const std::string& s);
double cents(double v);
std::string fmt(double v);
std::string duration(double seconds);
std::string clock_time(double t);
std::string when(double t);

std::string html_unescape(const std::string& s);
std::string html_escape(const std::string& s, bool quote = false);
std::string strip_tags(const std::string& s);
std::string js_unescape(const std::string& s);
std::string prefix(const std::string& s, size_t codepoints);

std::string url_encode(const std::string& s);
std::string form_encode(const Fields& fields);
std::string url_join(const std::string& base, const std::string& path);

}
