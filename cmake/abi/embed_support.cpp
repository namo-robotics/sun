/** Builds the immutable compiler support descriptor from checked-in TextProto. */
#include "abi/support_manifest.h"
#include <google/protobuf/text_format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

/** Validates a manifest and writes its binary protobuf as a C++ byte array. */
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("Expected manifest and output paths");
    std::ifstream input(argv[1]);
    if (!input) throw std::runtime_error("Cannot read compiler ABI manifest");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    sun::abi::proto::CompilerAbiSupport support;
    if (!google::protobuf::TextFormat::ParseFromString(text, &support))
      throw std::runtime_error("Invalid compiler ABI TextProto");
    sun::abi::validateCompilerSupport(support);
    const auto bytes = support.SerializeAsString();
    std::string header = "// Generated compiler ABI support.\n#pragma once\n"
        "inline constexpr unsigned char sunCompilerAbiSupport[] = {";
    for (unsigned char byte : bytes) header += std::to_string(byte) + ",";
    header += "};\n";
    std::ifstream previous(argv[2]);
    const std::string old((std::istreambuf_iterator<char>(previous)), {});
    if (old == header) return 0;
    std::ofstream output(argv[2]);
    output << header;
    if (!output) throw std::runtime_error("Cannot write compiler ABI support header");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
