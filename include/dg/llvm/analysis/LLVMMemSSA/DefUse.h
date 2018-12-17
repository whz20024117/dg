#ifndef _LLVM_MEMORY_SSA_ANALYSIS_H_
#define _LLVM_MEMORY_SSA_ANALYSIS_H_

#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Value.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>

#include <llvm/IR/LegacyPassManager.h>

namespace dg {

class LLVMMemorySSAAnalysis
{
		// code inspired by llvm-opt-fuzzer
		llvm::PassBuilder PB;

		llvm::LoopAnalysisManager LAM{true};
		llvm::FunctionAnalysisManager FAM{true};
		llvm::CGSCCAnalysisManager CGAM{true};
		llvm::ModulePassManager MPM{true};
		llvm::FunctionPassManager FPM{true};
		llvm::ModuleAnalysisManager MAM{true};

public:
    LLVMMemorySSAAnalysis(llvm::Module& M) {
        using namespace llvm;

		FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });
		PB.registerModuleAnalyses(MAM);
		PB.registerCGSCCAnalyses(CGAM);
		PB.registerFunctionAnalyses(FAM);
		PB.registerLoopAnalyses(LAM);
		PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

/*
		auto Err = PB.parsePassPipeline(MPM, "memoryssa",
										false, false);
		if (Err) {
			// Only fail with assert above, otherwise ignore the parsing error.
			errs() << "ERROR: " << toString(std::move(Err)) << "\n";
			assert(0);
			//consumeError(std::move(Err));
			return;
		}
*/
		// Run passes
		//MPM.run(M, MAM);

        llvm::MemorySSAAnalysis MSSA;
        for (auto& F: M) {
			if (F.isDeclaration())
				continue;
            auto mssa = MSSA.run(F, FAM);
			auto& ssa = mssa.getMSSA();
            llvm::errs() << "-- " << F.getName() << "\n";
            ssa.print(errs());

            auto MWalker = ssa.getWalker();
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
        }
    }
};

} // namespace dg

#endif //  _LLVM_DEF_USE_ANALYSIS_H_
