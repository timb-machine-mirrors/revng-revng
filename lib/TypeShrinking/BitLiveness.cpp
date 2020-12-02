//
// Copyright rev.ng Srls. See LICENSE.md for details.
//

#include <limits>

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"

#include "revng/Support/Assert.h"

#include "BitLiveness.h"

#include "DataFlowGraph.h"

namespace TypeShrinking {

using BitVector = llvm::BitVector;
using Instruction = llvm::Instruction;

const uint32_t Top = std::numeric_limits<uint32_t>::max();

bool isDataFlowSink(const Instruction *Ins) {
  if (Ins->mayHaveSideEffects() || Ins->getOpcode() == Instruction::Call
      || Ins->getOpcode() == Instruction::CallBr
      || Ins->getOpcode() == Instruction::Ret
      || Ins->getOpcode() == Instruction::Store
      || Ins->getOpcode() == Instruction::Br
      || Ins->getOpcode() == Instruction::IndirectBr)
    return true;
  return false;
}

uint32_t min(const uint32_t &a, const uint32_t &b) {
  return a < b ? a : b;
}

uint32_t max(const uint32_t &a, const uint32_t &b) {
  return a > b ? a : b;
}

uint32_t getMaxOperandSize(Instruction *Ins) {
  uint32_t Max = 0;
  for (uint32_t i = 0; i < Ins->getNumOperands(); ++i) {
    auto *Operand = Ins->getOperand(i);
    if (Operand->getType()->isIntegerTy())
      Max = max(Max, Operand->getType()->getIntegerBitWidth());
    else
      return Top;
  }
  return Max;
}

/// A specialization of the transfer function for the and instruction
/// In cases where one of the operands is constant
uint32_t transferMask(const uint32_t &Element, const uint32_t &MaskIndex) {
  return min(Element, MaskIndex);
}

uint32_t transferAnd(Instruction *Ins, const uint32_t &Element) {
  revng_assert(Ins->getOpcode() == Instruction::And);
  uint32_t Result = Element;
  for (auto &Operand : Ins->operands()) {
    if (llvm::isa<llvm::ConstantInt>(Operand)) {
      auto OpVal = llvm::cast<llvm::ConstantInt>(Operand)->getUniqueInteger();
      auto MostSignificantBit = OpVal.getBitWidth() - OpVal.countLeadingZeros();
      Result = min(Result, transferMask(Element, MostSignificantBit));
    }
  }
  return Result;
}

uint32_t transferShiftLeft(Instruction *Ins, const uint32_t &Element) {
  uint32_t OperandSize = getMaxOperandSize(Ins);
  if (auto ConstOp = llvm::dyn_cast<llvm::ConstantInt>(Ins->getOperand(1))) {
    auto OpVal = ConstOp->getZExtValue();
    if (Element < OpVal)
      return 0;
    return Element - OpVal;
  }
  return OperandSize;
}

uint32_t transferLogicalShiftRight(Instruction *Ins, const uint32_t &Element) {
  uint32_t OperandSize = getMaxOperandSize(Ins);
  if (auto ConstOp = llvm::dyn_cast<llvm::ConstantInt>(Ins->getOperand(1))) {
    auto OpVal = ConstOp->getZExtValue();
    if (Top - OpVal < Element)
      return Top;
    return min(OperandSize, Element + OpVal);
  }
  return OperandSize;
}

uint32_t
transferArithmeticalShiftRight(Instruction *Ins, const uint32_t &Element) {
  uint32_t OperandSize = getMaxOperandSize(Ins);
  if (auto ConstOp = llvm::dyn_cast<llvm::ConstantInt>(Ins->getOperand(1))) {
    auto OpVal = ConstOp->getZExtValue();
    if (Top - OpVal < Element)
      return Top;
    return min(OperandSize, Element + OpVal);
  }
  return OperandSize;
}

uint32_t transferTrunc(Instruction *Ins, const uint32_t &Element) {
  return min(Element, Ins->getType()->getIntegerBitWidth());
}

uint32_t transferZExt(Instruction *Ins, const uint32_t &Element) {
  return min(Element, getMaxOperandSize(Ins));
}

uint32_t
BitLivenessAnalysis::applyTransferFunction(DataFlowNode *L, const uint32_t E) {
  auto *Ins = L->Instruction;
  switch (Ins->getOpcode()) {
  case Instruction::And:
    return transferAnd(Ins, E);
  case Instruction::Xor:
  case Instruction::Or:
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
    return min(E, getMaxOperandSize(L->Instruction));
  case Instruction::Shl:
    return transferShiftLeft(Ins, E);
  case Instruction::LShr:
    return transferLogicalShiftRight(Ins, E);
  case Instruction::AShr:
    return transferArithmeticalShiftRight(Ins, E);
  case Instruction::Trunc:
    return transferTrunc(Ins, E);
  case Instruction::ZExt:
    return transferZExt(Ins, E);
  default:
    // by default all the bits of the operands can be alive
    return getMaxOperandSize(L->Instruction);
  }
}

uint32_t
BitLivenessAnalysis::combineValues(const uint32_t &lh, const uint32_t &rh) {
  return max(lh, rh);
}

bool BitLivenessAnalysis::isLessOrEqual(const uint32_t &lh,
                                        const uint32_t &rh) {
  return lh <= rh;
}

} // namespace TypeShrinking
