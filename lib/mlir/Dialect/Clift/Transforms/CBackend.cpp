//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <mutex>

#include "llvm/Support/ToolOutputFile.h"

#include "mlir/Pass/Pass.h"
#include "mlir/Support/FileUtilities.h"

#include "revng/TypeNames/PTMLCTypeBuilder.h"
#include "revng/mlir/Dialect/Clift/Transforms/Passes.h"
#include "revng/mlir/Dialect/Clift/Utils/CBackend.h"
#include "revng/mlir/Dialect/Clift/Utils/ImportModel.h"

namespace mlir {
namespace clift {
#define GEN_PASS_DEF_CLIFTEMITC
#include "revng/mlir/Dialect/Clift/Transforms/Passes.h.inc"
} // namespace clift
} // namespace mlir

namespace clift = mlir::clift;

namespace {

// This forces all accesses to the output file to be synchronised.
template<typename T>
class OutputFileWrapper {
public:
  OutputFileWrapper() = default;
  OutputFileWrapper(const OutputFileWrapper &) = delete;
  OutputFileWrapper &operator=(const OutputFileWrapper &) = delete;

  template<typename CallableType>
  decltype(auto) use(CallableType &&Callable) {
    std::lock_guard Lock(Mutex);
    return static_cast<CallableType &&>(Callable)(Value);
  }

private:
  std::mutex Mutex;
  T Value;
};

struct EmitCPass : clift::impl::CliftEmitCBase<EmitCPass> {
  using OutputFilePtr = std::unique_ptr<llvm::ToolOutputFile>;
  std::shared_ptr<OutputFileWrapper<OutputFilePtr>> OutputFile;

  EmitCPass() :
    OutputFile(std::make_shared<OutputFileWrapper<OutputFilePtr>>()) {}

  bool tryOpenOutputFile() {
    return OutputFile->use([&](auto &File) -> bool {
      if (File == nullptr) {
        std::string ErrorMessage;
        File = mlir::openOutputFile(Output, &ErrorMessage);

        if (File != nullptr) {
          File->keep();
        } else {
          llvm::errs() << ErrorMessage << "\n";
          signalPassFailure();
        }
      }
      return File != nullptr;
    });
  }

  void writeToOutputFile(llvm::StringRef Content) {
    OutputFile->use([&](const auto &File) {
      revng_assert(File != nullptr);
      File->os() << Content;
    });
  }

  void runOnOperation() override {
    // TODO: Get the platform info from an option.
    const clift::PlatformInfo Platform = {
      .sizeof_char = 1,
      .sizeof_short = 2,
      .sizeof_int = 4,
      .sizeof_long = 8,
      .sizeof_longlong = 8,
      .sizeof_float = 4,
      .sizeof_double = 8,
      .sizeof_pointer = 8,
    };

    if (not tryOpenOutputFile())
      return;

    llvm::raw_null_ostream NullStream;
    ptml::CTypeBuilder B(NullStream, *Model, ptml::CBuilder(Tagless));
    B.collectInlinableTypes();

    getOperation()->walk([&](clift::FunctionOp Function) {
      if (not Function.isExternal())
        writeToOutputFile(clift::decompile(Function, Platform, B));
    });
  }
};

} // namespace

std::unique_ptr<mlir::OperationPass<clift::ModuleOp>> clift::createEmitCPass() {
  return std::make_unique<EmitCPass>();
}
