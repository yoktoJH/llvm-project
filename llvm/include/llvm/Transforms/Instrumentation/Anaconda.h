#ifndef LLVM_TRANSFORMS_UTILS_ANACONDA_H
#define LLVM_TRANSFORMS_UTILS_ANACONDA_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/ValueMap.h"

#include <map>

namespace llvm {


// this pass should be a module runner as it modifies global variables and inserts functions
//
class AnacondaPass : public PassInfoMixin<AnacondaPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_ANACONDA_H