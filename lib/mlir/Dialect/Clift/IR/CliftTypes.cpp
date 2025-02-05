//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <string>

#include "llvm/ADT/SmallSet.h"

#include "revng/mlir/Dialect/Clift/IR/CliftTypes.h"
// keep this order
#include "revng/Model/Binary.h"
#include "revng/mlir/Dialect/Clift/IR/CliftAttributes.h"

#include "CliftParser.h"
#include "CliftStorage.h"

#define GET_TYPEDEF_CLASSES
#include "revng/mlir/Dialect/Clift/IR/CliftOpsTypes.cpp.inc"

using namespace mlir::clift;
namespace clift = mlir::clift;

using EmitErrorType = llvm::function_ref<mlir::InFlightDiagnostic()>;

//===---------------------------- Type helpers ----------------------------===//

TypedefDecomposition clift::decomposeTypedef(ValueType Type) {
  bool HasConstTypedef = false;

  while (true) {
    auto D = mlir::dyn_cast<DefinedType>(Type);
    if (not D)
      break;

    auto T = mlir::dyn_cast<TypedefTypeAttr>(D.getElementType());
    if (not T)
      break;

    Type = T.getUnderlyingType();
    HasConstTypedef |= D.isConst();
  }

  return { Type, HasConstTypedef };
}

ValueType clift::dealias(ValueType Type, bool IgnoreQualifiers) {
  auto [UnderlyingType, HasConstTypedef] = decomposeTypedef(Type);

  if (HasConstTypedef and not IgnoreQualifiers)
    UnderlyingType = UnderlyingType.addConst();

  return UnderlyingType;
}

bool clift::isModifiableType(ValueType Type) {
  auto [UnderlyingType, HasConst] = decomposeTypedef(Type);
  return not HasConst and not UnderlyingType.isConst();
}

bool clift::isIntegerKind(PrimitiveKind Kind) {
  switch (Kind) {
  case PrimitiveKind::GenericKind:
  case PrimitiveKind::PointerOrNumberKind:
  case PrimitiveKind::NumberKind:
  case PrimitiveKind::UnsignedKind:
  case PrimitiveKind::SignedKind:
    return true;

  case PrimitiveKind::VoidKind:
  case PrimitiveKind::FloatKind:
    break;
  }
  return false;
}

PrimitiveType clift::getUnderlyingIntegerType(ValueType Type) {
  Type = dealias(Type);

  if (auto T = mlir::dyn_cast<PrimitiveType>(Type))
    return isIntegerKind(T.getKind()) ? T : nullptr;

  if (auto T = mlir::dyn_cast<DefinedType>(Type)) {
    if (auto EnumT = mlir::dyn_cast<EnumTypeAttr>(T.getElementType()))
      return mlir::cast<PrimitiveType>(dealias(EnumT.getUnderlyingType()));
  }

  return nullptr;
}

bool clift::isCompleteType(ValueType Type) {
  Type = dealias(Type);

  if (auto T = mlir::dyn_cast<DefinedType>(Type)) {
    auto Definition = T.getElementType();
    if (auto D = mlir::dyn_cast<StructTypeAttr>(Definition))
      return D.isDefinition();
    if (auto D = mlir::dyn_cast<UnionTypeAttr>(Definition))
      return D.isDefinition();
    return true;
  }

  if (auto T = mlir::dyn_cast<ScalarTupleType>(Type))
    return T.isComplete();

  if (auto T = mlir::dyn_cast<ArrayType>(Type))
    return isCompleteType(T.getElementType());

  return true;
}

bool clift::isVoid(ValueType Type) {
  Type = dealias(Type);

  if (auto T = mlir::dyn_cast<PrimitiveType>(Type))
    return T.getKind() == PrimitiveKind::VoidKind;

  return false;
}

bool clift::isScalarType(ValueType Type) {
  Type = dealias(Type);

  if (auto T = mlir::dyn_cast<PrimitiveType>(Type))
    return T.getKind() != PrimitiveKind::VoidKind;

  if (auto T = mlir::dyn_cast<DefinedType>(Type))
    return mlir::isa<EnumTypeAttr>(T.getElementType());

  return mlir::isa<PointerType>(Type);
}

bool clift::isPrimitiveIntegerType(ValueType Type) {
  if (auto T = mlir::dyn_cast<PrimitiveType>(dealias(Type, true)))
    return isIntegerKind(T.getKind());

  return false;
}

bool clift::isIntegerType(ValueType Type) {
  Type = dealias(Type);

  if (auto T = mlir::dyn_cast<PrimitiveType>(Type))
    return isIntegerKind(T.getKind());

  if (auto T = mlir::dyn_cast<DefinedType>(Type))
    return mlir::isa<EnumTypeAttr>(T.getElementType());

  return false;
}

bool clift::isPointerType(ValueType Type) {
  return mlir::isa<PointerType>(dealias(Type));
}

bool clift::isObjectType(ValueType Type) {
  Type = dealias(Type);

  if (auto T = mlir::dyn_cast<PrimitiveType>(Type)) {
    if (T.getKind() == PrimitiveKind::VoidKind)
      return false;
  }

  if (auto T = mlir::dyn_cast<DefinedType>(Type)) {
    if (mlir::isa<FunctionTypeAttr>(T.getElementType()))
      return false;
  }

  if (mlir::isa<ScalarTupleType>(Type))
    return false;

  return true;
}

bool clift::isArrayType(ValueType Type) {
  return mlir::isa<ArrayType>(dealias(Type));
}

bool clift::isEnumType(ValueType Type) {
  if (auto T = mlir::dyn_cast<DefinedType>(dealias(Type))) {
    if (mlir::isa<EnumTypeAttr>(T.getElementType()))
      return true;
  }
  return false;
}

bool clift::isClassType(ValueType Type) {
  if (auto T = mlir::dyn_cast<DefinedType>(dealias(Type))) {
    if (mlir::isa<StructTypeAttr, UnionTypeAttr>(T.getElementType()))
      return true;
  }
  return false;
}

bool clift::isFunctionType(ValueType Type) {
  if (auto T = mlir::dyn_cast<DefinedType>(dealias(Type))) {
    if (mlir::isa<FunctionTypeAttr>(T.getElementType()))
      return true;
  }
  return false;
}

bool clift::isReturnableType(ValueType ReturnType) {
  ReturnType = dealias(ReturnType);

  if (isObjectType(ReturnType))
    return not isArrayType(ReturnType);

  return isVoid(ReturnType) or mlir::isa<ScalarTupleType>(ReturnType);
}

//===---------------------------- CliftDialect ----------------------------===//

void CliftDialect::registerTypes() {
  addTypes<ScalarTupleType, /* Include the auto-generated clift types */
#define GET_TYPEDEF_LIST
#include "revng/mlir/Dialect/Clift/IR/CliftOpsTypes.cpp.inc"
           /* End of auto-generated list */>();
}

/// Parse a type registered to this dialect
mlir::Type CliftDialect::parseType(mlir::DialectAsmParser &Parser) const {
  const llvm::SMLoc TypeLoc = Parser.getCurrentLocation();

  llvm::StringRef Mnemonic;
  if (mlir::Type GenType;
      generatedTypeParser(Parser, &Mnemonic, GenType).has_value())
    return GenType;

  if (Mnemonic == ScalarTupleType::getMnemonic())
    return ScalarTupleType::parse(Parser);

  Parser.emitError(TypeLoc) << "unknown type `" << Mnemonic << "` in dialect `"
                            << getNamespace() << "`";
  return {};
}

/// Print a type registered to this dialect
void CliftDialect::printType(mlir::Type Type,
                             mlir::DialectAsmPrinter &Printer) const {
  if (mlir::succeeded(generatedTypePrinter(Type, Printer)))
    return;
  if (auto T = mlir::dyn_cast<ScalarTupleType>(Type)) {
    T.print(Printer);
    return;
  }
  revng_abort("cannot print type");
}

//===---------------------------- PrimitiveType ---------------------------===//

static constexpr model::PrimitiveKind::Values
kindToKind(const PrimitiveKind Kind) {
  return static_cast<model::PrimitiveKind::Values>(Kind);
}

/// Test that kindToKind converts each clift::PrimitiveKind to the matching
/// model::PrimitiveKind. Use a switch converting in the opposite direction
/// in order to produce a warning if a new primitive kind is ever added.
static consteval bool testKindToKind() {
  PrimitiveKind UninitializedKind;
  const auto TestSwitch = [&](const model::PrimitiveKind::Values Kind) {
    switch (Kind) {
    case model::PrimitiveKind::Float:
      return PrimitiveKind::FloatKind;
    case model::PrimitiveKind::Generic:
      return PrimitiveKind::GenericKind;
    case model::PrimitiveKind::Number:
      return PrimitiveKind::NumberKind;
    case model::PrimitiveKind::PointerOrNumber:
      return PrimitiveKind::PointerOrNumberKind;
    case model::PrimitiveKind::Signed:
      return PrimitiveKind::SignedKind;
    case model::PrimitiveKind::Unsigned:
      return PrimitiveKind::UnsignedKind;
    case model::PrimitiveKind::Void:
      return PrimitiveKind::VoidKind;

    case model::PrimitiveKind::Invalid:
    case model::PrimitiveKind::Count:
      // Unreachable. This causes an error during constant evaluation.
      return UninitializedKind;
    }
  };

  for (int I = 0; I < static_cast<int>(model::PrimitiveKind::Count); ++I) {
    auto const Kind = static_cast<model::PrimitiveKind::Values>(I);
    if (Kind != model::PrimitiveKind::Invalid) {
      if (kindToKind(TestSwitch(Kind)) != Kind)
        return false;
    }
  }
  return true;
}
static_assert(testKindToKind());

mlir::LogicalResult PrimitiveType::verify(EmitErrorType EmitError,
                                          PrimitiveKind Kind,
                                          uint64_t Size,
                                          BoolAttr IsConst) {
  if (not model::PrimitiveType::make(kindToKind(Kind), Size)->verify())
    return EmitError() << "primitive type verify failed";

  return mlir::success();
}

bool PrimitiveType::getAlias(llvm::raw_ostream &OS) const {
  OS << toString(model::PrimitiveType::getCName(kindToKind(getKind()),
                                                getByteSize()));
  if (isConst())
    OS << "$const";
  return true;
}

ValueType PrimitiveType::addConst() const {
  if (isConst())
    return *this;

  return get(getContext(),
             getKind(),
             getSize(),
             BoolAttr::get(getContext(), true));
}

ValueType PrimitiveType::removeConst() const {
  if (not isConst())
    return *this;

  return get(getContext(),
             getKind(),
             getSize(),
             BoolAttr::get(getContext(), false));
}

//===----------------------------- PointerType ----------------------------===//

mlir::LogicalResult PointerType::verify(EmitErrorType EmitError,
                                        ValueType ElementType,
                                        uint64_t PointerSize,
                                        BoolAttr IsConst) {
  switch (PointerSize) {
  case 4:
  case 8:
    break;
  default:
    return EmitError() << "invalid pointer size: " << PointerSize;
  }
  return mlir::success();
}

uint64_t PointerType::getByteSize() const {
  return getPointerSize();
}

ValueType PointerType::addConst() const {
  if (isConst())
    return *this;

  return get(getContext(),
             getPointeeType(),
             getPointerSize(),
             BoolAttr::get(getContext(), true));
}

ValueType PointerType::removeConst() const {
  if (not isConst())
    return *this;

  return get(getContext(),
             getPointeeType(),
             getPointerSize(),
             BoolAttr::get(getContext(), false));
}

//===------------------------------ ArrayType -----------------------------===//

mlir::LogicalResult ArrayType::verify(EmitErrorType EmitError,
                                      ValueType ElementType,
                                      uint64_t ElementCount) {
  if (not isObjectType(ElementType))
    return EmitError() << "Array type element type must be an object type.";
  if (ElementCount == 0)
    return EmitError() << "Array type must have no less than one element.";

  return mlir::success();
}

uint64_t ArrayType::getByteSize() const {
  return getElementType().getByteSize() * getElementsCount();
}

bool ArrayType::isConst() const {
  return getElementType().isConst();
}

ValueType ArrayType::addConst() const {
  auto ElementT = getElementType();
  auto NewElementT = ElementT.addConst();

  if (ElementT == NewElementT)
    return *this;

  return get(getContext(), NewElementT, getElementsCount());
}

ValueType ArrayType::removeConst() const {
  auto ElementT = getElementType();
  auto NewElementT = ElementT.removeConst();

  if (ElementT == NewElementT)
    return *this;

  return get(getContext(), NewElementT, getElementsCount());
}

//===----------------------------- DefinedType ----------------------------===//

mlir::LogicalResult DefinedType::verify(EmitErrorType EmitError,
                                        TypeDefinitionAttr Definition,
                                        BoolAttr IsConst) {
  return mlir::success();
}

uint64_t DefinedType::id() const {
  return getElementType().id();
}

llvm::StringRef DefinedType::name() const {
  return getElementType().name();
}

uint64_t DefinedType::getByteSize() const {
  return getElementType().getByteSize();
}

bool DefinedType::getAlias(llvm::raw_ostream &OS) const {
  const llvm::StringRef Name = getElementType().name();

  if (Name.empty())
    return false;

  OS << Name;
  if (isConst())
    OS << "$const";
  return true;
}

ValueType DefinedType::addConst() const {
  if (getIsConst())
    return *this;

  return get(getContext(), getElementType(), BoolAttr::get(getContext(), true));
}

ValueType DefinedType::removeConst() const {
  if (not getIsConst())
    return *this;

  return get(getContext(),
             getElementType(),
             BoolAttr::get(getContext(), false));
}

//===--------------------------- ScalarTupleType --------------------------===//

mlir::LogicalResult ScalarTupleType::verify(const EmitErrorType EmitError,
                                            const uint64_t ID) {
  return mlir::success();
}

mlir::LogicalResult
ScalarTupleType::verify(const EmitErrorType EmitError,
                        const uint64_t ID,
                        const llvm::StringRef Name,
                        const llvm::ArrayRef<ScalarTupleElementAttr> Elements) {
  if (Elements.size() < 2)
    return EmitError() << "Scalar tuple types must have at least two elements";

  llvm::SmallSet<llvm::StringRef, 16> NameSet;
  for (auto Element : Elements) {
    if (not Element.getName().empty()) {
      if (not NameSet.insert(Element.getName()).second)
        return EmitError() << "Scalar tuple element names must be empty or "
                              "unique";
    }
  }

  return mlir::success();
}

ScalarTupleType ScalarTupleType::get(MLIRContext *const Context,
                                     const uint64_t ID) {
  return Base::get(Context, ID);
}

ScalarTupleType ScalarTupleType::getChecked(const EmitErrorType EmitError,
                                            MLIRContext *const Context,
                                            const uint64_t ID) {
  return get(Context, ID);
}

ScalarTupleType
ScalarTupleType::get(MLIRContext *const Context,
                     const uint64_t ID,
                     const llvm::StringRef Name,
                     const llvm::ArrayRef<ScalarTupleElementAttr> Elements) {
  auto Result = Base::get(Context, ID);
  Result.define(Name, Elements);
  return Result;
}

ScalarTupleType
ScalarTupleType::getChecked(const EmitErrorType EmitError,
                            MLIRContext *const Context,
                            const uint64_t ID,
                            const llvm::StringRef Name,
                            const llvm::ArrayRef<ScalarTupleElementAttr>
                              Elements) {
  if (failed(verify(EmitError, ID, Name, Elements)))
    return {};
  return get(Context, ID, Name, Elements);
}

void ScalarTupleType::define(const llvm::StringRef Name,
                             const llvm::ArrayRef<ScalarTupleElementAttr>
                               Elements) {
  LogicalResult Result = Base::mutate(Name, Elements);

  revng_assert(succeeded(Result)
               && "attempting to change the body of an already-initialized "
                  "type");
}

uint64_t ScalarTupleType::getId() const {
  return getImpl()->getID();
}

llvm::StringRef ScalarTupleType::getName() const {
  return getImpl()->getName();
}

llvm::ArrayRef<ScalarTupleElementAttr> ScalarTupleType::getElements() const {
  return getImpl()->getSubobjects();
}

bool ScalarTupleType::isComplete() const {
  return getImpl()->isInitialized();
}

uint64_t ScalarTupleType::getByteSize() const {
  uint64_t Size = 0;
  for (ScalarTupleElementAttr Element : getElements())
    Size += Element.getType().getByteSize();
  return Size;
}

bool ScalarTupleType::getAlias(llvm::raw_ostream &OS) const {
  if (getName().empty())
    return false;
  OS << getName() << "$tuple";
  return true;
}

mlir::BoolAttr ScalarTupleType::getIsConst() const {
  return BoolAttr::get(getContext(), false);
}

mlir::Type ScalarTupleType::parse(AsmParser &Parser) {
  return parseCompositeType<ScalarTupleType>(Parser, /*MinSubobjects=*/2);
}

void ScalarTupleType::print(AsmPrinter &Printer) const {
  return printCompositeType(Printer, *this);
}

void ScalarTupleType::walkImmediateSubElements(function_ref<void(Attribute)>
                                                 WalkAttr,
                                               function_ref<void(Type)>
                                                 WalkType) const {
  if (getImpl()->isInitialized()) {
    for (auto Element : getElements())
      WalkAttr(Element);
  }
}

mlir::Type ScalarTupleType::replaceImmediateSubElements(ArrayRef<Attribute>,
                                                        ArrayRef<Type>) const {
  revng_abort("it does not make any sense to replace the elements of a "
              "scalar tuple");
}
