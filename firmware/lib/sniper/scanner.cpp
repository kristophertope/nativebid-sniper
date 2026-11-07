#include "scanner.h"

#include <algorithm>
#include <cctype>

#include "text.h"

namespace sniper {

Pattern compile(const char* s) {
  Pattern p;
  auto add = [&](Token::Kind k, char c = 0) { p.tokens.push_back(Token{k, "", c}); };
  auto lit = [&](char c) {
    if (p.tokens.empty() || p.tokens.back().kind != Token::Lit) add(Token::Lit);
    p.tokens.back().text += c;
  };
  for (; *s; s++) {
    if (*s != '%') {
      lit(*s);
      continue;
    }
    char k = *++s;
    switch (k) {
      case 's': add(Token::Ws); break;
      case 'b': add(Token::Nbsp); break;
      case '.': add(Token::SkipAny); break;
      case '*': add(Token::SkipNot, *++s); break;
      case '?': add(Token::Opt, *++s); break;
      case '^': add(Token::CapUntil, *++s); break;
      case 'd': add(Token::CapDigits); break;
      case 'n': add(Token::CapNumber); break;
      case 'w': add(Token::CapWord); break;
      case 'q': add(Token::CapJs); break;
      case 'c': add(Token::CapLazy); break;
      default: lit(k); break;
    }
  }
  return p;
}

static bool in_class(Token::Kind k, char ch) {
  unsigned char c = ch;
  if (k == Token::CapDigits) return std::isdigit(c);
  if (k == Token::CapNumber) return std::isdigit(c) || c == ',' || c == '.';
  return std::isalnum(c) || c == '_';
}

static Outcome run(const std::vector<Token>& t, size_t i, const std::string& b, size_t p,
                   std::vector<std::string>& caps, size_t& end) {
  if (i == t.size()) {
    end = p;
    return Outcome::Matched;
  }
  const Token& k = t[i];
  const size_t n = b.size();
  const size_t mark = caps.size();
  auto then = [&](size_t q) {
    Outcome o = run(t, i + 1, b, q, caps, end);
    if (o != Outcome::Matched) caps.resize(mark);
    return o;
  };
  switch (k.kind) {
    case Token::Lit:
      for (size_t j = 0; j < k.text.size(); j++) {
        if (p + j >= n) return Outcome::NeedMore;
        if (b[p + j] != k.text[j]) return Outcome::Fail;
      }
      return then(p + k.text.size());
    case Token::Ws:
      while (p < n && is_space(b[p])) p++;
      if (p >= n) return Outcome::NeedMore;
      return then(p);
    case Token::Nbsp: {
      static const std::string nbsp = "&nbsp;";
      for (;;) {
        if (p >= n) return Outcome::NeedMore;
        if (is_space(b[p])) {
          p++;
        } else if (b.compare(p, nbsp.size(), nbsp) == 0) {
          p += nbsp.size();
        } else if (n - p < nbsp.size() && nbsp.compare(0, n - p, b, p, n - p) == 0) {
          return Outcome::NeedMore;
        } else {
          break;
        }
      }
      return then(p);
    }
    case Token::SkipNot:
    case Token::SkipAny:
      for (size_t q = p;; q++) {
        Outcome o = then(q);
        if (o != Outcome::Fail) return o;
        if (q >= n) return Outcome::NeedMore;
        if (k.kind == Token::SkipNot && b[q] == k.c) return Outcome::Fail;
      }
    case Token::Opt:
      if (p >= n) return Outcome::NeedMore;
      if (b[p] == k.c) {
        Outcome o = then(p + 1);
        if (o != Outcome::Fail) return o;
      }
      return then(p);
    case Token::CapUntil: {
      size_t q = p;
      while (q < n && b[q] != k.c) q++;
      if (q >= n) return Outcome::NeedMore;
      caps.push_back(b.substr(p, q - p));
      return then(q);
    }
    case Token::CapDigits:
    case Token::CapNumber:
    case Token::CapWord: {
      size_t q = p;
      while (q < n && in_class(k.kind, b[q])) q++;
      if (q >= n) return Outcome::NeedMore;
      if (q == p) return Outcome::Fail;
      caps.push_back(b.substr(p, q - p));
      return then(q);
    }
    case Token::CapJs: {
      size_t q = p;
      while (q < n && b[q] != '"') q += b[q] == '\\' ? 2 : 1;
      if (q >= n) return Outcome::NeedMore;
      caps.push_back(b.substr(p, q - p));
      return then(q);
    }
    case Token::CapLazy:
      for (size_t q = p;; q++) {
        caps.push_back(std::string());
        Outcome o = run(t, i + 1, b, q, caps, end);
        if (o == Outcome::Matched) {
          caps[mark] = b.substr(p, q - p);
          return o;
        }
        caps.resize(mark);
        if (o == Outcome::NeedMore || q >= n) return Outcome::NeedMore;
      }
  }
  return Outcome::Fail;
}

Outcome match_at(const Pattern& p, const std::string& buf, size_t pos, Match& m) {
  m.caps.clear();
  m.start = pos;
  return run(p.tokens, 0, buf, pos, m.caps, m.end);
}

static Outcome first(const Pattern& p, const std::string& buf, size_t from, Match& m) {
  const std::string& anchor = p.tokens[0].text;
  for (size_t i = from;;) {
    size_t at = buf.find(anchor, i);
    if (at == std::string::npos) return Outcome::Fail;
    Outcome o = match_at(p, buf, at, m);
    if (o != Outcome::Fail) return o;
    i = at + 1;
  }
}

std::vector<Pattern> compile_all(const std::vector<const char*>& specs) {
  std::vector<Pattern> out;
  for (const char* s : specs) out.push_back(compile(s));
  return out;
}

void Scanner::rule(int id, const std::vector<Pattern>& alts, bool many) {
  Rule r{id, &alts, many, 0, 0};
  for (const auto& a : alts) r.anchor = std::max(r.anchor, a.tokens[0].text.size());
  rules_.push_back(r);
}

bool Scanner::done() const {
  for (int id : needed_) {
    if (!found_.count(id)) return false;
  }
  return true;
}

std::string Scanner::get(int id) const {
  auto it = found_.find(id);
  return it == found_.end() ? std::string() : it->second;
}

void Scanner::hit(int id, const Match& m, size_t, const std::string& buf) {
  found_[id] = m.caps.empty() ? buf.substr(m.start, m.end - m.start) : m.caps[0];
}

void Scanner::feed(const char* data, size_t len) {
  bytes_ += len;
  buf_.append(data, len);
  if (buf_.size() > peak_) peak_ = buf_.size();
  for (auto& r : rules_) {
    if (!r.many && found_.count(r.id)) continue;
    for (;;) {
      size_t from = r.pos > base_ ? r.pos - base_ : 0;
      Outcome best = Outcome::Fail;
      Match bm;
      for (const auto& alt : *r.alts) {
        Match m;
        Outcome o = first(alt, buf_, from, m);
        if (o != Outcome::Fail && (best == Outcome::Fail || m.start < bm.start)) {
          best = o;
          bm = std::move(m);
        }
      }
      if (best == Outcome::Fail) {
        size_t tail = buf_.size() >= r.anchor ? buf_.size() - r.anchor + 1 : 0;
        r.pos = base_ + std::max(from, tail);
        break;
      }
      if (best == Outcome::NeedMore) {
        r.pos = base_ + bm.start;
        break;
      }
      r.pos = base_ + bm.end;
      hit(r.id, bm, base_ + bm.start, buf_);
      if (!r.many) break;
    }
  }
  if (buf_.size() > WINDOW) {
    size_t cut = buf_.size() - WINDOW;
    buf_.erase(0, cut);
    base_ += cut;
  }
}

}
