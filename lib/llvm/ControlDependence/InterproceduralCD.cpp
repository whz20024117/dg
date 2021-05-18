#include <dg/util/SilenceLLVMWarnings.h>
SILENCE_LLVM_WARNINGS_PUSH
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
SILENCE_LLVM_WARNINGS_POP

#include "dg/ADT/Queue.h"
#include "dg/llvm/PointerAnalysis/PointerAnalysis.h"
#include "dg/util/debug.h"
#include "llvm/ControlDependence/InterproceduralCD.h"

using namespace std;

namespace dg {
namespace llvmdg {

std::vector<const llvm::Function *>
LLVMInterprocCD::getCalledFunctions(const llvm::Value *v) {
    if (const auto *F = llvm::dyn_cast<llvm::Function>(v)) {
        return {F};
    }

    if (!PTA)
        return {};

    return dg::getCalledFunctions(v, PTA);
}

static inline bool hasNoSuccessors(const llvm::BasicBlock *bb) {
    return succ_begin(bb) == succ_end(bb);
}

void LLVMInterprocCD::computeFuncInfo(const llvm::Function *fun,
                                      std::set<const llvm::Function *> stack) {
    using namespace llvm;

    if (fun->isDeclaration() || hasFuncInfo(fun))
        return;

    DBG_SECTION_BEGIN(cda, "Computing no-return points for function "
                                   << fun->getName().str());

    auto &info = _funcInfos[fun];
    assert(hasFuncInfo(fun) && "Bug in hasFuncInfo");

    stack.insert(fun);

    //  compute nonreturning blocks (without successors
    //  and terminated with non-ret instruction
    //  and find calls (and other noret points) inside blocks
    for (const auto &B : *fun) {
        // no successors and does not return to caller
        // -- this is a point of no return :)
        if (hasNoSuccessors(&B) && !isa<ReturnInst>(B.getTerminator())) {
            info.noret.insert(B.getTerminator());
        }

        // process the calls in basic blocks
        for (const auto &I : B) {
            const auto *C = dyn_cast<CallInst>(&I);
            if (!C) {
                continue;
            }

#if LLVM_VERSION_MAJOR >= 8
            auto *val = C->getCalledOperand();
#else
            auto *val = C->getCalledValue();
#endif
            for (const auto *calledFun : getCalledFunctions(val)) {
                if (calledFun->isDeclaration())
                    continue;

                if (stack.count(calledFun) > 0) {
                    // recursive call
                    info.noret.insert(C);
                } else {
                    computeFuncInfo(calledFun, stack);
                    auto *fi = getFuncInfo(calledFun);
                    assert(fi && "Did not compute func info");
                    if (!fi->noret.empty()) {
                        info.noret.insert(C);
                    }
                }
            }
        }
    }

    // llvm::errs() << "Noret points of " << fun->getName() << "\n";
    // for (auto *nr : info.noret) {
    //    llvm::errs() << "  -> " << *nr << "\n";
    //}

    DBG_SECTION_END(cda, "Done computing no-return points for function "
                                 << fun->getName().str());
}

struct BlkInfo {
    // noret points in a block
    std::vector<llvm::Value *> noret;
};

void LLVMInterprocCD::computeCD(const llvm::Function *fun) {
    using namespace llvm;
    DBG_SECTION_BEGIN(cda, "Computing interprocedural CD for function "
                                   << fun->getName().str());

    // (1) initialize information about blocks
    // FIXME: we could do that in computeFuncInfo (add this mapping to FuncInfo)
    std::unordered_map<const llvm::BasicBlock *, BlkInfo> blkInfos;
    blkInfos.reserve(fun->size());

    for (const auto &B : *fun) {
        for (const auto &I : B) {
            const auto *C = dyn_cast<CallInst>(&I);
            if (!C) {
                continue;
            }

            bool maynoret = false;
#if LLVM_VERSION_MAJOR >= 8
            auto *val = C->getCalledOperand();
#else
            auto *val = C->getCalledValue();
#endif
            for (const auto *calledFun : getCalledFunctions(val)) {
                if (calledFun->isDeclaration())
                    continue;

                auto *fi = getFuncInfo(calledFun);
                assert(fi && "Do not have func info for a defined function");
                if (!fi->noret.empty()) {
                    maynoret = true;
                    break;
                }
            }
            if (maynoret)
                blkInfos[&B].noret.push_back(const_cast<Instruction *>(&I));
        }
    }

    // (2) compute control dependencies generated by calls
    // compute set of reachable nonret points untill fixpoint
    std::unordered_map<const llvm::BasicBlock *, std::set<llvm::Value *>> cds;
    cds.reserve(fun->size());

    ADT::QueueLIFO<const llvm::BasicBlock *> queue;
    for (auto &it : blkInfos) {
        for (const auto *succ : successors(it.first)) {
            queue.push(succ);
        }
    }

    // run until fixpoint
    while (!queue.empty()) {
        const auto *block = queue.pop();
        bool changed = false;
        for (const auto *pred : predecessors(block)) {
            // merge previously computed info
            auto cit = cds.find(pred);
            if (cit != cds.end()) {
                for (auto *nr : cit->second) {
                    changed |= cds[block].insert(nr).second;
                }
            }
            // merge info from blkInfos
            auto bit = blkInfos.find(pred);
            if (bit != blkInfos.end()) {
                for (auto *nr : bit->second.noret) {
                    changed |= cds[block].insert(nr).second;
                }
            }
        }

        if (changed) {
            for (const auto *succ : successors(block)) {
                queue.push(succ);
            }
        }
    }

    // (3) compute control dependencies
    for (auto &it : cds) {
        _blockCD[it.first] = std::move(it.second);
    }
    for (auto &it : blkInfos) {
        unsigned noretsidx = 0;
        auto &norets = it.second.noret;

        for (const auto &I : *it.first) {
            for (unsigned i = 0; i < noretsidx; ++i) {
                _instrCD[&I].insert(norets[i]);
            }
            if (noretsidx < norets.size() && &I == norets[noretsidx]) {
                ++noretsidx;
            }
        }
    }

    auto *fi = getFuncInfo(fun);
    assert(fi && "Whata?!");
    fi->hasCD = true;

    DBG_SECTION_END(cda, "Done computing interprocedural CD for function "
                                 << fun->getName().str());
}

} // namespace llvmdg
} // namespace dg
