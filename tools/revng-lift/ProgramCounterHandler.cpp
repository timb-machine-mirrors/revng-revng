/// \file ProgramCounterHandler.cpp
/// \brief

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/SmallSet.h"

#include "revng/BasicAnalyses/GeneratedCodeBasicInfo.h"

#include "ProgramCounterHandler.h"

using namespace llvm;
using PCH = ProgramCounterHandler;

static void eraseIfNoUse(const WeakVH &V) {
  if (Instruction *I = dyn_cast_or_null<Instruction>(&*V))
    if (I->use_begin() == I->use_end())
      I->eraseFromParent();
}

static SwitchInst *getNextSwitch(llvm::SwitchInst::CaseHandle Case) {
  return cast<SwitchInst>(Case.getCaseSuccessor()->getTerminator());
}

static SwitchInst *getNextSwitch(SwitchInst::CaseIt It) {
  return getNextSwitch(*It);
}

static ConstantInt *caseConstant(SwitchInst *Switch, uint64_t Value) {
  auto *ConditionType = cast<IntegerType>(Switch->getCondition()->getType());
  return ConstantInt::get(ConditionType, Value);
}

static void
addCase(llvm::SwitchInst *Switch, uint64_t Value, llvm::BasicBlock *BB) {
  Switch->addCase(caseConstant(Switch, Value), BB);
}

class PartialMetaAddress {
private:
  Optional<uint64_t> Address;
  Optional<uint64_t> Epoch;
  Optional<uint64_t> AddressSpace;
  Optional<uint64_t> Type;

public:
  bool isEmpty() const { return not(Address or Epoch or AddressSpace or Type); }

  void set(const MetaAddress &MA) {
    setAddress(MA.address());
    setEpoch(MA.epoch());
    setAddressSpace(MA.addressSpace());
    setType(MA.type());
  }

  void setAddress(uint64_t V) {
    if (not Address)
      Address = V;
  }

  void setEpoch(uint64_t V) {
    if (not Epoch)
      Epoch = V;
  }

  void setAddressSpace(uint64_t V) {
    if (not AddressSpace)
      AddressSpace = V;
  }

  void setType(uint64_t V) {
    if (not Type)
      Type = V;
  }

  MetaAddress toMetaAddress() const {
    if (Type and Address and Epoch and AddressSpace) {
      auto TheType = static_cast<MetaAddressType::Values>(*Type);
      if (MetaAddressType::isValid(TheType)) {
        return MetaAddress(*Address, TheType, *Epoch, *AddressSpace);
      }
    }

    return MetaAddress::invalid();
  }
};

class State {
private:
  PartialMetaAddress PMA;
  SmallSet<BasicBlock *, 4> Visited;

public:
  bool visit(BasicBlock *BB) {
    // Check if we already visited this block
    if (Visited.count(BB) != 0) {
      return true;
    } else {
      // Register as visited
      Visited.insert(BB);
      return false;
    }
  }

  PartialMetaAddress &agreement() { return PMA; }
};

class StackEntry {
private:
  State S;
  pred_iterator Next;
  pred_iterator End;

public:
  StackEntry(pred_iterator Begin, pred_iterator End, const State &S) :
    S(S), Next(Begin), End(End) {}

  bool isDone() const { return Next == End; }

  std::pair<State *, BasicBlock *> next() {
    revng_assert(not isDone());
    BasicBlock *NextBB = *Next;
    ++Next;
    return { &S, NextBB };
  }
};

bool PCH::isPCAffectingHelper(Instruction *I) const {
  CallInst *HelperCall = getCallToHelper(I);
  if (HelperCall == nullptr)
    return false;

  using GCBI = GeneratedCodeBasicInfo;
  Optional<GCBI::CSVsUsedByHelperCall> UsedCSVs;
  UsedCSVs = GCBI::getCSVUsedByHelperCallIfAvailable(HelperCall);

  // If CSAA didn't consider this helper, be conservative
  if (not UsedCSVs)
    return true;

  for (GlobalVariable *CSV : UsedCSVs->Written)
    if (affectsPC(CSV))
      return true;

  return false;
}

std::pair<NextJumpTarget::Values, MetaAddress>
PCH::getUniqueJumpTarget(llvm::BasicBlock *BB) {
  std::vector<StackEntry> Stack;

  enum ProcessResult { Proceed, DontProceed, BailOut };

  Optional<MetaAddress> AgreedMA;

  bool ChangedByHelper = false;

  auto Process = [&AgreedMA,
                  this,
                  &ChangedByHelper](State &S, BasicBlock *BB) -> ProcessResult {
    // Do not follow backedges
    if (S.visit(BB))
      return DontProceed;

    PartialMetaAddress &PMA = S.agreement();

    // Iterate backward on all instructions
    for (Instruction &I : llvm::make_range(BB->rbegin(), BB->rend())) {
      if (auto *Store = dyn_cast<StoreInst>(&I)) {
        // We found a store
        Value *Pointer = Store->getPointerOperand();
        Value *V = Store->getValueOperand();
        bool AffectsPC = (Pointer == AddressCSV || Pointer == EpochCSV
                          || Pointer == AddressSpaceCSV || Pointer == TypeCSV);

        if (not AffectsPC)
          continue;

        if (auto *StoredValue = dyn_cast<ConstantInt>(skipCasts(V))) {
          // The store affects the PC and it's constant
          uint64_t Value = getLimitedValue(StoredValue);
          if (Pointer == AddressCSV) {
            PMA.setAddress(Value);
          } else if (Pointer == EpochCSV) {
            PMA.setEpoch(Value);
          } else if (Pointer == AddressSpaceCSV) {
            PMA.setAddressSpace(Value);
          } else if (Pointer == TypeCSV) {
            PMA.setType(Value);
          }

        } else {
          // Non-constant store to PC CSV, bail out
          AgreedMA = MetaAddress::invalid();
          return BailOut;
        }

      } else if (CallInst *NewPCCall = getCallTo(&I, "newpc")) {
        //
        // We reached a call to newpc
        //

        if (PMA.isEmpty()) {
          // We have found a path on which the PC doesn't change return an
          // empty llvm::Optional
          revng_abort();
        }

        // Obtain the current PC and fill in all the missing fields
        Value *FirstArgument = NewPCCall->getArgOperand(0);
        PMA.set(MetaAddress::fromConstant(FirstArgument));

        // Compute the final MetaAddress on this path and ensure it's the same
        // as previous ones
        auto MA = PMA.toMetaAddress();
        if (AgreedMA and MA != *AgreedMA) {
          AgreedMA = MetaAddress::invalid();
          return BailOut;
        } else {
          AgreedMA = MA;
          return DontProceed;
        }

      } else if (isPCAffectingHelper(&I)) {
        // Non-constant store to PC CSV, bail out
        AgreedMA = MetaAddress::invalid();
        ChangedByHelper = true;
        return BailOut;
      }
    }

    return Proceed;
  };

  State Initial;

  BasicBlock *CurrentBB = BB;
  State *CurrentState = &Initial;

  while (true) {
    ProcessResult Result = Process(*CurrentState, CurrentBB);
    switch (Result) {
    case Proceed:
      Stack.emplace_back(pred_begin(CurrentBB),
                         pred_end(CurrentBB),
                         *CurrentState);
      break;

    case BailOut:
      Stack.clear();
      break;

    case DontProceed:
      break;
    }

    while (Stack.size() > 0 and Stack.back().isDone())
      Stack.pop_back();

    if (Stack.size() == 0)
      break;

    std::tie(CurrentState, CurrentBB) = Stack.back().next();
  }

  if (ChangedByHelper) {
    return { NextJumpTarget::Helper, MetaAddress::invalid() };
  } else if (AgreedMA and AgreedMA->isValid()) {
    return { NextJumpTarget::Unique, *AgreedMA };
  } else {
    return { NextJumpTarget::Multiple, MetaAddress::invalid() };
  }
}

class SwitchManager {
private:
  LLVMContext &Context;
  Function *F;
  BasicBlock *Default;

  Value *CurrentEpoch;
  Value *CurrentAddressSpace;
  Value *CurrentType;
  Value *CurrentAddress;

  Optional<BlockType::Values> SetBlockType;

public:
  SwitchManager(BasicBlock *Default,
                Value *CurrentEpoch,
                Value *CurrentAddressSpace,
                Value *CurrentType,
                Value *CurrentAddress,
                Optional<BlockType::Values> SetBlockType) :
    Context(getContext(Default)),
    F(Default->getParent()),
    Default(Default),
    CurrentEpoch(CurrentEpoch),
    CurrentAddressSpace(CurrentAddressSpace),
    CurrentType(CurrentType),
    CurrentAddress(CurrentAddress),
    SetBlockType(SetBlockType) {}

  SwitchManager(SwitchInst *Root, Optional<BlockType::Values> SetBlockType) :
    Context(getContext(Root)),
    F(Root->getParent()->getParent()),
    Default(Root->getDefaultDest()),
    SetBlockType(SetBlockType) {

    // Get the switches of the the first MA. This is just in order to get a
    // reference to their conditions
    SwitchInst *EpochSwitch = Root;
    SwitchInst *AddressSpaceSwitch = getNextSwitch(EpochSwitch->case_begin());
    SwitchInst *TypeSwitch = getNextSwitch(AddressSpaceSwitch->case_begin());
    SwitchInst *AddressSwitch = getNextSwitch(TypeSwitch->case_begin());

    // Get the conditions
    CurrentEpoch = EpochSwitch->getCondition();
    CurrentAddressSpace = AddressSpaceSwitch->getCondition();
    CurrentType = TypeSwitch->getCondition();
    CurrentAddress = AddressSwitch->getCondition();
  }

public:
  void destroy(SwitchInst *Root) {
    std::vector<BasicBlock *> AddressSpaceSwitchesBBs;
    std::vector<BasicBlock *> TypeSwitchesBBs;
    std::vector<BasicBlock *> AddressSwitchesBBs;

    // Collect all the switches basic blocks in post-order
    for (const auto &EpochCase : Root->cases()) {
      AddressSpaceSwitchesBBs.push_back(EpochCase.getCaseSuccessor());
      for (const auto &AddressSpaceCase : getNextSwitch(EpochCase)->cases()) {
        TypeSwitchesBBs.push_back(AddressSpaceCase.getCaseSuccessor());
        for (const auto &TypeCase : getNextSwitch(AddressSpaceCase)->cases()) {
          AddressSwitchesBBs.push_back(TypeCase.getCaseSuccessor());
        }
      }
    }

    WeakVH EpochVH(CurrentEpoch);
    WeakVH AddressSpaceVH(CurrentAddressSpace);
    WeakVH TypeVH(CurrentType);
    WeakVH AddressVH(CurrentAddress);

    // Drop the epoch switch
    Root->eraseFromParent();

    // Drop all the switches on address space
    for (BasicBlock *BB : AddressSpaceSwitchesBBs)
      BB->eraseFromParent();

    // Drop all the switches on type
    for (BasicBlock *BB : TypeSwitchesBBs)
      BB->eraseFromParent();

    // Drop all the switches on address
    for (BasicBlock *BB : AddressSwitchesBBs)
      BB->eraseFromParent();

    eraseIfNoUse(EpochVH);
    eraseIfNoUse(AddressSpaceVH);
    eraseIfNoUse(TypeVH);
    eraseIfNoUse(AddressVH);
  }

  SwitchInst *createSwitch(Value *V, IRBuilder<> &Builder) {
    return Builder.CreateSwitch(V, Default, 0);
  }

  SwitchInst *getOrCreateAddressSpaceSwitch(SwitchInst *EpochSwitch,
                                            const MetaAddress &MA) {
    if (auto *Existing = getSwitchForLabel(EpochSwitch, MA.epoch())) {
      return Existing;
    } else {
      return registerEpochCase(EpochSwitch, MA);
    }
  }

  SwitchInst *
  getOrCreateTypeSwitch(SwitchInst *AddressSpaceSwitch, const MetaAddress &MA) {
    if (auto *Existing = getSwitchForLabel(AddressSpaceSwitch,
                                           MA.addressSpace())) {
      return Existing;
    } else {
      return registerAddressSpaceCase(AddressSpaceSwitch, MA);
    }
  }

  SwitchInst *
  getOrCreateAddressSwitch(SwitchInst *TypeSwitch, const MetaAddress &MA) {
    if (auto *Existing = getSwitchForLabel(TypeSwitch, MA.type())) {
      return Existing;
    } else {
      return registerTypeCase(TypeSwitch, MA);
    }
  }

  SwitchInst *registerEpochCase(SwitchInst *Switch, const MetaAddress &MA) {
    return registerNewCase(Switch,
                           MA.epoch(),
                           Twine("epoch_") + Twine(MA.epoch()),
                           CurrentAddressSpace);
  }

  SwitchInst *
  registerAddressSpaceCase(SwitchInst *Switch, const MetaAddress &MA) {
    return registerNewCase(Switch,
                           MA.addressSpace(),
                           "address_space_" + Twine(MA.addressSpace()),
                           CurrentType);
  }

  SwitchInst *registerTypeCase(SwitchInst *Switch, const MetaAddress &MA) {
    const char *TypeName = MetaAddressType::toString(MA.type());
    return registerNewCase(Switch,
                           MA.type(),
                           "type_" + Twine(TypeName),
                           CurrentAddress);
  }

private:
  SwitchInst *getSwitchForLabel(SwitchInst *Parent, uint64_t CaseValue) {
    auto *CaseConstant = caseConstant(Parent, CaseValue);
    auto CaseIt = Parent->findCaseValue(CaseConstant);
    if (CaseIt != Parent->case_default())
      return getNextSwitch(CaseIt);
    else
      return nullptr;
  }

  /// Helper to create a new case in the parent switch and create a new switch
  SwitchInst *registerNewCase(SwitchInst *Switch,
                              uint64_t NewCaseValue,
                              const Twine &NewSuffix,
                              Value *SwitchOn) {
    using BB = BasicBlock;
    auto *NewSwitchBB = BB::Create(Context,
                                   (Switch->getParent()->getName() + "_"
                                    + NewSuffix),
                                   F);
    ::addCase(Switch, NewCaseValue, NewSwitchBB);
    IRBuilder<> Builder(NewSwitchBB);
    SwitchInst *Result = createSwitch(SwitchOn, Builder);
    if (SetBlockType)
      setBlockType(Result, *SetBlockType);
    return Result;
  }
};

void PCH::addCaseToDispatcher(SwitchInst *Root,
                              const DispatcherTarget &NewTarget,
                              Optional<BlockType::Values> SetBlockType) const {
  auto &[MA, BB] = NewTarget;

  SwitchManager SM(Root, SetBlockType);

  SwitchInst *EpochSwitch = Root;
  SwitchInst *AddressSpaceSwitch = nullptr;
  SwitchInst *TypeSwitch = nullptr;
  SwitchInst *AddressSwitch = nullptr;

  // Get or create, step by step, the switches for MA
  AddressSpaceSwitch = SM.getOrCreateAddressSpaceSwitch(EpochSwitch, MA);
  TypeSwitch = SM.getOrCreateTypeSwitch(AddressSpaceSwitch, MA);
  AddressSwitch = SM.getOrCreateAddressSwitch(TypeSwitch, MA);

  // We are the switch of the addresses, add a case targeting BB, if required
  auto *C = caseConstant(AddressSwitch, MA.address());
  auto CaseIt = AddressSwitch->findCaseValue(C);
  if (CaseIt == AddressSwitch->case_default()) {
    ::addCase(AddressSwitch, MA.address(), BB);
  } else {
    revng_assert(CaseIt->getCaseSuccessor() == BB);
  }
}

void PCH::destroyDispatcher(SwitchInst *Root) const {
  SwitchManager(Root, {}).destroy(Root);
}

SwitchInst *
PCH::buildDispatcher(DispatcherTargets &Targets,
                     IRBuilder<> &Builder,
                     BasicBlock *Default,
                     Optional<BlockType::Values> SetBlockType) const {
  revng_assert(Targets.size() != 0);

  LLVMContext &Context = getContext(Default);

  // Sort by MetaAddress
  std::sort(Targets.begin(),
            Targets.end(),
            [](const DispatcherTarget &LHS, const DispatcherTarget &RHS) {
              return std::less<MetaAddress>()(LHS.first, RHS.first);
            });

  // First of all, create code to load the components of the MetaAddress
  Value *CurrentEpoch = Builder.CreateLoad(EpochCSV);
  Value *CurrentAddressSpace = Builder.CreateLoad(AddressSpaceCSV);
  Value *CurrentType = Builder.CreateLoad(TypeCSV);
  Value *CurrentAddress = Builder.CreateLoad(AddressCSV);

  SwitchManager SM(Default,
                   CurrentEpoch,
                   CurrentAddressSpace,
                   CurrentType,
                   CurrentAddress,
                   SetBlockType);

  // Create the first switch, for epoch
  SwitchInst *EpochSwitch = SM.createSwitch(CurrentEpoch, Builder);
  SwitchInst *AddressSpaceSwitch = nullptr;
  SwitchInst *TypeSwitch = nullptr;
  SwitchInst *AddressSwitch = nullptr;

  // Initially, we need to create a switch at each level
  bool ForceNewSwitch = true;

  MetaAddress Last = MetaAddress::invalid();
  for (const auto &[MA, BB] : Targets) {
    // Extract raw values for the current MetaAddress
    uint64_t Epoch = MA.epoch();
    uint64_t AddressSpace = MA.addressSpace();
    uint64_t Type = MA.type();
    uint64_t Address = MA.address();

    // If it's the first iteration, or any of the components of the
    // MetaAddress has a different value, emit the required switch and new
    // cases

    if (ForceNewSwitch or Epoch != Last.epoch()) {
      AddressSpaceSwitch = SM.registerEpochCase(EpochSwitch, MA);
      ForceNewSwitch = true;
    }

    if (ForceNewSwitch or AddressSpace != Last.addressSpace()) {
      TypeSwitch = SM.registerAddressSpaceCase(AddressSpaceSwitch, MA);
      ForceNewSwitch = true;
    }

    if (ForceNewSwitch or Type != Last.type()) {
      const char *TypeName = MetaAddressType::toString(MA.type());
      AddressSwitch = SM.registerTypeCase(TypeSwitch, MA);
      ForceNewSwitch = true;
    }

    ::addCase(AddressSwitch, Address, BB);

    Last = MA;
    ForceNewSwitch = false;
  }

  return EpochSwitch;
}

std::unique_ptr<ProgramCounterHandler>
PCH::create(Triple::ArchType Architecture,
            Module *M,
            PTCInterface *PTC,
            const CSVFactory &Factory) {
  switch (Architecture) {
  case Triple::arm:
    return std::make_unique<ARMProgramCounterHandler>(M, PTC, Factory);

  case Triple::x86_64:
  case Triple::mips:
  case Triple::mipsel:
  case Triple::aarch64:
  case Triple::systemz:
  case Triple::x86:
    return std::make_unique<PCOnlyProgramCounterHandler>(M, PTC, Factory);

  default:
    revng_abort("Unsupported architecture");
  }

  revng_abort();
}

void PCH::buildHotPath(IRBuilder<> &B,
                       const DispatcherTarget &CandidateTarget,
                       BasicBlock *Default) const {
  auto &[Address, BB] = CandidateTarget;

  auto CreateCmp = [&B](GlobalVariable *CSV, uint64_t Value) {
    Instruction *Load = B.CreateLoad(CSV);
    Type *LoadType = Load->getType();
    return B.CreateICmpEQ(Load, ConstantInt::get(LoadType, Value));
  };

  std::array<Value *, 4> ToAnd = { CreateCmp(EpochCSV, Address.epoch()),
                                   CreateCmp(AddressSpaceCSV,
                                             Address.addressSpace()),
                                   CreateCmp(TypeCSV, Address.type()),
                                   CreateCmp(AddressCSV, Address.address()) };
  auto *Condition = B.CreateAnd(ToAnd);
  B.CreateCondBr(Condition, BB, Default);
}
