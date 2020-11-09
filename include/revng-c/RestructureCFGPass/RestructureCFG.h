#ifndef REVNGC_RESTRUCTURE_CFG_RESTRUCTURECFG_H
#define REVNGC_RESTRUCTURE_CFG_RESTRUCTURECFG_H

//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

// Standard includes
#include <fstream>

// LLVM includes
#include "llvm/Pass.h"

// Local libraries includes
#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/RegionCFGTreeBB.h"

class RestructureCFG : public llvm::FunctionPass {

private:
  using DuplicationMap = std::map<llvm::BasicBlock *, size_t>;

public:
  static char ID;

public:
  RestructureCFG() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  ASTTree &getAST() { return AST; }

  std::map<llvm::BasicBlock *, size_t> &getNDuplicates() { return NDuplicates; }

private:
  ASTTree AST;
  DuplicationMap NDuplicates;
};

#endif // REVNGC_RESTRUCTURE_CFG_RESTRUCTURECFG_H
