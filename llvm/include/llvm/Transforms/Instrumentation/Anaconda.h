#ifndef LLVM_TRANSFORMS_UTILS_ANACONDA_H
#define LLVM_TRANSFORMS_UTILS_ANACONDA_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class AnacondaPass : public PassInfoMixin<AnacondaPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_ANACONDA_H