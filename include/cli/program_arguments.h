// program_arguments.h — the argc and argv handed to a Sun program's main.
//
// A Sun program sees the same shape a C program does: argv[0] is the script
// name, the forwarded arguments follow, and a null pointer ends the list.
// This class owns the strings so the pointers it hands out stay valid for as
// long as it lives.

#pragma once

#include <string>
#include <vector>

namespace sun::cli {

class ProgramArguments {
 public:
  // scriptName becomes argv[0]; args follow it in order.
  ProgramArguments(const std::string& scriptName,
                   const std::vector<std::string>& args) {
    storage_.reserve(args.size() + 1);
    storage_.push_back(scriptName);
    storage_.insert(storage_.end(), args.begin(), args.end());
    for (std::string& arg : storage_) {
      pointers_.push_back(arg.data());
    }
    // Null-terminate for C compatibility
    pointers_.push_back(nullptr);
  }

  // The pointers refer into this object's own strings, so a copy or a move
  // would leave them pointing at the wrong place.
  ProgramArguments(const ProgramArguments&) = delete;
  ProgramArguments& operator=(const ProgramArguments&) = delete;

  // Number of arguments, counting the script name but not the closing null.
  int argc() const { return static_cast<int>(storage_.size()); }

  // The argument pointers, ending with a null pointer.
  char** argv() { return pointers_.data(); }

 private:
  std::vector<std::string> storage_;
  std::vector<char*> pointers_;
};

}  // namespace sun::cli
