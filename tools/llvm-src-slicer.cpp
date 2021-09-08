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

// lines with matching braces per file
typedef std::vector<std::pair<unsigned, unsigned>> MatchingBracesVector;
std::map<std::string, MatchingBracesVector> matching_braces_per_file;
// mapping line->index in matching_braces
std::map<std::string, std::map<unsigned, unsigned>> nesting_structure_per_file;
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

    std::map<unsigned, unsigned> nesting_structure;
    MatchingBracesVector matching_braces;

    char ch;
    unsigned cur_line = 1;
    unsigned idx;
    std::stack<unsigned> nesting;

    uint8_t src_flag = 0;
    enum SRCSTATE{
        C_COMMENT = 1 << 0,
        CPP_COMMENT = 1 << 1,
        IN_CHAR = 1 << 2,
        IN_STRING= 1 << 3
    };

    while (ifs.get(ch)) {

        if (ch == '\n')
            ++cur_line;

        /* Check special cases and Update flags */
        if ((src_flag & CPP_COMMENT)){
            if (ch != '\n'){ // end of cpp comments
                continue;
            } else {
                src_flag &= ~CPP_COMMENT;
            }
        }

        if ((src_flag & C_COMMENT)){
            if (ch == '*' && ifs.peek() == '/') { // end of c comments
                src_flag &= ~C_COMMENT;
                ifs.get();
            } else {
                continue;
            }
        }

        if (!(src_flag & (CPP_COMMENT | C_COMMENT)) && ch == '/'){ // Beginning of comments
            if (ifs.peek() == '/'){
                src_flag |= CPP_COMMENT;
                ifs.get();
                continue;
            }
            else if (ifs.peek() == '*'){
                src_flag |= C_COMMENT;
                ifs.get();
                continue;
            }
        }

        if (ch == '\\' && src_flag & (IN_CHAR | IN_STRING)) { // escapes in char and string
            ifs.get();
            continue;
        }

        if (ch == '\'' && !(src_flag & IN_STRING)) {
            src_flag ^= IN_CHAR;
        }

        if (ch == '"' && !(src_flag & IN_CHAR)) {
            src_flag ^= IN_STRING;
        }

        if (src_flag & (IN_CHAR | IN_STRING))
            continue;

        switch (ch) {
        case '\n':
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

    nesting_structure_per_file[source] = std::move(nesting_structure);
    matching_braces_per_file[source] = std::move(matching_braces);

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

    // llvm::LLVMContext context2;
    // std::unique_ptr<llvm::Module> M2 = parseModule(context2, options);

    // printf("#############################%x\n", &context);
    // printf("#############################%x\n", &context2);

    maybe_print_statistics(M.get(), "Statistics before ");

    // remove unused from module, we don't need that
    ModuleWriter writer(options, M.get());
    writer.removeUnusedFromModule();

    /// ---------------
    // slice the code
    /// ---------------

    ::Slicer slicer(M.get(), options);
    if (!slicer.buildDG()) {
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

    // mark nodes that are going to be in the slice
    if (!slicer.mark(criteria_nodes)) {
        llvm::errs() << "Finding dependent nodes failed\n";
        return 1;
    }

    // slice the graph
    if (!slicer.slice()) {
        errs() << "ERROR: Slicing failed\n";
        return 1;
    }

    maybe_print_statistics(M.get(), "Statistics after ");

    writer.removeUnusedFromModule();
    writer.makeDeclarationsExternal();

    get_lines_from_module(M.get());

    for (auto &fit : line_dict){
        if (!get_nesting_structure(fit.first.c_str()))
            continue;
        /* fill in the lines with braces */
        /* really not efficient, but easy */
        auto &nesting_structure = nesting_structure_per_file[fit.first];
        auto &matching_braces = matching_braces_per_file[fit.first];

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
    if (!print_line_num){
        for (auto &fit : line_dict){
            // std::cout<< "FILE: " <<fit.first<<std::endl;
            std::ifstream ifs(fit.first.c_str());
            if (!ifs.is_open() || ifs.bad()) {
                errs() << "Failed opening given source file: " << fit.first << "\n";
                return -1;
            }

            print_lines(ifs, fit.second);
            ifs.close();
        }
    } else {
        for (auto &fit : line_dict){
            std::cout << fit.first;
            for (auto l : fit.second) {
                std::cout << ',' << l;
            }
            std::cout << std::endl;
        }
    }
    
}
