/** Internal checks shared by analysis and test-runner generation. */
#pragma once
#include "driver/driver.h"

/** Compiler pipeline implementation details. */
namespace sun::driver {
/** Reports whether explicit imports supply the standard library. */
bool hasStdlibImport(const std::vector<MoonImport>& moonImports);
/** Reports whether source declarations supply the standard String type. */
bool declaresStdlibString(const BlockExprAST& block);
}  // namespace sun::driver
