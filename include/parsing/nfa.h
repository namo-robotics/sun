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

#include "support/position.h"

struct State {
  bool isAccepting = false;
  std::map<unsigned char, std::unordered_set<State*>>
      transitions;                                // byte -> next states
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

class DFA;

class NFA {
 private:
  // The DFA reads the compiled arrays below to run lazy subset construction.
  friend class DFA;

  static constexpr int kAlphabet = 256;

  std::vector<std::unique_ptr<State>> allStates;

  // Compiled form (see compile()); rebuilt whenever the graph grows
  bool compiled_ = false;
  int numStates_ = 0;
  std::unordered_map<const State*, int> idOf_;
  std::vector<uint8_t> accepting_, enterGroup_, exitGroup_, reachNonEmpty_;
  std::vector<int> groupId_, groupNameNum_;
  std::vector<const std::string*> groupName_;
  std::vector<int> epsOff_, epsTargets_;    // epsilon closure per state
  std::vector<int> charOff_, charTargets_;  // (state, char) -> targets
  std::vector<int> anyOff_, anyTargets_;    // '.' transitions per state
  std::vector<int> startClosure_;
  int maxGroupId_ = 0;  // highest group id; sizes the DFA's capture slots

  State* createState() {
    compiled_ = false;
    allStates.emplace_back(std::make_unique<State>());
    return allStates.back().get();
  }

 public:
  State* startState;
  State* acceptingState;
  NFA() {
    startState = createState();
    acceptingState = createState();
    acceptingState->isAccepting = true;
  }

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
    return nfa;
  }

  static NFA createForChar(char c) {
    NFA nfa;
    nfa.startState->transitions[static_cast<unsigned char>(c)].insert(
        nfa.acceptingState);
    return nfa;
  }

  // Negation spans the full 0-255 byte range, so [^"\\] and friends accept
  // UTF-8 continuation bytes inside string literals and comments.
  static NFA createForCharClass(const std::set<unsigned char>& charSet,
                                bool negated = false) {
    NFA nfa;
    if (negated) {
      for (int c = 0; c < kAlphabet; ++c) {
        if (charSet.count(static_cast<unsigned char>(c)) == 0) {
          nfa.startState->transitions[static_cast<unsigned char>(c)].insert(
              nfa.acceptingState);
        }
      }
    } else {
      for (unsigned char c : charSet) {
        nfa.startState->transitions[c].insert(nfa.acceptingState);
      }
    }
    return nfa;
  }

  static NFA createForCharClass(const std::string& chars,
                                bool negated = false) {
    std::set<unsigned char> set(chars.begin(), chars.end());
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
    return nfa;
  }

  static NFA createForAnyChar() {
    NFA nfa;
    nfa.startState->anyCharTransitions.insert(nfa.acceptingState);
    return nfa;
  }


  // --- compiled form ------------------------------------------------------
  //
  // Flattens the pointer graph into dense arrays for the DFA to determinize:
  // states get integer ids, and epsilon closures (transitive, including the
  // state itself) and per-byte transitions are precomputed as CSR tables.
  // Built lazily on first use and invalidated whenever the graph grows
  // (construction goes through the static factories).

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
      if (st->groupId >= 0) maxGroupId_ = std::max(maxGroupId_, st->groupId);
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

    // Epsilon closure of the start state; seeds the DFA's start state
    startClosure_.clear();
    for (int i = epsOff_[idOf_[startState]]; i < epsOff_[idOf_[startState] + 1];
         ++i) {
      startClosure_.push_back(epsTargets_[i]);
    }

    compiled_ = true;
  }

  void ensureCompiled() {
    if (!compiled_) compile();
  }

 private:
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

};

// ------------------------------------------------------------------
// Lazily determinized NFA (subset construction with a transition cache)
// ------------------------------------------------------------------
//
// A DFA state is a set of NFA states. Transitions are materialized the first
// time they are taken and then cached, so a scan costs one array load per
// input byte instead of a walk over the NFA's active set. The state count is
// bounded by the regex, not by the input.
//
// Captures survive determinization because the NFA's capture bookkeeping only
// ever depended on *which states are active*, not on the path taken: a group's
// candidate start is written by any active enterGroup state and its end is
// committed by any active exitGroup state (both single global slots per group
// id). Those are properties of the state set, so they are precomputed per DFA
// state. This reproduces the NFA's semantics exactly, including its
// "last writer wins per group id" approximation. Path-accurate captures would
// need a tagged DFA with per-transition register copies; that is not built
// here.
//
// The Lexer bypasses captures entirely and uses acceptKind()/step() directly.
class DFA {
 public:
  static constexpr int kAlphabet = 256;
  static constexpr int32_t kDead = 0;        // reserved: the empty state set
  static constexpr int32_t kUncomputed = -1;  // sentinel inside trans_
  static constexpr int32_t kNoAccept = -1;    // acceptKind_ of a non-accepting

 private:
  struct SetHash {
    size_t operator()(const std::vector<int32_t>& v) const noexcept {
      size_t h = 1469598103934665603ull;  // FNV-1a over the state ids
      for (int32_t x : v) {
        h ^= static_cast<size_t>(static_cast<uint32_t>(x));
        h *= 1099511628211ull;
      }
      return h;
    }
  };

  struct ExitInfo {
    int32_t groupId;
    const std::string* groupName;
    int32_t groupNameNum;
  };

  NFA nfa_;  // owned: drives lazy expansion, and owns the group-name strings
  int32_t start_ = kDead;

  // Lazily grown cache. trans_ is numStates * kAlphabet.
  std::vector<int32_t> trans_;
  std::vector<std::vector<int32_t>> sets_;  // sorted NFA ids per DFA state
  std::unordered_map<std::vector<int32_t>, int32_t, SetHash> interner_;

  // Per-state properties, derived once when the state is created
  std::vector<uint8_t> accepting_, canExtend_;
  std::vector<int32_t> acceptKind_;  // min groupNameNum over exitGroup states
  std::vector<int32_t> enterOff_, enterPool_;  // CSR, capture path only
  std::vector<int32_t> exitOff_;
  std::vector<ExitInfo> exitPool_;

  // Scratch for materialize(); never touched by the scanning API
  std::vector<uint32_t> mark_;
  uint32_t markGen_ = 0;
  std::vector<int32_t> scratch_;
  long long misses_ = 0;

  // Embedded capture scanner. The Lexer never touches any of this; it exists
  // for the regex-level API (matches/step/captureFor/bestCapture).
  int32_t cur_ = kDead;
  Position position_;
  std::vector<RegexCapture> captureSlots_;
  std::vector<uint32_t> captureGen_;
  std::vector<Position> candidateStart_;
  std::vector<uint32_t> candidateGen_;
  uint32_t gen_ = 0;

  void addClosure(int32_t id) {
    for (int k = nfa_.epsOff_[id]; k < nfa_.epsOff_[id + 1]; ++k) {
      int t = nfa_.epsTargets_[k];
      if (mark_[t] != markGen_) {
        mark_[t] = markGen_;
        scratch_.push_back(t);
      }
    }
  }

  // Look up a sorted state set, creating the DFA state if it is new
  int32_t intern(const std::vector<int32_t>& set) {
    if (auto it = interner_.find(set); it != interner_.end()) return it->second;

    int32_t id = static_cast<int32_t>(sets_.size());
    sets_.push_back(set);
    interner_.emplace(set, id);
    trans_.resize(static_cast<size_t>(sets_.size()) * kAlphabet, kUncomputed);

    uint8_t acc = 0, ext = 0;
    int32_t kind = kNoAccept;
    for (int32_t s : set) {
      if (nfa_.accepting_[s]) acc = 1;
      if (nfa_.reachNonEmpty_[s]) ext = 1;
      if (nfa_.enterGroup_[s]) enterPool_.push_back(nfa_.groupId_[s]);
      if (nfa_.exitGroup_[s]) {
        exitPool_.push_back(
            {nfa_.groupId_[s], nfa_.groupName_[s], nfa_.groupNameNum_[s]});
        // Lowest declaration index wins ties, matching bestCapture()
        int32_t n = nfa_.groupNameNum_[s];
        if (n >= 0 && (kind == kNoAccept || n < kind)) kind = n;
      }
    }
    enterOff_.push_back(static_cast<int32_t>(enterPool_.size()));
    exitOff_.push_back(static_cast<int32_t>(exitPool_.size()));
    accepting_.push_back(acc);
    canExtend_.push_back(ext);
    acceptKind_.push_back(kind);
    assert(trans_.size() == sets_.size() * kAlphabet);
    return id;
  }

  // Compute and cache the transition out of `from` on byte `c`
  int32_t materialize(int32_t from, unsigned char c) {
    ++misses_;
    scratch_.clear();
    ++markGen_;
    for (int32_t s : sets_[from]) {
      int off = nfa_.charOff_[static_cast<size_t>(s) * kAlphabet + c];
      if (off >= 0) {
        int count = nfa_.charTargets_[off];
        for (int k = 1; k <= count; ++k) addClosure(nfa_.charTargets_[off + k]);
      }
      // '.' matches every byte, so it feeds every column of the table
      for (int k = nfa_.anyOff_[s]; k < nfa_.anyOff_[s + 1]; ++k) {
        addClosure(nfa_.anyTargets_[k]);
      }
    }
    std::sort(scratch_.begin(), scratch_.end());

    // intern() resizes trans_, invalidating any pointer into it. Never cache
    // a row pointer across this call; index trans_ only after it returns.
    int32_t to = intern(scratch_);
    trans_[static_cast<size_t>(from) * kAlphabet + c] = to;
    return to;
  }

 public:
  bool isAccepting = false;

  explicit DFA(NFA nfa) : nfa_(std::move(nfa)) {
    nfa_.ensureCompiled();
    mark_.assign(nfa_.numStates_, 0);
    enterOff_.push_back(0);
    exitOff_.push_back(0);

    intern({});  // id 0 is the dead state: an empty set of NFA states
    std::fill(trans_.begin(), trans_.begin() + kAlphabet, kDead);

    std::vector<int32_t> s0(nfa_.startClosure_.begin(),
                            nfa_.startClosure_.end());
    std::sort(s0.begin(), s0.end());
    start_ = intern(s0);

    const size_t slots = static_cast<size_t>(nfa_.maxGroupId_) + 1;
    captureSlots_.resize(slots);
    captureGen_.assign(slots, 0);
    candidateStart_.resize(slots);
    candidateGen_.assign(slots, 0);

    fullReset();
  }

  // --- driver API (the Lexer uses only these) -----------------------------

  int32_t startState() const { return start_; }
  static bool isDead(int32_t s) { return s == kDead; }

  int32_t step(int32_t s, unsigned char c) {
    int32_t t = trans_[static_cast<size_t>(s) * kAlphabet + c];
    return t >= 0 ? t : materialize(s, c);  // negative == not yet computed
  }

  // Token kind of the alternative accepted in this state, or kNoAccept
  int32_t acceptKind(int32_t s) const { return acceptKind_[s]; }
  bool acceptingState(int32_t s) const { return accepting_[s] != 0; }

  int stateCount() const { return static_cast<int>(sets_.size()); }
  long long transitionMisses() const { return misses_; }

  // --- capture-tracking scanner (regex-level API) -------------------------

  void resetToPosition(Position pos) {
    position_ = pos;
    ++gen_;  // invalidates capture/candidate slots in O(1)
    cur_ = start_;
    isAccepting = accepting_[cur_] != 0;
  }

  void fullReset() { resetToPosition(Position()); }

  bool step(char c) {
    // Group entry is read off the pre-transition state at the pre-transition
    // position, exactly as NFA::step did.
    for (int k = enterOff_[cur_]; k < enterOff_[cur_ + 1]; ++k) {
      int g = enterPool_[k];
      candidateStart_[g] = position_;
      candidateGen_[g] = gen_;
    }

    cur_ = step(cur_, static_cast<unsigned char>(c));

    position_.offset += 1;
    if (c == '\n') {
      position_.line += 1;
      position_.column = 1;
    } else {
      position_.column += 1;
    }

    // ...and group exit off the post-transition state at the new position
    for (int k = exitOff_[cur_]; k < exitOff_[cur_ + 1]; ++k) {
      const ExitInfo& ex = exitPool_[k];
      if (candidateGen_[ex.groupId] == gen_) {
        RegexCapture& cap = captureSlots_[ex.groupId];
        cap.start = candidateStart_[ex.groupId];
        cap.end = position_;
        cap.groupIdx = ex.groupId;
        cap.groupName = ex.groupName;
        cap.groupNameNum = ex.groupNameNum;
        captureGen_[ex.groupId] = gen_;
      }
    }

    isAccepting = accepting_[cur_] != 0;
    return isAccepting;
  }

  // Capture recorded for a group in the current scan, or nullptr.
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
    for (char c : input) {
      step(c);
      if (cur_ == kDead) break;
    }
  }

  bool matches(const std::string& input) {
    simulate(input);
    return isAccepting;
  }

  bool canReachAcceptingWithNonEmptyInput() const { return canExtend_[cur_]; }
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

    std::set<unsigned char> chars;
    // Range endpoints are compared and iterated as unsigned: a signed char
    // would order bytes >= 0x80 below 0x00 and overflow past CHAR_MAX.
    int prev = 0;
    bool inRange = false;

    while (true) {
      TokenChar tc = peek();
      if (tc.isEnd()) throw std::runtime_error("Unclosed character class");

      if (tc.ch == ']' && !tc.escaped) break;

      consume();  // consume the current character

      int c = static_cast<unsigned char>(tc.ch);

      if (inRange) {
        if (prev > c) throw std::runtime_error("Invalid range: start > end");
        for (int ch = prev; ch <= c; ++ch)
          chars.insert(static_cast<unsigned char>(ch));
        inRange = false;
      } else if (c == '-' && !tc.escaped && !peek().isEnd() &&
                 peek().ch != ']') {
        // Unescaped '-' in middle (not at end) starts a range
        inRange = true;
      } else {
        chars.insert(static_cast<unsigned char>(c));
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
  // Thompson construction only; the caller determinizes.
  NFA parseToNFA(const std::string& r) {
    regex = r;
    pos = 0;
    nextGroupId = 0;
    NFA nfa = parseUnion();

    if (peek().ch != '\0')
      throw std::runtime_error("Extra characters at end of regex");

    return nfa;
  }

  // Parse and determinize. DFA states are materialized lazily on first use.
  DFA parse(const std::string& r) { return DFA(parseToNFA(r)); }
};