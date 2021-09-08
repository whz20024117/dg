#include <cassert>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <llvm/Config/llvm-config.h>

#if (LLVM_VERSION_MAJOR < 3)
#error "Unsupported version of LLVM"
#endif

#include "dg/tools/llvm-slicer-opts.h"
#include "dg/tools/llvm-slicer-utils.h"
#include "dg/tools/llvm-slicer.h"
#include "git-version.h"

#include <dg/util/SilenceLLVMWarnings.h>
SILENCE_LLVM_WARNINGS_PUSH
#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 7
#include <llvm/IR/LLVMContext.h>
#endif
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
SILENCE_LLVM_WARNINGS_POP

#include "dg/ADT/Queue.h"
#include "dg/util/debug.h"

using namespace dg;

using dg::LLVMDataDependenceAnalysisOptions;
using dg::LLVMPointerAnalysisOptions;
using llvm::errs;

using AnnotationOptsT =
        dg::debug::LLVMDGAssemblyAnnotationWriter::AnnotationOptsT;

llvm::cl::opt<bool> enable_debug(
        "dbg", llvm::cl::desc("Enable debugging messages (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> print_line_num(
        "linenum", llvm::cl::desc("Print comma-seperated line number instead for Python Wrapper (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> should_verify_module(
        "dont-verify", llvm::cl::desc("Verify sliced module (default=true)."),
        llvm::cl::init(true), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> statistics(
        "statistics",
        llvm::cl::desc("Print statistics about slicing (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> criteria_are_next_instr(
        "criteria-are-next-instr",
        llvm::cl::desc(
                "Assume that slicing criteria are not the call-sites\n"
                "of the given function, but the instructions that\n"
                "follow the call. I.e. the call is used just to mark\n"
                "the instruction.\n"
                "E.g. for 'crit' being set as the criterion, slicing critera "
                "are all instructions that follow any call of 'crit'.\n"),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));


#ifndef USING_SANITIZERS
void setupStackTraceOnError(int argc, char *argv[]) {
#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 9
    llvm::sys::PrintStackTraceOnErrorSignal();
#else
    llvm::sys::PrintStackTraceOnErrorSignal(llvm::StringRef());
#endif
    llvm::PrettyStackTraceProgram X(argc, argv);
}
#else
void setupStackTraceOnError(int, char **) {}
#endif // not USING_SANITIZERS


std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &context,
                                          const SlicerOptions &options) {
    llvm::SMDiagnostic SMD;

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    auto _M = llvm::ParseIRFile(options.inputFile, SMD, context);
    auto M = std::unique_ptr<llvm::Module>(_M);
#else
    auto M = llvm::parseIRFile(options.inputFile, SMD, context);
    // _M is unique pointer, we need to get Module *
#endif

    if (!M) {
        SMD.print("llvm-slicer", llvm::errs());
    }

    return M;
}

// class MyPDG {
// public:
//     typedef std::pair<llvm::Value *, llvm::Value *> MyEdge;
//     std::set<LLVMNode *> nodes;
    
//     std::set<MyEdge> cd_edges;
//     std::set<MyEdge> dd_edges;
//     std::set<MyEdge> id_edges;
//     std::set<MyEdge> use_edges;
//     std::set<MyEdge> user_edges;



// };

enum EdgeFlag {
    CD_EDGE,
    DD_EDGE,
    ID_EDGE,
    USE_EDGE,
    USER_EDGE
};

// max_depth = 1 means only add criteria node. Make sure max_depth is at least 2
void walkDFS(LLVMNode * node, std::map<llvm::Value *, LLVMNode *> &node_in_slice, EdgeFlag edgeflag, int max_depth, int depth = 1) {

    if (depth > max_depth)
        return;

    if (node_in_slice.find(node->getKey()) != node_in_slice.end())
        return;

    if (edgeflag == CD_EDGE) {
        for (auto eit = node->control_begin(); eit != node->control_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }

        for (auto eit = node->rev_control_begin(); eit != node->rev_control_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }
    }

    if (edgeflag == DD_EDGE) {
        for (auto eit = node->data_begin(); eit != node->data_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }

        for (auto eit = node->rev_data_begin(); eit != node->rev_data_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }
    }

    if (edgeflag == ID_EDGE) {
        for (auto eit = node->interference_begin(); eit != node->interference_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }

        for (auto eit = node->rev_interference_begin(); eit != node->rev_interference_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }
    }

    if (edgeflag == USE_EDGE) {
        for (auto eit = node->use_begin(); eit != node->use_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }
    }

    if (edgeflag == USER_EDGE) {
        for (auto eit = node->user_begin(); eit != node->user_end(); eit++) {
            dg::LLVMNode * n = *eit;
            node_in_slice[n->getKey()] = *eit;
            walkDFS(*eit, node_in_slice, edgeflag, max_depth, depth + 1);
        }
    }

    if (node->subgraphsNum() > 0) { // this is a call site
        dg::LLVMDGParameters * para = node->getParameters();
        if (!para)
            return;

        // //TODO: iterate through para
        // for (auto p = para->begin(); p != para->end();)
    }
       
}

// void update_mypdg(LLVMDependenceGraph &dg, MyPDG &mypdg) {
//     std::vector<llvm::Value *> candidate_nodes;

//     for (auto &G : dg.getModule()->globals()) {
//         candidate_nodes.push_back(&G);
//     }


// }


std::ostream &printLLVMVal(std::ostream &os,
                                         const llvm::Value *val) {
    if (!val) {
        os << "(null)";
        return os;
    }

    std::ostringstream ostr;
    llvm::raw_os_ostream ro(ostr);

    if (llvm::isa<llvm::Function>(val)) {
        ro << "FUNC " << val->getName();
    } else if (auto B = llvm::dyn_cast<llvm::BasicBlock>(val)) {
        ro << B->getParent()->getName() << "::\n";
        ro << "label " << val->getName();
    } else if (auto I = llvm::dyn_cast<llvm::Instruction>(val)) {
        const auto B = I->getParent();
        if (B) {
            ro << B->getParent()->getName() << "::\n";
        } else {
            ro << "<null>::\n";
        }
        ro << *val;
    } else {
        ro << *val;
    }

    ro.flush();

    // break the string if it is too long
    std::string str = ostr.str();
    if (str.length() > 100) {
        str.resize(40);
    }

    // escape the "
    size_t pos = 0;
    while ((pos = str.find('"', pos)) != std::string::npos) {
        str.replace(pos, 1, "\\\"");
        // we replaced one char with two, so we must shift after the new "
        pos += 2;
    }

    os << str;

    return os;
}


int main(int argc, char *argv[]) {
    setupStackTraceOnError(argc, argv);

#if ((LLVM_VERSION_MAJOR >= 6))
    llvm::cl::SetVersionPrinter(
            [](llvm::raw_ostream &) { printf("%s\n", GIT_VERSION); });
#else
    llvm::cl::SetVersionPrinter([]() { printf("%s\n", GIT_VERSION); });
#endif

    SlicerOptions options =
            parseSlicerOptions(argc, argv, true /* require crit*/);

    if (enable_debug) {
        DBG_ENABLE();
    }

    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> M = parseModule(context, options);
    if (!M) {
        llvm::errs() << "Failed parsing '" << options.inputFile << "' file:\n";
        return 1;
    }

    if (!M->getFunction(options.dgOptions.entryFunction)) {
        llvm::errs() << "The entry function not found: "
                     << options.dgOptions.entryFunction << "\n";
        return 1;
    }

    // remove unused from module, we don't need that
    ModuleWriter writer(options, M.get());
    writer.removeUnusedFromModule();

    /// ---------------
    // slice the code
    /// ---------------

    ::Slicer slicer(M.get(), options);
    if (!slicer.buildDG(true)) {
        errs() << "ERROR: Failed building DG\n";
        return 1;
    }

    std::set<LLVMNode *> criteria_nodes;
    if (!getSlicingCriteriaNodes(slicer.getDG(), options.slicingCriteria,
                                 options.legacySlicingCriteria,
                                 options.legacySecondarySlicingCriteria,
                                 criteria_nodes, criteria_are_next_instr)) {
        llvm::errs() << "ERROR: Failed finding slicing criteria: '"
                     << options.slicingCriteria << "'\n";

        return 1;
    }

    if (criteria_nodes.empty()) {
        llvm::errs() << "No reachable slicing criteria: '"
                     << options.slicingCriteria << "'\n";
        return 1;
    }

    // We do slice in our way
    std::map<llvm::Value *, LLVMNode *> sliced_nodes;
    int max_depth = 50;

    auto & funcs = getConstructedFunctions();
    dg::ADT::QueueFIFO<BBlock<LLVMNode> *> bbqueue;

    // for (auto nit : criteria_nodes) {
    //     for (auto it : funcs) {
    //         auto nodes = it.second->getNodes();
    //         for (auto n : *nodes) {
    //             bbqueue.push(n.second->getBBlock());

    //             while (!bbqueue.empty()) {
    //                 auto bb = bbqueue.pop();
    //                 if (bb->getCallSitesNum() > 0) {
    //                     for (auto cit : n.second->getBBlock()->getCallSites()) {
    //                         for (auto subdg : cit->getSubgraphs()) {
    //                             // llvm::errs() << "---------------------------------------" << "\n";
    //                             if (subdg->getEntryBB() == nit->getBBlock()) {
    //                                 llvm::errs() << "##############################################3" << "\n";
    //                             }
    //                         }
    //                     }
    //                 }
                    
    //                 for (auto E : bb->successors()) {
    //                     bbqueue.push(E.target);
    //                 }
    //             }
                
                
    //             // llvm::errs() << n.second->getBBlock()->getCallSitesNum() << "\n";
    //         }
    //     }
    // }

    // for (auto n : criteria_nodes) {
        
    //         bbqueue.push(n->getBBlock());

    //         while (!bbqueue.empty()) {
    //             auto bb = bbqueue.pop();
    //             if (bb->getCallSitesNum() > 0) {
    //                 for (auto cit : n->getBBlock()->getCallSites()) {
    //                     for (auto subdg : cit->getSubgraphs()) {
    //                         llvm::errs() << "---------------------------------------" << "\n";
    //                         if (subdg->getEntryBB() == n->getBBlock()) {
    //                             llvm::errs() << "##############################################3" << "\n";
    //                         }
    //                     }
    //                 }
    //             }
                
    //             for (auto E : bb->successors()) {
    //                 bbqueue.push(E.target);
    //             }
    //         }
            
            
            // llvm::errs() << n.second->getBBlock()->getCallSitesNum() << "\n";
    // }
    
    
    
    for (auto nit : criteria_nodes) {
        std::cerr << nit->getKey() << ": ";
        printLLVMVal(std::cerr, nit ->getKey()); std::cerr << "\n";

        
        walkDFS(nit, sliced_nodes, CD_EDGE, max_depth);
        walkDFS(nit, sliced_nodes, DD_EDGE, max_depth);
        walkDFS(nit, sliced_nodes, ID_EDGE, max_depth);
        walkDFS(nit, sliced_nodes, USE_EDGE, max_depth);
        walkDFS(nit, sliced_nodes, USER_EDGE, max_depth);
    }

    for (auto nit : sliced_nodes) {
        llvm::errs() << nit.second->getBBlock()-> getCallSitesNum() << "\n";
    }

    for (auto nit : sliced_nodes) {
        std::cerr << nit.second->getKey() << ": ";
        printLLVMVal(std::cerr, nit.second->getKey());
        std::cerr << "\n";
    }
    

   

    // writer.removeUnusedFromModule();
    // writer.makeDeclarationsExternal();

    // get_lines_from_module(M.get());

    // for (auto &fit : line_dict){
    //     if (!get_nesting_structure(fit.first.c_str()))
    //         continue;
    //     /* fill in the lines with braces */
    //     /* really not efficient, but easy */
    //     auto &nesting_structure = nesting_structure_per_file[fit.first];
    //     auto &matching_braces = matching_braces_per_file[fit.first];

    //     size_t old_size;
    //     do {
    //         old_size = fit.second.size();
    //         std::set<unsigned> new_lines;

    //         for (unsigned i : fit.second) {
    //             new_lines.insert(i);
    //             auto it = nesting_structure.find(i);
    //             if (it != nesting_structure.end()) {
    //                 auto &pr = matching_braces[it->second];
    //                 new_lines.insert(pr.first);
    //                 new_lines.insert(pr.second);
    //             }
    //         }

    //         fit.second.swap(new_lines);
    //     } while (fit.second.size() > old_size);
    // }

    // // Print lines
    // if (!print_line_num){
    //     for (auto &fit : line_dict){
    //         // std::cout<< "FILE: " <<fit.first<<std::endl;
    //         std::ifstream ifs(fit.first.c_str());
    //         if (!ifs.is_open() || ifs.bad()) {
    //             errs() << "Failed opening given source file: " << fit.first << "\n";
    //             return -1;
    //         }

    //         print_lines(ifs, fit.second);
    //         ifs.close();
    //     }
    // } else {
    //     for (auto &fit : line_dict){
    //         std::cout << fit.first;
    //         for (auto l : fit.second) {
    //             std::cout << ',' << l;
    //         }
    //         std::cout << std::endl;
    //     }
    // }
    
}