#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

class ProgramRunner {
private:
  std::string CurrentProgramPath;
  llvm::SmallVector<llvm::StringRef, 64> Paths;

public:
  ProgramRunner();

  /// returns the exit code of the program.
  [[nodiscard]] int
  run(llvm::StringRef ProgramName, llvm::ArrayRef<std::string> Args);
};

extern ProgramRunner Runner;
