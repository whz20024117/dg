#ifndef _LLVM_MEMORY_SSA_ANALYSIS_H_
#define _LLVM_MEMORY_SSA_ANALYSIS_H_

#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Value.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>

#include <llvm/IR/LegacyPassManager.h>

namespace dg {

class LLVMMemorySSAAnalysis
{
     // Build up all of the passes that we want to run on the module.
    llvm::legacy::PassManager pm;

public:
    LLVMMemorySSAAnalysis(llvm::Module& M) {
        //M.dump();
        //pm.add(llvm::createBasicAAWrapperPass());
        /*
        pm.add(llvm::createAAResultsWrapperPass());
        auto MSSA = new llvm::MemorySSAWrapperPass();
        pm.add(MSSA);
        pm.add(new llvm::MemorySSAPrinterLegacyPass());
        pm.run(M);
        */

        llvm::FunctionAnalysisManager FM;
        llvm::MemorySSAAnalysis MSSA;
        for (auto& F: M) {
            auto mssa = MSSA.run(F, FM);
            /*
            llvm::errs() << "-- " << F.getName() << "\n";

            auto& m = MSSA->getMSSA();
            auto MWalker = m.getWalker();
            for (auto& B :F) {
                for (auto& I : B) {
                    if (llvm::isa<llvm::LoadInst>(&I) || llvm::isa<llvm::StoreInst>(&I)) {
                        auto access = MWalker->getClobberingMemoryAccess(&I);
                        if (access) {
                            llvm::errs() << I << "\n";
                            access->dump();
                        }
                    }
                }
            }
            */
        }
    }
};

} // namespace dg

#endif //  _LLVM_DEF_USE_ANALYSIS_H_
