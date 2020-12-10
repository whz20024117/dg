#ifndef DG_LLVM_CALLGRAPH_H_
#define DG_LLVM_CALLGRAPH_H_

#include <memory>
#include <vector>

#include <dg/util/SilenceLLVMWarnings.h>
SILENCE_LLVM_WARNINGS_PUSH
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
SILENCE_LLVM_WARNINGS_POP

#include "dg/ADT/HashMap.h"
#include "dg/ADT/Queue.h"
#include "dg/ADT/SetQueue.h"
#include "dg/CallGraph/CallGraph.h"
#include "dg/PointerAnalysis/PSNode.h"
#include "dg/llvm/PointerAnalysis/PointerAnalysis.h"

namespace dg {
namespace pta {
class PSNode;
}

namespace llvmdg {

class CallGraphImpl {
  public:
    using FuncVec = std::vector<const llvm::Function *>;

    virtual ~CallGraphImpl() = default;

    // XXX: return iterators...

    ///
    /// \brief functions
    /// \return Functions that are in the callgraph. Note that there may be
    /// functions missing if the callgraph is being build lazily
    /// (you may force building CG by build() method).
    ///
    virtual FuncVec functions() const = 0;
    virtual FuncVec callers(const llvm::Function *) const = 0;
    virtual FuncVec callees(const llvm::Function *) const = 0;
    virtual bool calls(const llvm::Function *,
                       const llvm::Function *) const = 0;

    // trigger building the CG (can be used to force building when CG is
    // constructed on demand)
    virtual void build() {}
};

///
/// Callgraph that re-uses the Call graph built during pointer analysis from DG
///
class DGCallGraphImpl : public CallGraphImpl {
    const GenericCallGraph<PSNode *> &_cg;
    std::map<const llvm::Function *, PSNode *> _mapping;

    const llvm::Function *getFunFromNode(PSNode *n) const {
        auto f = n->getUserData<llvm::Function>();
        assert(f && "Invalid data in a node");
        return f;
    }

  public:
    DGCallGraphImpl(const dg::GenericCallGraph<PSNode *> &cg) : _cg(cg) {
        for (auto &it : _cg) {
            _mapping[getFunFromNode(it.first)] = it.first;
        }
    }

    FuncVec functions() const override {
        FuncVec ret;
        for (auto &it : _mapping) {
            ret.push_back(it.first);
        }
        return ret;
    }

    FuncVec callers(const llvm::Function *F) const override {
        FuncVec ret;
        auto it = _mapping.find(F);
        if (it == _mapping.end())
            return ret;
        auto fnd = _cg.get(it->second);
        for (auto *nd : fnd->getCallers()) {
            ret.push_back(getFunFromNode(nd->getValue()));
        }
        return ret;
    }

    FuncVec callees(const llvm::Function *F) const override {
        FuncVec ret;
        auto it = _mapping.find(F);
        if (it == _mapping.end())
            return ret;
        auto fnd = _cg.get(it->second);
        for (auto *nd : fnd->getCalls()) {
            ret.push_back(getFunFromNode(nd->getValue()));
        }
        return ret;
    }

    bool calls(const llvm::Function *F,
               const llvm::Function *what) const override {
        auto it = _mapping.find(F);
        if (it == _mapping.end())
            return false;
        auto it2 = _mapping.find(what);
        if (it2 == _mapping.end())
            return false;
        auto fn1 = _cg.get(it->second);
        auto fn2 = _cg.get(it2->second);
        if (fn1 && fn2) {
            return fn1->calls(fn2);
        }
        return false;
    };
};

///
/// Callgraph that is built based on the results of pointer analysis.
/// This class has been superseeded by LazyLLVMCallGraph.
///
class LLVMPTACallGraphImpl : public CallGraphImpl {
    GenericCallGraph<const llvm::Function *> _cg{};
    const llvm::Module *_module;
    LLVMPointerAnalysis *_pta;

    void
    processBBlock(const llvm::Function *parent, const llvm::BasicBlock &B,
                  ADT::SetQueue<QueueFIFO<const llvm::Function *>> &queue) {
        for (auto &I : B) {
            if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
#if LLVM_VERSION_MAJOR >= 8
                auto pts = _pta->getLLVMPointsTo(C->getCalledOperand());
#else
                auto pts = _pta->getLLVMPointsTo(C->getCalledValue());
#endif
                for (const auto &ptr : pts) {
                    auto *F = llvm::dyn_cast<llvm::Function>(ptr.value);
                    if (!F)
                        continue;

                    _cg.addCall(parent, F);
                    queue.push(F);
                }
            }
        }
    }

    void _build() {
        auto *entry = _module->getFunction(_pta->getOptions().entryFunction);
        assert(entry && "Entry function not found");
        _cg.createNode(entry);

        ADT::SetQueue<QueueFIFO<const llvm::Function *>> queue;
        queue.push(entry);

        while (!queue.empty()) {
            auto *cur = queue.pop();
            for (auto &B : *cur) {
                processBBlock(cur, B, queue);
            }
        }
    }

  public:
    LLVMPTACallGraphImpl(const llvm::Module *m, LLVMPointerAnalysis *pta)
            : _module(m), _pta(pta) {
        _build();
    }

    FuncVec functions() const override {
        FuncVec ret;
        for (auto &it : _cg) {
            ret.push_back(it.first);
        }
        return ret;
    }

    FuncVec callers(const llvm::Function *F) const override {
        FuncVec ret;
        auto fnd = _cg.get(F);
        for (auto *nd : fnd->getCallers()) {
            ret.push_back(nd->getValue());
        }
        return ret;
    }

    FuncVec callees(const llvm::Function *F) const override {
        FuncVec ret;
        auto fnd = _cg.get(F);
        for (auto *nd : fnd->getCalls()) {
            ret.push_back(nd->getValue());
        }
        return ret;
    }

    bool calls(const llvm::Function *F,
               const llvm::Function *what) const override {
        auto fn1 = _cg.get(F);
        auto fn2 = _cg.get(what);
        if (fn1 && fn2) {
            return fn1->calls(fn2);
        }
        return false;
    };
};

inline bool funHasAddressTaken(const llvm::Function *fun) {
    using namespace llvm;
    for (auto &use : fun->uses()) {
#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR < 5))
        Value *user = *use;
#else
        Value *user = use.getUser();
#endif
        // FIXME: we can detect more cases as false
        if (auto *C = dyn_cast<CallInst>(user)) {
            if (fun == C->getCalledFunction()) {
                continue;
            }
        }
        return true;
    }
    return false;
}

// FIXME: copied from llvm-utils.h
namespace {
inline bool isPointerOrIntegerTy(const llvm::Type *Ty) {
    return Ty->isPointerTy() || Ty->isIntegerTy();
}

// can the given function be called by the given call inst?
enum class CallCompatibility {
    STRICT,       // require full compatibility
    LOOSE,        // ignore some incompatible patterns that usually work
                  // in practice, e.g., calling a function of 2 arguments
                  // with 3 arguments.
    MATCHING_ARGS // check only that matching arguments are compatible,
                  // ignore the number of arguments, etc.
};

inline bool
callIsCompatible(const llvm::Function *F, const llvm::CallInst *CI,
                 CallCompatibility policy = CallCompatibility::LOOSE) {
    using namespace llvm;

    if (policy != CallCompatibility::MATCHING_ARGS) {
        if (F->isVarArg()) {
            if (F->arg_size() > CI->getNumArgOperands()) {
                return false;
            }
        } else if (F->arg_size() != CI->getNumArgOperands()) {
            if (policy == CallCompatibility::STRICT ||
                F->arg_size() > CI->getNumArgOperands()) {
                // too few arguments
                return false;
            }
        }

        if (!F->getReturnType()->canLosslesslyBitCastTo(CI->getType())) {
            // it showed up that the loosless bitcast is too strict
            // alternative since we can use the constexpr castings
            if (!(isPointerOrIntegerTy(F->getReturnType()) &&
                  isPointerOrIntegerTy(CI->getType()))) {
                return false;
            }
        }
    }

    size_t idx = 0;
    auto max_idx = CI->getNumArgOperands();
    for (auto A = F->arg_begin(), E = F->arg_end(); idx < max_idx && A != E;
         ++A, ++idx) {
        Type *CTy = CI->getArgOperand(idx)->getType();
        Type *ATy = A->getType();

        if (!(isPointerOrIntegerTy(CTy) && isPointerOrIntegerTy(ATy)))
            if (!CTy->canLosslesslyBitCastTo(ATy)) {
                return false;
            }
    }

    return true;
}
} // anonymous namespace

///
/// \brief The LazyLLVMCallGraph class
///
/// A callgraph that is being build lazily based on user queries.
/// It can use pointer analysis, but it is sound even without it
/// (by overapproximating).
class LazyLLVMCallGraph : public CallGraphImpl {
    GenericCallGraph<const llvm::Function *> _cg{};
    const llvm::Module *_module;
    LLVMPointerAnalysis *_pta;

    using FuncVec = CallGraphImpl::FuncVec;
    // resolved function pointers
    dg::HashMap<const llvm::CallInst *, FuncVec> _funptrs;
    std::vector<const llvm::Function *> _address_taken;
    bool _address_taken_initialized{false};

    inline const llvm::Value *_getCalledValue(const llvm::CallInst *C) const {
#if LLVM_VERSION_MAJOR >= 8
        return C->getCalledOperand()->stripPointerCasts();
#else
        return C->getCalledValue()->stripPointerCasts();
#endif
    }

    void _initializeAddressTaken() {
        assert(!_address_taken_initialized);
        _address_taken_initialized = true;

        for (auto &F : *_module) {
            if (F.isDeclaration())
                continue;
            if (funHasAddressTaken(&F)) {
                _address_taken.push_back(&F);
            }
        }
    }

    FuncVec _getAddressTakenFuns(const llvm::CallInst *C) {
        if (!_address_taken_initialized)
            _initializeAddressTaken();
        assert(_address_taken_initialized);

        FuncVec ret;
        // filter out compatible functions
        for (auto *fun : _address_taken) {
            if (callIsCompatible(fun, C)) {
                ret.push_back(fun);
            }
        }
        return ret;
    }

    // we pass also the call inst to be able to filter out incompatible
    // functions
    FuncVec _getCalledFunctions(const llvm::CallInst *C,
                                const llvm::Value *val) {
        if (_pta) {
            FuncVec ret;
            auto pts = _pta->getLLVMPointsTo(val);
            for (const auto &ptr : pts) {
                if (auto *fun = llvm::dyn_cast<llvm::Function>(ptr.value)) {
                    if (callIsCompatible(fun, C)) {
                        ret.push_back(fun);
                    }
                }
            }
            return ret;
        } else {
            return _getAddressTakenFuns(C);
        }
    }

    FuncVec _getCalledFunctions(const llvm::CallInst *C) {
        auto *callval = _getCalledValue(C);
        assert(!llvm::isa<llvm::Function>(callval) &&
               "This method should be called on funptr");
        auto *thisf = C->getParent()->getParent();
        auto ret = _getCalledFunctions(C, callval);
        for (auto *f : ret)
            _cg.addCall(thisf, f);
        return ret;
    }

    void _populateCalledFunctions(const llvm::Function *fun) {
        for (auto &I : llvm::instructions(fun)) {
            if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
                getCalledFunctions(C);
            }
        }
    }

    void
    processBBlock(const llvm::Function *parent, const llvm::BasicBlock &B,
                  ADT::SetQueue<QueueFIFO<const llvm::Function *>> &queue) {
        assert(_pta && "This method can be used only with PTA");
        for (auto &I : B) {
            if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
                auto pts = _pta->getLLVMPointsTo(_getCalledValue(C));
                for (const auto &ptr : pts) {
                    if (auto *F = llvm::dyn_cast<llvm::Function>(ptr.value)) {
                        _cg.addCall(parent, F);
                        queue.push(F);
                    }
                }
            }
        }
    }

  public:
    LazyLLVMCallGraph(const llvm::Module *m, LLVMPointerAnalysis *pta = nullptr)
            : _module(m), _pta(pta) {}

    // TODO: add this method to general interface?
    const FuncVec &getCalledFunctions(const llvm::CallInst *C) {
        auto *val = _getCalledValue(C);
        if (auto *fun = llvm::dyn_cast<llvm::Function>(val)) {
            static FuncVec retval;
            retval.clear();
            retval.push_back(fun);
            _cg.addCall(C->getParent()->getParent(), fun);
            return retval;
        }

        auto it = _funptrs.find(C);
        if (it != _funptrs.end()) {
            return it->second;
        }

        // FIXME: do not do multiple lookups
        _funptrs[C] = _getCalledFunctions(C);
        return _funptrs[C];
    }

    FuncVec functions() const override {
        FuncVec ret;
        for (auto &it : _cg) {
            ret.push_back(it.first);
        }
        return ret;
    }

    FuncVec callers(const llvm::Function *F) const override {
        FuncVec ret;
        auto fnd = _cg.get(F);
        // if (!fnd)
        //    _populateCalledFunctions(F);
        //
        for (auto *nd : fnd->getCallers()) {
            ret.push_back(nd->getValue());
        }
        return ret;
    }

    FuncVec callees(const llvm::Function *F) const override {
        FuncVec ret;
        auto fnd = _cg.get(F);
        for (auto *nd : fnd->getCalls()) {
            ret.push_back(nd->getValue());
        }
        return ret;
    }

    bool calls(const llvm::Function *F,
               const llvm::Function *what) const override {
        auto fn1 = _cg.get(F);
        auto fn2 = _cg.get(what);
        if (fn1 && fn2) {
            return fn1->calls(fn2);
        }
        return false;
    };

    // trigger build
    void build() override {
        if (_pta) { // build only reachable functions
            auto *entry =
                    _module->getFunction(_pta->getOptions().entryFunction);
            assert(entry && "Entry function not found");
            _cg.createNode(entry);

            ADT::SetQueue<QueueFIFO<const llvm::Function *>> queue;
            queue.push(entry);

            while (!queue.empty()) {
                auto *cur = queue.pop();
                for (auto &B : *cur) {
                    processBBlock(cur, B, queue);
                }
            }
        } else {
            for (auto &F : *_module) {
                if (F.isDeclaration())
                    continue;
                _populateCalledFunctions(&F);
            }
        }
    }
};

class CallGraph {
    std::unique_ptr<CallGraphImpl> _impl;

  public:
    using FuncVec = CallGraphImpl::FuncVec;

    CallGraph(GenericCallGraph<PSNode *> &cg)
            : _impl(new DGCallGraphImpl(cg)) {}
    CallGraph(const llvm::Module *m, LLVMPointerAnalysis *pta, bool lazy = true)
            : _impl(lazy ? static_cast<CallGraphImpl *>(
                                   new LazyLLVMCallGraph(m, pta))
                         : static_cast<CallGraphImpl *>(
                                   new LLVMPTACallGraphImpl(m, pta))) {}
    CallGraph(const llvm::Module *m) : _impl(new LazyLLVMCallGraph(m)) {}

    ///
    /// Get all functions in this call graph
    ///
    FuncVec functions() const { return _impl->functions(); }
    ///
    /// Get callers of a function
    ///
    FuncVec callers(const llvm::Function *F) const {
        return _impl->callers(F);
    };
    ///
    /// Get functions called from the given function
    ///
    FuncVec callees(const llvm::Function *F) const {
        return _impl->callees(F);
    };
    ///
    /// Return true if function 'F' calls 'what'
    ///
    bool calls(const llvm::Function *F, const llvm::Function *what) const {
        return _impl->calls(F, what);
    };

    void build() { _impl->build(); }
};

} // namespace llvmdg
} // namespace dg

#endif
