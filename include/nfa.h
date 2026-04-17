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
};
struct RegexCapture {
  Position start;
  Position end;
  int groupIdx = -1;
  std::string text;
  std::string groupName;
};

struct StepResult {
  bool isAccepting = false;
  std::map<int, std::vector<RegexCapture>> captures = {};
};

class NFA {
 private:
  std::vector<std::unique_ptr<State>> allStates;
  std::unordered_set<State*> activeStates;
  std::map<int, std::vector<RegexCapture>> candidateRegexCaptures;
  std::map<int, std::vector<RegexCapture>> actualRegexCaptures;
  std::string currentInput = "";
  Position position;

  // Precomputed reachability cache: states that can reach accepting via
  // non-empty path
  std::unordered_set<State*> canReachAcceptingNonEmpty_;
  bool reachabilityComputed_ = false;

  State* createState() {
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
    activeStates.clear();
    candidateRegexCaptures.clear();
    actualRegexCaptures.clear();
    currentInput = currentInput.substr(0, pos.offset);
    activeStates = epsilonClosure({startState});
    for (auto* s : activeStates) {
      if (s->isAccepting) {
        isAccepting = true;
      }
    }
  }

  void fullReset() { resetToPosition(Position()); }

  void acquireStatesFrom(NFA& other) {
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

  std::unordered_set<State*> epsilonClosure(
      const std::unordered_set<State*>& states) const {
    std::unordered_set<State*> result = states;
    std::stack<State*> stack;
    for (State* s : states) stack.push(s);

    while (!stack.empty()) {
      State* cur = stack.top();
      stack.pop();
      for (State* next : cur->epsilonTransitions) {
        if (result.insert(next).second) stack.push(next);
      }
    }
    return result;
  }

  StepResult step(char c) {
    currentInput += c;
    std::unordered_set<State*> nextActiveStates;

    // === NORMAL TRANSITIONS ===
    for (State* s : activeStates) {
      // enterGroup: start RegexCapture BEFORE consuming char
      if (s->enterGroup) {
        RegexCapture cap;
        cap.start = position;  // RegexCapture starts at current position
        cap.groupIdx = s->groupId;
        cap.groupName = s->groupName;

        candidateRegexCaptures[s->groupId].push_back(cap);
      }

      // char transitions
      auto it = s->transitions.find(c);
      if (it != s->transitions.end()) {
        for (State* t : it->second) nextActiveStates.insert(t);
      }

      // dot transitions
      for (State* t : s->anyCharTransitions) nextActiveStates.insert(t);
    }

    activeStates = epsilonClosure(nextActiveStates);

    // update position
    position.offset += 1;
    if (c == '\n') {
      position.line += 1;
      position.column = 1;
    } else {
      position.column += 1;
    }

    bool nowAccepting = false;

    // === HANDLE EXIT AND ACCEPTING ===
    for (State* s : activeStates) {
      if (s->isAccepting) nowAccepting = true;

      if (s->exitGroup) {
        auto it = candidateRegexCaptures.find(s->groupId);
        if (it != candidateRegexCaptures.end() && !it->second.empty()) {
          RegexCapture cap = it->second.back();
          cap.end = position;

          // === NEW: Extract actual text ===
          if (!currentInput.empty()) {
            if (cap.start.offset >= 0 &&
                cap.end.offset <= static_cast<int>(currentInput.size())) {
              cap.text = currentInput.substr(cap.start.offset,
                                             cap.end.offset - cap.start.offset);
            } else {
              cap.text = "";  // safety
            }
          }

          // Also set group name if not already (in case it was missing)
          cap.groupName = s->groupName;

          actualRegexCaptures[s->groupId].push_back(cap);
        }
      }
    }

    isAccepting = nowAccepting;
    return {isAccepting, actualRegexCaptures};
  }

  void simulate(const std::string& input) {
    fullReset();
    for (char c : input) {
      step(c);
      if (activeStates.empty()) break;
    }
  }

  bool matches(const std::string& input) {
    simulate(input);
    for (State* s : activeStates)
      if (s->isAccepting) return true;
    return false;
  }

  // Precompute which states can reach an accepting state via a non-empty path.
  // Uses reverse BFS from accepting states.
  void precomputeCanReachAccepting() {
    if (reachabilityComputed_) return;
    reachabilityComputed_ = true;

    // First: find states that can reach accepting (with any path)
    std::unordered_set<State*> canReachAccepting;

    // Build reverse transition graph
    std::unordered_map<State*, std::vector<State*>> reverseEpsilon;
    std::unordered_map<State*, std::vector<State*>> reverseNonEpsilon;

    for (const auto& statePtr : allStates) {
      State* s = statePtr.get();
      for (State* next : s->epsilonTransitions) {
        reverseEpsilon[next].push_back(s);
      }
      for (const auto& [ch, targets] : s->transitions) {
        for (State* next : targets) {
          reverseNonEpsilon[next].push_back(s);
        }
      }
      for (State* next : s->anyCharTransitions) {
        reverseNonEpsilon[next].push_back(s);
      }
    }

    // BFS backward from accepting states to find all states that can reach
    // accepting
    std::queue<State*> q;
    for (const auto& statePtr : allStates) {
      if (statePtr->isAccepting) {
        canReachAccepting.insert(statePtr.get());
        q.push(statePtr.get());
      }
    }

    while (!q.empty()) {
      State* cur = q.front();
      q.pop();
      // Follow reverse epsilon edges
      for (State* prev : reverseEpsilon[cur]) {
        if (canReachAccepting.insert(prev).second) {
          q.push(prev);
        }
      }
      // Follow reverse non-epsilon edges
      for (State* prev : reverseNonEpsilon[cur]) {
        if (canReachAccepting.insert(prev).second) {
          q.push(prev);
        }
      }
    }

    // Now find states that can reach accepting via non-empty path:
    // A state S can reach accepting with non-empty input if:
    // S has a non-epsilon transition to a state in canReachAccepting
    for (const auto& statePtr : allStates) {
      State* s = statePtr.get();
      bool canReachNonEmpty = false;

      for (const auto& [ch, targets] : s->transitions) {
        for (State* next : targets) {
          if (canReachAccepting.count(next)) {
            canReachNonEmpty = true;
            break;
          }
        }
        if (canReachNonEmpty) break;
      }
      if (!canReachNonEmpty) {
        for (State* next : s->anyCharTransitions) {
          if (canReachAccepting.count(next)) {
            canReachNonEmpty = true;
            break;
          }
        }
      }

      if (canReachNonEmpty) {
        canReachAcceptingNonEmpty_.insert(s);
      }
    }

    // Also propagate backward through epsilon transitions:
    // If S ->epsilon-> T and T is in canReachAcceptingNonEmpty_, then S is too
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto& statePtr : allStates) {
        State* s = statePtr.get();
        if (canReachAcceptingNonEmpty_.count(s)) continue;

        for (State* next : s->epsilonTransitions) {
          if (canReachAcceptingNonEmpty_.count(next)) {
            canReachAcceptingNonEmpty_.insert(s);
            changed = true;
            break;
          }
        }
      }
    }
  }

  bool canReachAcceptingWithNonEmptyInput() {
    precomputeCanReachAccepting();

    // Check if any active state can reach accepting with non-empty input
    for (State* s : activeStates) {
      if (canReachAcceptingNonEmpty_.count(s)) {
        return true;
      }
    }
    return false;
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

      // Create entry state for group
      NFA entryNFA;
      entryNFA.startState->enterGroup = true;
      entryNFA.startState->groupId = groupId;
      entryNFA.startState->groupName = groupName;
      entryNFA.startState->epsilonTransitions.insert(entryNFA.acceptingState);

      // Parse subexpression
      NFA sub = parseUnion();

      // Create exit state for group
      NFA exitNFA;
      exitNFA.startState->exitGroup = true;
      exitNFA.startState->groupId = groupId;
      exitNFA.startState->groupName = groupName;
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