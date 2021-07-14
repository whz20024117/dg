#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <stack>
#include <map>

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
#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR < 5))
#include <llvm/DebugInfo.h>
#else
#include <llvm/DebugInfo/DIContext.h>
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>

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

llvm::cl::opt<bool> should_verify_module(
        "dont-verify", llvm::cl::desc("Verify sliced module (default=true)."),
        llvm::cl::init(true), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> remove_unused_only(
        "remove-unused-only",
        llvm::cl::desc("Only remove unused parts of module (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

llvm::cl::opt<bool> statistics(
        "statistics",
        llvm::cl::desc("Print statistics about slicing (default=false)."),
        llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

// llvm::cl::opt<bool>
//         dump_dg("dump-dg",
//                 llvm::cl::desc("Dump dependence graph to dot (default=false)."),
//                 llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

// llvm::cl::opt<bool> dump_dg_only(
//         "dump-dg-only",
//         llvm::cl::desc("Only dump dependence graph to dot,"
//                        " do not slice the module (default=false)."),
//         llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

// llvm::cl::opt<bool> dump_bb_only(
//         "dump-bb-only",
//         llvm::cl::desc("Only dump basic blocks of dependence graph to dot"
//                        " (default=false)."),
//         llvm::cl::init(false), llvm::cl::cat(SlicingOpts));

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

llvm::cl::opt<std::string> annotationOpts(
        "annotate",
        llvm::cl::desc(
                "Save annotated version of module as a text (.ll).\n"
                "Options:\n"
                "  dd: data dependencies,\n"
                "  cd:control dependencies,\n"
                "  pta: points-to information,\n"
                "  memacc: memory accesses of instructions,\n"
                "  slice: comment out what is going to be sliced away).\n"
                "for more options, use comma separated list"),
        llvm::cl::value_desc("val1,val2,..."), llvm::cl::init(""),
        llvm::cl::cat(SlicingOpts));

static void maybe_print_statistics(llvm::Module *M,
                                   const char *prefix = nullptr) {
    if (!statistics)
        return;

    using namespace llvm;
    uint64_t inum, bnum, fnum, gnum;
    inum = bnum = fnum = gnum = 0;

    for (auto I = M->begin(), E = M->end(); I != E; ++I) {
        // don't count in declarations
        if (I->size() == 0)
            continue;

        ++fnum;

        for (const BasicBlock &B : *I) {
            ++bnum;
            inum += B.size();
        }
    }

    for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I)
        ++gnum;

    if (prefix)
        errs() << prefix;

    errs() << "Globals/Functions/Blocks/Instr.: " << gnum << " " << fnum << " "
           << bnum << " " << inum << "\n";
}

static AnnotationOptsT parseAnnotationOptions(const std::string &annot) {
    if (annot.empty())
        return {};

    AnnotationOptsT opts{};
    std::vector<std::string> lst = splitList(annot);
    for (const std::string &opt : lst) {
        if (opt == "dd")
            opts |= AnnotationOptsT::ANNOTATE_DD;
        else if (opt == "cd" || opt == "cda")
            opts |= AnnotationOptsT::ANNOTATE_CD;
        else if (opt == "dda" || opt == "du")
            opts |= AnnotationOptsT::ANNOTATE_DEF;
        else if (opt == "pta")
            opts |= AnnotationOptsT::ANNOTATE_PTR;
        else if (opt == "memacc")
            opts |= AnnotationOptsT::ANNOTATE_MEMORYACC;
        else if (opt == "slice" || opt == "sl" || opt == "slicer")
            opts |= AnnotationOptsT::ANNOTATE_SLICE;
    }

    return opts;
}

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

// lines with matching braces
std::vector<std::pair<unsigned, unsigned>> matching_braces;
// mapping line->index in matching_braces
std::map<unsigned, unsigned> nesting_structure;
// line per file
std::map<std::string, std::set<unsigned>> line_dict;

static void get_lines_from_module(const llvm::Module *M) {
    // iterate over all instructions
    for (const llvm::Function &F : *M) {
        for (const llvm::BasicBlock &B : F) {
            for (const llvm::Instruction &I : B) {
                const llvm::DebugLoc &Loc = I.getDebugLoc();
                // Make sure that the llvm istruction has corresponding dbg LOC

                if (Loc && Loc.getLine() > 0){
                    // llvm::DIScope *Scope = cast<llvm::DIScope>(Loc.getScope());
                    // std::string fileName = Scope->getFilename();
                    std::string fileName = F.getSubprogram()->getFile()->getFilename();
                    line_dict[fileName].insert(F.getSubprogram()->getLine());
                    line_dict[fileName].insert(Loc.getLine());
                }
            }
        }
    }

    // iterate over all globals
    /*
    for (const GlobalVariable& G : M->globals()) {
        const DebugLoc& Loc = G.getDebugLoc();
        lines.insert(Loc.getLine());
    }
    */
}

static bool get_nesting_structure(const char *source) {
    std::ifstream ifs(source);
    if (!ifs.is_open() || ifs.bad()) {
        errs() << "Failed opening given source file: " << source << "\n";
        return false;
        //abort();
    }

    char ch;
    unsigned cur_line = 1;
    unsigned idx;
    std::stack<unsigned> nesting;
    while (ifs.get(ch)) {
        switch (ch) {
        case '\n':
            ++cur_line;
            if (!nesting.empty())
                nesting_structure.emplace(cur_line, nesting.top());
            break;
        case '{':
            nesting.push(matching_braces.size());
            matching_braces.push_back({cur_line, 0});
            break;
        case '}':
            idx = nesting.top();
            assert(idx < matching_braces.size());
            assert(matching_braces[idx].second == 0);
            matching_braces[idx].second = cur_line;
            nesting.pop();
            break;
        default:
            break;
        }
    }

    ifs.close();
    return true;
}

static void print_lines(std::ifstream &ifs, std::set<unsigned> &lines) {
    char buf[1024];
    unsigned cur_line = 1;
    while (!ifs.eof()) {
        ifs.getline(buf, sizeof buf);

        if (lines.count(cur_line) > 0) {
            // std::cout << cur_line << ": ";
            std::cout << buf << "\n";
        }

        if (ifs.bad()) {
            errs() << "An error occured\n";
            break;
        }

        ++cur_line;
    }
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

    // // dump_dg_only implies dumg_dg
    // if (dump_dg_only) {
    //     dump_dg = true;
    // }

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

    maybe_print_statistics(M.get(), "Statistics before ");

    // remove unused from module, we don't need that
    ModuleWriter writer(options, M.get());
    writer.removeUnusedFromModule();

    if (remove_unused_only) {
        errs() << "[llvm-slicer] removed unused parts of module, exiting...\n";
        maybe_print_statistics(M.get(), "Statistics after ");
        return writer.saveModule(should_verify_module);
    }

    /// ---------------
    // slice the code
    /// ---------------

    ::Slicer slicer(M.get(), options);
    if (!slicer.buildDG()) {
        errs() << "ERROR: Failed building DG\n";
        return 1;
    }

    ModuleAnnotator annotator(options, &slicer.getDG(),
                              parseAnnotationOptions(annotationOpts));

    std::set<LLVMNode *> criteria_nodes;
    if (!getSlicingCriteriaNodes(slicer.getDG(), options.slicingCriteria,
                                 options.legacySlicingCriteria,
                                 options.legacySecondarySlicingCriteria,
                                 criteria_nodes, criteria_are_next_instr)) {
        llvm::errs() << "ERROR: Failed finding slicing criteria: '"
                     << options.slicingCriteria << "'\n";

        if (annotator.shouldAnnotate()) {
            slicer.computeDependencies();
            annotator.annotate();
        }

        return 1;
    }

    if (criteria_nodes.empty()) {
        llvm::errs() << "No reachable slicing criteria: '"
                     << options.slicingCriteria << "'\n";
        if (annotator.shouldAnnotate()) {
            slicer.computeDependencies();
            annotator.annotate();
        }

        if (!slicer.createEmptyMain()) {
            llvm::errs() << "ERROR: failed creating an empty main\n";
            return 1;
        }

        maybe_print_statistics(M.get(), "Statistics after ");
        return writer.cleanAndSaveModule(should_verify_module);
    }

    // mark nodes that are going to be in the slice
    if (!slicer.mark(criteria_nodes)) {
        llvm::errs() << "Finding dependent nodes failed\n";
        return 1;
    }

    // // print debugging llvm IR if user asked for it
    // if (annotator.shouldAnnotate())
    //     annotator.annotate(&criteria_nodes);

    // DGDumper dumper(options, &slicer.getDG(), dump_bb_only);
    // if (dump_dg) {
    //     dumper.dumpToDot();

    //     if (dump_dg_only)
    //         return 0;
    // }

    // slice the graph
    if (!slicer.slice()) {
        errs() << "ERROR: Slicing failed\n";
        return 1;
    }

    // if (dump_dg) {
    //     dumper.dumpToDot(".sliced.dot");
    // }

    // remove unused from module again, since slicing
    // could and probably did make some other parts unused
    maybe_print_statistics(M.get(), "Statistics after ");
    // return writer.cleanAndSaveModule(should_verify_module);

    writer.removeUnusedFromModule();
    writer.makeDeclarationsExternal();

    get_lines_from_module(M.get());

    for (auto &fit : line_dict){
        if (!get_nesting_structure(fit.first.c_str()))
            continue;
        /* fill in the lines with braces */
        /* really not efficient, but easy */
        size_t old_size;
        do {
            old_size = fit.second.size();
            std::set<unsigned> new_lines;

            for (unsigned i : fit.second) {
                new_lines.insert(i);
                auto it = nesting_structure.find(i);
                if (it != nesting_structure.end()) {
                    auto &pr = matching_braces[it->second];
                    new_lines.insert(pr.first);
                    new_lines.insert(pr.second);
                }
            }

            fit.second.swap(new_lines);
        } while (fit.second.size() > old_size);
    }

    // Print lines

    for (auto &fit : line_dict){
        std::cout<< "FILE: " <<fit.first<<std::endl;
        std::ifstream ifs(fit.first.c_str());
        if (!ifs.is_open() || ifs.bad()) {
            errs() << "Failed opening given source file: " << fit.first << "\n";
            continue;
        }

        print_lines(ifs, fit.second);
        ifs.close();
    }
}
