#pragma once

#include <map>
#include <string>

/** Publishes implemented compatibility rules independently of producer age. */
namespace sun::abi {
/** Returns supported rule implementations; this inventory never authorizes a payload. */
std::map<std::string, std::string> supportedRules();
/** Returns the experimental support inventory and producer identity as release JSON. */
std::string compatibilitySupportJson();
}
