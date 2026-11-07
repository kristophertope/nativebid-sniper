#pragma once

#include <map>
#include <string>
#include <vector>

namespace sniper {

constexpr size_t CHUNK = 1024;
constexpr size_t WINDOW = 2048;

struct Token {
  enum Kind { Lit, Ws, Nbsp, SkipNot, SkipAny, Opt, CapUntil, CapDigits, CapNumber, CapWord, CapJs, CapLazy };
  Kind kind;
  std::string text;
  char c;
};

struct Pattern {
  std::vector<Token> tokens;
};

Pattern compile(const char* spec);
std::vector<Pattern> compile_all(const std::vector<const char*>& specs);

struct Match {
  size_t start = 0;
  size_t end = 0;
  std::vector<std::string> caps;
};

enum class Outcome { Fail, NeedMore, Matched };

Outcome match_at(const Pattern& p, const std::string& buf, size_t pos, Match& m);

class Scanner {
 public:
  virtual ~Scanner() = default;
  void feed(const char* data, size_t len);
  virtual bool done() const;
  bool has(int id) const { return found_.count(id) > 0; }
  std::string get(int id) const;
  size_t bytes() const { return bytes_; }
  size_t peak() const { return peak_; }

 protected:
  void rule(int id, const std::vector<Pattern>& alts, bool many = false);
  virtual void hit(int id, const Match& m, size_t at, const std::string& buf);
  std::map<int, std::string> found_;
  std::vector<int> needed_;

 private:
  struct Rule {
    int id;
    const std::vector<Pattern>* alts;
    bool many;
    size_t pos;
    size_t anchor;
  };
  std::vector<Rule> rules_;
  std::string buf_;
  size_t base_ = 0;
  size_t bytes_ = 0;
  size_t peak_ = 0;
};

}
