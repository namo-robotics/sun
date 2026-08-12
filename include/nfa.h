#pragma once

#include <cassert>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "position.h"

struct State {
  bool isAccepting = false;
  std::map<char, std::unordered_set<State*>>
      transitions;                                // char -> next states
  std::unordered_set<State*> epsilonTransitions;  // epsilon next states
  std::unordered_set<State*> anyCharTransitions;  // for '.' wildcard
  bool enterGroup = false;  // if true, entering this RegexCapture group when
                            // reaching this state
  bool exitGroup = false;   // if true, exiting this RegexCapture group when
                            // reaching this state
  int groupId = 0;
  std::string groupName = "";
  int groupNameNum = -1;  // groupName parsed as int (-1 when unnamed)
};
// A group match recorded during a scan. The matched text is NOT stored;
// callers slice it from their own buffer via [start.offset, end.offset).
struct RegexCapture {
  Position start;
  Position end;
  int groupIdx = -1;
  const std::string* groupName = nullptr;  // owned by the matching State
  int groupNameNum = -1;                   // groupName as int (-1 unnamed)

  int length() const { return end.offset - start.offset; }
};

class NFA {
 private:
  static constexpr int kAlphabet = 256;

  std::vector<std::unique_ptr<State>> allStates;
  Position position;

  // Compiled form (see compile()); rebuilt whenever the graph grows
  bool compiled_ = false;
  bool initialized_ = false;
  int numStates_ = 0;
  std::unordered_map<const State*, int> idOf_;
  std::vector<uint8_t> accepting_, enterGroup_, exitGroup_, reachNonEmpty_;
  std::vector<int> groupId_, groupNameNum_;
  std::vector<const std::string*> groupName_;
  std::vector<int> epsOff_, epsTargets_;    // epsilon closure per state
  std::vector<int> charOff_, charTargets_;  // (state, char) -> targets
  std::vector<int> anyOff_, anyTargets_;    // '.' transitions per state
  std::vector<int> startClosure_;
  std::vector<int> active_, next_;
  std::vector<uint32_t> mark_;
  uint32_t markGen_ = 0;
  bool canReachNonEmpty_ = false;

  // Capture slots indexed by groupId. A slot is live only when its
  // generation matches gen_; resetToPosition bumps gen_ so per-scan reset
  // is O(1) with no allocation. Only the longest match per group is kept
  // (later exits of a group within one scan always extend the same start).
  std::vector<RegexCapture> captureSlots_;
  std::vector<uint32_t> captureGen_;
  std::vector<Position> candidateStart_;
  std::vector<uint32_t> candidateGen_;
  uint32_t gen_ = 0;

  void ensureSlot(int g) {
    if (g >= static_cast<int>(captureSlots_.size())) {
      captureSlots_.resize(g + 1);
      captureGen_.resize(g + 1, 0);
      candidateStart_.resize(g + 1);
      candidateGen_.resize(g + 1, 0);
    }
  }

  State* createState() {
    compiled_ = false;
    allStates.emplace_back(std::make_unique<State>());
    return allStates.back().get();
  }

 public:
  State* startState;
  State* acceptingState;
  bool isAccepting = false;
  NFA() {
    startState = createState();
    acceptingState = createState();
    acceptingState->isAccepting = true;
    fullReset();
  }

  void resetToPosition(Position pos) {
    position = pos;
    ++gen_;                // invalidates capture/candidate slots in O(1)
    initialized_ = false;  // active set rebuilt lazily from startClosure_
  }

  void fullReset() { resetToPosition(Position()); }

  void acquireStatesFrom(NFA& other) {
    compiled_ = false;
    for (auto& state : other.allStates) {
      allStates.push_back(std::move(state));
    }
    other.allStates.clear();
  }

  static NFA createForEpsilon() {
    NFA nfa;
    nfa.startState->epsilonTransitions.insert(nfa.acceptingState);
    nfa.fullReset();
    return nfa;
  }

  static NFA createForChar(char c) {
    NFA nfa;
    nfa.startState->transitions[c].insert(nfa.acceptingState);
    nfa.fullReset();
    return nfa;
  }

  static NFA createForCharClass(const std::set<char>& charSet,
                                bool negated = false) {
    NFA nfa;
    if (negated) {
      for (char c = 0; c < 127; ++c)  // ASCII range
      {
        if (charSet.count(c) == 0) {
          nfa.startState->transitions[c].insert(nfa.acceptingState);
        }
      }
    } else {
      for (char c : charSet) {
        nfa.startState->transitions[c].insert(nfa.acceptingState);
      }
    }
    nfa.fullReset();
    return nfa;
  }

  static NFA createForCharClass(const std::string& chars,
                                bool negated = false) {
    std::set<char> set(chars.begin(), chars.end());
    return createForCharClass(set, negated);
  }

  static NFA createForUnion(NFA& n1, NFA& n2) {
    NFA nfa;
    n1.acceptingState->isAccepting = false;
    n2.acceptingState->isAccepting = false;
    nfa.startState->epsilonTransitions.insert(n1.startState);
    nfa.startState->epsilonTransitions.insert(n2.startState);
    n1.acceptingState->epsilonTransitions.insert(nfa.acceptingState);
    n2.acceptingState->epsilonTransitions.insert(nfa.acceptingState);
    nfa.acquireStatesFrom(n1);
    nfa.acquireStatesFrom(n2);
    nfa.fullReset();
    return nfa;
  }

  static NFA createForConcatenation(NFA& n1, NFA& n2) {
    NFA nfa;
    n1.acceptingState->epsilonTransitions.insert(n2.startState);
    n1.acceptingState->isAccepting = false;
    nfa.startState = n1.startState;
    nfa.acceptingState = n2.acceptingState;
    nfa.acquireStatesFrom(n1);
    nfa.acquireStatesFrom(n2);
    nfa.fullReset();
    return nfa;
  }

  static NFA createForKleeneStar(NFA& n) {
    NFA nfa;
    nfa.startState->epsilonTransitions.insert(n.startState);
    nfa.startState->epsilonTransitions.insert(nfa.acceptingState);
    n.acceptingState->epsilonTransitions.insert(n.startState);
    n.acceptingState->epsilonTransitions.insert(nfa.acceptingState);
    n.acceptingState->isAccepting = false;
    nfa.acquireStatesFrom(n);
    nfa.fullReset();
    return nfa;
  }

  static NFA createForPlus(NFA& n) {
    NFA star = createForKleeneStar(n);
    return createForConcatenation(n, star);
  }

  static NFA createForOptional(NFA& n) {
    NFA nfa;
    // Bypass: start -> accepting (zero times)
    nfa.startState->epsilonTransitions.insert(nfa.acceptingState);
    // Or go through the sub-NFA (one time)
    nfa.startState->epsilonTransitions.insert(n.startState);
    n.acceptingState->epsilonTransitions.insert(nfa.acceptingState);
    n.acceptingState->isAccepting = false;
    nfa.acquireStatesFrom(n);
    nfa.fullReset();
    return nfa;
  }

  static NFA createForAnyChar() {
    NFA nfa;
    nfa.startState->anyCharTransitions.insert(nfa.acceptingState);
    nfa.fullReset();
    return nfa;
  }


  // --- compiled execution -------------------------------------------------
  //
  // Scanning runs on dense arrays rather than the pointer graph: states get
  // integer ids, epsilon closures and per-character transitions are
  // precomputed once, and the active set is a vector deduplicated by
  // generation marks. Built lazily on first use and invalidated whenever the
  // graph grows (construction goes through the static factories).

  void compile() {
    numStates_ = static_cast<int>(allStates.size());
    idOf_.clear();
    idOf_.reserve(numStates_);
    for (int i = 0; i < numStates_; ++i) idOf_[allStates[i].get()] = i;

    accepting_.assign(numStates_, 0);
    enterGroup_.assign(numStates_, 0);
    exitGroup_.assign(numStates_, 0);
    groupId_.assign(numStates_, 0);
    groupName_.assign(numStates_, nullptr);
    groupNameNum_.assign(numStates_, -1);
    for (int i = 0; i < numStates_; ++i) {
      const State* st = allStates[i].get();
      accepting_[i] = st->isAccepting;
      enterGroup_[i] = st->enterGroup;
      exitGroup_[i] = st->exitGroup;
      groupId_[i] = st->groupId;
      groupName_[i] = &st->groupName;
      groupNameNum_[i] = st->groupNameNum;
      if (st->groupId >= 0) ensureSlot(st->groupId);
    }

    // Transitive epsilon closure per state, flattened (CSR)
    epsOff_.assign(numStates_ + 1, 0);
    epsTargets_.clear();
    {
      std::vector<uint32_t> mark(numStates_, 0);
      uint32_t m = 0;
      std::vector<int> stack;
      for (int i = 0; i < numStates_; ++i) {
        epsOff_[i] = static_cast<int>(epsTargets_.size());
        ++m;
        stack.clear();
        stack.push_back(i);
        mark[i] = m;
        epsTargets_.push_back(i);  // closure includes the state itself
        while (!stack.empty()) {
          int cur = stack.back();
          stack.pop_back();
          for (State* nx : allStates[cur]->epsilonTransitions) {
            int nid = idOf_[nx];
            if (mark[nid] != m) {
              mark[nid] = m;
              epsTargets_.push_back(nid);
              stack.push_back(nid);
            }
          }
        }
      }
      epsOff_[numStates_] = static_cast<int>(epsTargets_.size());
    }

    // Per-character transitions, indexed by (state, unsigned char) -> CSR
    charOff_.assign(static_cast<size_t>(numStates_) * kAlphabet + 1, -1);
    charTargets_.clear();
    anyOff_.assign(numStates_ + 1, 0);
    anyTargets_.clear();
    for (int i = 0; i < numStates_; ++i) {
      const State* st = allStates[i].get();
      for (const auto& [ch, targets] : st->transitions) {
        size_t slot = static_cast<size_t>(i) * kAlphabet +
                      static_cast<unsigned char>(ch);
        charOff_[slot] = static_cast<int>(charTargets_.size());
        charTargets_.push_back(static_cast<int>(targets.size()));
        for (State* t : targets) charTargets_.push_back(idOf_[t]);
      }
      anyOff_[i] = static_cast<int>(anyTargets_.size());
      for (State* t : st->anyCharTransitions) anyTargets_.push_back(idOf_[t]);
    }
    anyOff_[numStates_] = static_cast<int>(anyTargets_.size());

    computeReachability();

    // Start-state closure, reused by every resetToPosition
    startClosure_.clear();
    for (int i = epsOff_[idOf_[startState]]; i < epsOff_[idOf_[startState] + 1];
         ++i) {
      startClosure_.push_back(epsTargets_[i]);
    }

    mark_.assign(numStates_, 0);
    markGen_ = 0;
    active_.clear();
    next_.clear();
    compiled_ = true;
  }

  void ensureCompiled() {
    if (!compiled_) compile();
  }

  // Materialize the active set from the cached start closure
  void ensureActive() {
    ensureCompiled();
    if (initialized_) return;
    initialized_ = true;
    active_ = startClosure_;
    isAccepting = false;
    canReachNonEmpty_ = false;
    for (int id : active_) {
      if (accepting_[id]) isAccepting = true;
      if (reachNonEmpty_[id]) canReachNonEmpty_ = true;
    }
  }

 public:
  bool step(char c) {
    ensureActive();

    next_.clear();
    ++markGen_;

    // === NORMAL TRANSITIONS (+ group entry, before consuming the char) ===
    const size_t base = static_cast<size_t>(kAlphabet);
    const unsigned char uc = static_cast<unsigned char>(c);
    for (int id : active_) {
      if (enterGroup_[id]) {
        int g = groupId_[id];
        candidateStart_[g] = position;
        candidateGen_[g] = gen_;
      }

      int off = charOff_[static_cast<size_t>(id) * base + uc];
      if (off >= 0) {
        int count = charTargets_[off];
        for (int k = 1; k <= count; ++k) addWithClosure(charTargets_[off + k]);
      }
      for (int k = anyOff_[id]; k < anyOff_[id + 1]; ++k) {
        addWithClosure(anyTargets_[k]);
      }
    }

    active_.swap(next_);

    // update position
    position.offset += 1;
    if (c == '\n') {
      position.line += 1;
      position.column = 1;
    } else {
      position.column += 1;
    }

    // === EXITS, ACCEPTANCE AND REACHABILITY (single pass) ===
    bool nowAccepting = false;
    canReachNonEmpty_ = false;
    for (int id : active_) {
      if (accepting_[id]) nowAccepting = true;
      if (reachNonEmpty_[id]) canReachNonEmpty_ = true;

      if (exitGroup_[id]) {
        int g = groupId_[id];
        if (candidateGen_[g] == gen_) {
          RegexCapture& cap = captureSlots_[g];
          cap.start = candidateStart_[g];
          cap.end = position;
          cap.groupIdx = g;
          cap.groupName = groupName_[id];
          cap.groupNameNum = groupNameNum_[id];
          captureGen_[g] = gen_;
        }
      }
    }

    isAccepting = nowAccepting;
    return isAccepting;
  }

 private:
  // Add a state and its precomputed epsilon closure to the next active set
  void addWithClosure(int id) {
    for (int k = epsOff_[id]; k < epsOff_[id + 1]; ++k) {
      int t = epsTargets_[k];
      if (mark_[t] != markGen_) {
        mark_[t] = markGen_;
        next_.push_back(t);
      }
    }
  }

  // States that can reach an accepting state via a non-empty path
  void computeReachability() {
    std::vector<std::vector<int>> revEps(numStates_);
    std::vector<std::vector<int>> revOther(numStates_);
    for (int i = 0; i < numStates_; ++i) {
      const State* st = allStates[i].get();
      for (State* nx : st->epsilonTransitions) revEps[idOf_[nx]].push_back(i);
      for (const auto& [ch, targets] : st->transitions) {
        for (State* nx : targets) revOther[idOf_[nx]].push_back(i);
      }
      for (State* nx : st->anyCharTransitions) revOther[idOf_[nx]].push_back(i);
    }

    std::vector<uint8_t> canReach(numStates_, 0);
    std::vector<int> queue;
    for (int i = 0; i < numStates_; ++i) {
      if (accepting_[i]) {
        canReach[i] = 1;
        queue.push_back(i);
      }
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
      int cur = queue[qi];
      for (int prev : revEps[cur]) {
        if (!canReach[prev]) {
          canReach[prev] = 1;
          queue.push_back(prev);
        }
      }
      for (int prev : revOther[cur]) {
        if (!canReach[prev]) {
          canReach[prev] = 1;
          queue.push_back(prev);
        }
      }
    }

    // A state qualifies if some non-epsilon edge lands in canReach...
    reachNonEmpty_.assign(numStates_, 0);
    for (int i = 0; i < numStates_; ++i) {
      const State* st = allStates[i].get();
      bool ok = false;
      for (const auto& [ch, targets] : st->transitions) {
        for (State* nx : targets) {
          if (canReach[idOf_[nx]]) { ok = true; break; }
        }
        if (ok) break;
      }
      if (!ok) {
        for (State* nx : st->anyCharTransitions) {
          if (canReach[idOf_[nx]]) { ok = true; break; }
        }
      }
      reachNonEmpty_[i] = ok;
    }
    // ...or reaches such a state through epsilon edges
    bool changed = true;
    while (changed) {
      changed = false;
      for (int i = 0; i < numStates_; ++i) {
        if (reachNonEmpty_[i]) continue;
        for (State* nx : allStates[i]->epsilonTransitions) {
          if (reachNonEmpty_[idOf_[nx]]) {
            reachNonEmpty_[i] = 1;
            changed = true;
            break;
          }
        }
      }
    }
  }

 public:
  // Capture recorded for a group in the current scan, or nullptr.
  // Pointers stay valid until the next resetToPosition/fullReset/step.
  const RegexCapture* captureFor(int groupIdx) const {
    if (groupIdx < 0 || groupIdx >= static_cast<int>(captureSlots_.size()) ||
        captureGen_[groupIdx] != gen_) {
      return nullptr;
    }
    return &captureSlots_[groupIdx];
  }

  // Winning capture of the current scan: longest match among NAMED groups,
  // ties broken by lowest numeric group name (= token declaration order)
  const RegexCapture* bestCapture() const {
    const RegexCapture* best = nullptr;
    int bestLen = -1;
    int bestName = 0;
    for (size_t g = 0; g < captureSlots_.size(); ++g) {
      if (captureGen_[g] != gen_) continue;
      const RegexCapture& cap = captureSlots_[g];
      if (cap.groupNameNum < 0) continue;  // unnamed group
      int len = cap.length();
      if (len > bestLen || (len == bestLen && cap.groupNameNum < bestName)) {
        best = &cap;
        bestLen = len;
        bestName = cap.groupNameNum;
      }
    }
    return best;
  }

  void simulate(const std::string& input) {
    fullReset();
    ensureActive();
    for (char c : input) {
      step(c);
      if (active_.empty()) break;
    }
  }

  bool matches(const std::string& input) {
    simulate(input);
    return isAccepting;
  }

  bool canReachAcceptingWithNonEmptyInput() {
    ensureActive();
    return canReachNonEmpty_;
  }
};

// ------------------------------------------------------------------
// Escape-aware token character
// ------------------------------------------------------------------
struct TokenChar {
  char ch;
  bool escaped;

  bool isEnd() const { return ch == '\0'; }
};

class RegexParser {
 private:
  std::string regex;
  size_t pos = 0;
  int nextGroupId = 0;

  TokenChar peek() const {
    if (pos >= regex.size()) return {'\0', false};

    if (regex[pos] == '\\' && pos + 1 < regex.size()) {
      return {regex[pos + 1], true};
    }
    return {regex[pos], false};
  }

  TokenChar consume() {
    if (pos >= regex.size()) return {'\0', false};

    if (regex[pos] == '\\' && pos + 1 < regex.size()) {
      ++pos;  // skip '\'
      char escapedChar = regex[pos++];
      return {escapedChar, true};
    }

    return {regex[pos++], false};
  }

  // ------------------------------------------------------------------
  // Parse a character class like [a-zA-Z0-9] or [^...] with escapes
  // ------------------------------------------------------------------
  NFA parseCharClass() {
    TokenChar open = consume();
    if (open.ch != '[' || open.escaped)
      throw std::runtime_error("Expected unescaped '['");

    bool negated = false;
    TokenChar nextTc = peek();
    if (nextTc.ch == '^' && !nextTc.escaped) {
      consume();
      negated = true;
    }

    std::set<char> chars;
    char prev = '\0';
    bool inRange = false;

    while (true) {
      TokenChar tc = peek();
      if (tc.isEnd()) throw std::runtime_error("Unclosed character class");

      if (tc.ch == ']' && !tc.escaped) break;

      consume();  // consume the current character

      char c = tc.ch;

      if (inRange) {
        if (prev > c) throw std::runtime_error("Invalid range: start > end");
        for (char ch = prev; ch <= c; ++ch) chars.insert(ch);
        inRange = false;
      } else if (c == '-' && !tc.escaped && !peek().isEnd() &&
                 peek().ch != ']') {
        // Unescaped '-' in middle (not at end) starts a range
        inRange = true;
      } else {
        chars.insert(c);
        prev = c;
      }
    }

    if (inRange) throw std::runtime_error("Dangling '-' in character class");

    consume();  // consume ']'

    return NFA::createForCharClass(chars, negated);
  }

  // ------------------------------------------------------------------
  // Parse a single atom (literal char, ., (, [, escaped char)
  // ------------------------------------------------------------------
  NFA parseAtom() {
    TokenChar tc = peek();

    if (tc.isEnd()) throw std::runtime_error("Unexpected end of regex");

    if (tc.escaped) {
      // Any escaped character is treated literally
      consume();
      return NFA::createForChar(tc.ch);
    } else if (tc.ch == '[') {
      return parseCharClass();
    } else if (tc.ch == '(') {
      consume();  // consume '('

      std::string groupName;
      int groupId = nextGroupId++;

      // Check for named group: (?<name>
      TokenChar nextTc = peek();
      if (nextTc.ch == '?' && !nextTc.escaped) {
        consume();  // consume '?'
        TokenChar lt = consume();
        if (lt.ch != '<' || lt.escaped)
          throw std::runtime_error("Expected '<' after '(?' for named group");

        // Read name until '>'
        while (true) {
          TokenChar nameTc = peek();
          if (nameTc.isEnd())
            throw std::runtime_error("Unclosed named RegexCapture group");
          if (nameTc.ch == '>' && !nameTc.escaped) {
            consume();  // consume '>'
            break;
          }
          consume();
          if (nameTc.escaped || !std::isalnum(nameTc.ch) && nameTc.ch != '_')
            throw std::runtime_error("Invalid character in group name");
          groupName += nameTc.ch;
        }

        if (groupName.empty())
          throw std::runtime_error("Empty group name not allowed");
      }

      // Numeric form of the group name, precomputed so scanning never parses
      // it (the lexer names each token alternative with its TokenKind index)
      int groupNameNum = -1;
      if (!groupName.empty() &&
          groupName.find_first_not_of("0123456789") == std::string::npos) {
        groupNameNum = std::stoi(groupName);
      }

      // Create entry state for group
      NFA entryNFA;
      entryNFA.startState->enterGroup = true;
      entryNFA.startState->groupId = groupId;
      entryNFA.startState->groupName = groupName;
      entryNFA.startState->groupNameNum = groupNameNum;
      entryNFA.startState->epsilonTransitions.insert(entryNFA.acceptingState);

      // Parse subexpression
      NFA sub = parseUnion();

      // Create exit state for group
      NFA exitNFA;
      exitNFA.startState->exitGroup = true;
      exitNFA.startState->groupId = groupId;
      exitNFA.startState->groupName = groupName;
      exitNFA.startState->groupNameNum = groupNameNum;
      exitNFA.startState->epsilonTransitions.insert(exitNFA.acceptingState);

      // Chain: entry -> sub -> exit
      NFA temp = NFA::createForConcatenation(entryNFA, sub);
      NFA result = NFA::createForConcatenation(temp, exitNFA);

      // Consume closing ')'
      TokenChar close = consume();
      if (close.ch != ')' || close.escaped)
        throw std::runtime_error("Expected unescaped ')'");

      return result;
    }

    else if (tc.ch == '.') {
      consume();
      return NFA::createForAnyChar();
    } else if (tc.ch == '*' || tc.ch == '+' || tc.ch == '|' || tc.ch == ')') {
      // Unescaped special chars not allowed as atoms
      throw std::runtime_error(std::string("Unexpected character: ") + tc.ch);
    } else {
      // Any other unescaped character is literal
      consume();
      return NFA::createForChar(tc.ch);
    }
  }

  // Handles *, +
  NFA parsePostfix() {
    NFA n = parseAtom();

    while (true) {
      TokenChar tc = peek();

      if (tc.ch == '*' && !tc.escaped) {
        consume();
        n = NFA::createForKleeneStar(n);
      } else if (tc.ch == '+' && !tc.escaped) {
        consume();
        n = NFA::createForPlus(n);
      } else if (tc.ch == '?' && !tc.escaped)  // <-- NEW: handle ?
      {
        consume();
        n = NFA::createForOptional(n);
      } else {
        break;
      }
    }
    return n;
  }

  NFA parseConcatenation() {
    NFA n = parsePostfix();

    while (true) {
      TokenChar tc = peek();
      // Continue concatenating if next is a valid atom start
      if (tc.isEnd() || (tc.ch == ')' && !tc.escaped) ||
          (tc.ch == '|' && !tc.escaped) || (tc.ch == ']' && !tc.escaped)) {
        break;
      }

      NFA next = parsePostfix();
      n = NFA::createForConcatenation(n, next);
    }
    return n;
  }

  NFA parseUnion() {
    NFA n = parseConcatenation();

    while (true) {
      TokenChar tc = peek();
      if (tc.ch == '|' && !tc.escaped) {
        consume();
        NFA next = parseConcatenation();
        n = NFA::createForUnion(n, next);
      } else {
        break;
      }
    }
    return n;
  }

 public:
  NFA parse(const std::string& r) {
    regex = r;
    pos = 0;
    NFA nfa = parseUnion();

    if (peek().ch != '\0')
      throw std::runtime_error("Extra characters at end of regex");

    return nfa;
  }
};