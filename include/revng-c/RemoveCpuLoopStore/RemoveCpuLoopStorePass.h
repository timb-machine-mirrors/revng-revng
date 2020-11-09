#ifndef REVNGC_REMOVE_CPU_LOOP_STORE_H
#define REVNGC_REMOVE_CPU_LOOP_STORE_H

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

// LLVM includes
#include <llvm/Pass.h>

class RemoveCpuLoopStorePass : public llvm::FunctionPass {
public:
  static char ID;

public:
  RemoveCpuLoopStorePass() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;
};

#endif // REVNGC_REMOVE_CPU_LOOP_STORE_H
