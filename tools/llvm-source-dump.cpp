#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stack>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include <dg/util/SilenceLLVMWarnings.h>
SILENCE_LLVM_WARNINGS_PUSH
#include <llvm/Config/llvm-config.h>

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR < 5))
#include <llvm/DebugInfo.h>
#else
#include <llvm/DebugInfo/DIContext.h>
#endif

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
SILENCE_LLVM_WARNINGS_POP

using namespace llvm;

// lines with matching braces
std::vector<std::pair<unsigned, unsigned>> matching_braces;
// mapping line->index in matching_braces
std::map<unsigned, unsigned> nesting_structure;
// line per file
std::map<std::string, std::set<unsigned>> line_dict;

static void get_lines_from_module(const Module *M) {
    // iterate over all instructions
    for (const Function &F : *M) {
        for (const BasicBlock &B : F) {
            for (const Instruction &I : B) {
                const DebugLoc &Loc = I.getDebugLoc();
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
    llvm::Module *M;
    LLVMContext context;
    SMDiagnostic SMD;

    const char *module = nullptr;

    if (argc < 2 || argc > 3) {
        errs() << "Usage: module [source_code]\n";
        return 1;
    }

    module = argv[1];

#if ((LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 5))
    M = llvm::ParseIRFile(module, SMD, context);
#else
    auto _M = llvm::parseIRFile(module, SMD, context);
    // _M is unique pointer, we need to get Module *
    M = _M.get();
#endif

    if (!M) {
        llvm::errs() << "Failed parsing '" << module << "' file:\n";
        SMD.print(argv[0], errs());
        return 1;
    }

    // FIXME find out if we have debugging info at all
    // no difficult machineris - just find out
    // which lines are in our module and print them
    get_lines_from_module(M);


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
    
    

    return 0;
}
