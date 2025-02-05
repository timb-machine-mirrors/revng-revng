#pragma once

//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "revng/ADT/Concepts.h"
#include "revng/Support/FunctionTags.h"

// We use a template here in order to instantiate `FunctionType` both as
// `Function` and `const Function`
template<ConstOrNot<llvm::Module> ModuleType>
inline typename std::conditional_t<std::is_const_v<ModuleType>,
                                   const llvm::Function,
                                   llvm::Function> *
getUniqueFunctionWithTag(FunctionTags::Tag &MarkerFunctionTag, ModuleType *M) {
  using FunctionType = typename std::conditional_t<std::is_const_v<ModuleType>,
                                                   const llvm::Function,
                                                   llvm::Function>;
  FunctionType *MarkerCallFunction = nullptr;

  // We could early break from this loop but we would loose the ability of
  // asserting that a single marker function is present
  for (FunctionType &F : MarkerFunctionTag.functions(M)) {
    revng_assert(not MarkerCallFunction);
    MarkerCallFunction = &F;
  }

  revng_assert(MarkerCallFunction);
  return MarkerCallFunction;
}

/// A class that wraps all the logic for injecting goto edges and scope closer
/// edges on LLVM IR. Such edges are then necessary for the ScopeGraph view on
/// LLVM IR
class ScopeGraphBuilder {
private:
  llvm::Function *ScopeCloserFunction = nullptr;
  llvm::Function *GotoBlockFunction = nullptr;

public:
  ScopeGraphBuilder(llvm::Function *F);

public:
  void makeGoto(llvm::BasicBlock *GotoBlock);
  void addScopeCloser(llvm::BasicBlock *Source, llvm::BasicBlock *Target);
};

llvm::SmallVector<const llvm::Instruction *, 2>
getLast2InstructionsBeforeTerminator(const llvm::BasicBlock *BB);

/// Helper function to retrieve the `BasicBlock` target of the marker
llvm::BasicBlock *getScopeCloserTarget(const llvm::BasicBlock *BB);

/// Helper function to determine if `BB` contains a `goto_block` marker
bool isGotoBlock(const llvm::BasicBlock *BB);

void verifyScopeGraphAnnotationsImpl(FunctionTags::Tag &Tag,
                                     const llvm::BasicBlock *BB);

void verifyScopeGraphAnnotations(const llvm::BasicBlock *BB);
