#include <dg/util/SilenceLLVMWarnings.h>
SILENCE_LLVM_WARNINGS_PUSH
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_os_ostream.h>
SILENCE_LLVM_WARNINGS_POP

#include "dg/llvm/LLVMFastSlicer.h"


namespace dg {
namespace llvmdg {

std::set<llvm::Value *>
LLVMFastSlicer::computeSlice(const std::vector<const llvm::Value *> &criteria) {
    std::set<llvm::Value *> slice;
    return slice;
}

} // namespace llvmdg
} // namespace dg
