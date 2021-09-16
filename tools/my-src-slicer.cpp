#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include <dg/util/SilenceLLVMWarnings.h>
SILENCE_LLVM_WARNINGS_PUSH
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>

#if LLVM_VERSION_MAJOR >= 4
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#else
#include <llvm/Bitcode/ReaderWriter.h>
#endif
SILENCE_LLVM_WARNINGS_POP

#include "dg/PointerAnalysis/Pointer.h"
#include "dg/PointerAnalysis/PointerAnalysisFI.h"
#include "dg/PointerAnalysis/PointerAnalysisFS.h"

#include "dg/llvm/DataDependence/DataDependence.h"
#include "dg/llvm/PointerAnalysis/PointerAnalysis.h"

#include "dg/tools/TimeMeasure.h"
#include "dg/tools/llvm-slicer-opts.h"
#include "dg/tools/llvm-slicer-utils.h"
#include "dg/util/debug.h"

#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>

using namespace dg;
using llvm::errs;

llvm::cl::opt<std::string> my_criteria("mysc", 
    llvm::cl::desc("My slicing criteria at source-level. Use: -mysc <filename>#<line>#<col>")
);
llvm::cl::opt<std::string> walk_depth("wd", 
    llvm::cl::desc("Max depths of walks on the graph"),
    llvm::cl::init("15")
);
llvm::cl::opt<std::string> walk_depth_interproc("wdi", 
    llvm::cl::desc("Max depths of interprocedule walks on the graph"),
    llvm::cl::init("1")
);


using VariablesMapTy = std::map<const llvm::Value *, CVariableDecl>;
VariablesMapTy allocasToVars(const llvm::Module &M);
VariablesMapTy valuesToVars;

class MyNode;
using MyEdgeT = std::set<MyNode *>;
class MyNodeKey {
public:
    uint32_t linenum;
    uint32_t colnum;

    std::string filename;

    bool operator==(const MyNodeKey& key) const {
        return filename == key.filename && linenum == key.linenum && colnum == key.colnum;
    }
};

class MyNode {
public:
    MyEdgeT dd_edge;
    MyEdgeT rev_dd_edge;
    MyEdgeT cd_edge;
    MyEdgeT rev_cd_edge;

    MyNodeKey key;

    std::string fun_name;

    MyNode(std::string file, uint32_t line, uint32_t col) {
        key.filename = file;
        key.linenum = line;
        key.colnum = col;
    }

    int add_dd_successor(MyNode* node) {
        if (!node)
            return -1;
        this->dd_edge.insert(node);
        node->rev_dd_edge.insert(this);

        return 0;
    }

    int add_cd_successor(MyNode* node) {
        if (!node)
            return -1;
        this->cd_edge.insert(node);
        node->rev_cd_edge.insert(this);

        return 0;
    }

    bool has_no_edges() {
        if (cd_edge.empty() && rev_cd_edge.empty() &&
            dd_edge.empty() && rev_dd_edge.empty())
            return true;

        return false;
    }

    bool operator==(const MyNode& node) const {
        return key == node.key;
    }

};

class MyNodeKeyHash {
public:
    std::size_t operator() (const MyNodeKey &key) const {
        std::size_t h1 = std::hash<uint32_t>()(key.colnum);
        std::size_t h2 = std::hash<uint32_t>()(key.linenum);
        std::size_t h3 = std::hash<std::string>()(key.filename);

        return h1 ^ h2 ^ h3;
    }
};

class MySrcPDG {
    std::unordered_map<MyNodeKey, MyNode*, MyNodeKeyHash> nodes;
    
    void sliceWalkDFS(MyNode* node, 
                        int depth, int interproc_depth,
                        int *max_depth, int *max_interproc_depth, std::map<std::string, std::set<unsigned>> *ret, 
                        std::set<MyNode*> *visited) {
        
        if (!node)
            return;
        
        if (depth > *max_depth)
            return;

        if (interproc_depth > *max_interproc_depth)
            return;

        if (visited->find(node) != visited->end())
            return;

        // Add line to final slice
        (*ret)[node->key.filename].insert(node->key.linenum);

        visited->insert(node);
        
        // Walk along data dependency edges
        for (auto e : node->dd_edge) {
            if (e->fun_name == node->fun_name) {
                sliceWalkDFS(e, depth + 1, interproc_depth, max_depth, max_interproc_depth, ret, visited);
            } else {
                sliceWalkDFS(e, depth + 1, interproc_depth + 1, max_depth, max_interproc_depth, ret, visited);
            }
        }

        for (auto e : node->rev_dd_edge) {
            if (e->fun_name == node->fun_name) {
                sliceWalkDFS(e, depth + 1, interproc_depth, max_depth, max_interproc_depth, ret, visited);
            } else {
                sliceWalkDFS(e, depth + 1, interproc_depth + 1, max_depth, max_interproc_depth, ret, visited);
            }
        }

        // Walk along control dependency edges
        for (auto e : node->cd_edge) {
            if (e->fun_name == node->fun_name) {
                sliceWalkDFS(e, depth + 1, interproc_depth, max_depth, max_interproc_depth, ret, visited);
            } else {
                sliceWalkDFS(e, depth + 1, interproc_depth + 1, max_depth, max_interproc_depth, ret, visited);
            }
        }

        for (auto e : node->rev_cd_edge) {
            if (e->fun_name == node->fun_name) {
                sliceWalkDFS(e, depth + 1, interproc_depth, max_depth, max_interproc_depth, ret, visited);
            } else {
                sliceWalkDFS(e, depth + 1, interproc_depth + 1, max_depth, max_interproc_depth, ret, visited);
            }
        }

    }

public:

    void list_nodes() {
        for (auto n : nodes) {
            errs() << n.first.filename << "@" << n.first.linenum << ":" << n.first.colnum << "\n";
        }
    }

    void print_dd_edge(std::string filename, uint32_t linenum, uint32_t colnum) {
        MyNodeKey key;
        key.filename = filename;
        key.linenum = linenum;
        key.colnum = colnum;

        print_dd_edge(key);
    }

    void print_cd_edge(std::string filename, uint32_t linenum, uint32_t colnum) {
        MyNodeKey key;
        key.filename = filename;
        key.linenum = linenum;
        key.colnum = colnum;

        print_cd_edge(key);
    }

    void print_dd_edge(MyNodeKey& key) {
        if (!is_exist(key)) {
            errs() << "Node not existed. \n";
        }

        auto n = is_exist(key);
        errs() << n->key.filename << "@" << n->key.linenum << ":" << n->key.colnum << "\n";
        errs() << "  Function: " << n->fun_name << "\n";
        for (auto e : n->dd_edge) {
            errs() << "  -> " << e->key.filename << "@" << e->key.linenum << ":" << e->key.colnum << "\n";
        }
        for (auto e : n->rev_dd_edge) {
            errs() << "  <- " << e->key.filename << "@" << e->key.linenum << ":" << e->key.colnum << "\n";
        }
    }

    void print_cd_edge(MyNodeKey& key) {
        if (!is_exist(key)) {
            errs() << "Node not existed. \n";
        }

        auto n = is_exist(key);
        errs() << n->key.filename << "@" << n->key.linenum << ":" << n->key.colnum << "\n";
        errs() << "  Function: " << n->fun_name << "\n";
        for (auto e : n->cd_edge) {
            errs() << "  -> " << e->key.filename << "@" << e->key.linenum << ":" << e->key.colnum << "\n";
        }
        for (auto e : n->rev_cd_edge) {
            errs() << "  <- " << e->key.filename << "@" << e->key.linenum << ":" << e->key.colnum << "\n";
        }
    }

    MyNode* add_node(MyNodeKey& key) {
        if (!is_exist(key)) {
            nodes[key] = new MyNode(key.filename, key.linenum, key.colnum);
        }
        return nodes[key];
    }

    MyNode* add_node(std::string filename, uint32_t linenum, uint32_t colnum, std::string funcname) {
        MyNodeKey key;
        key.filename = filename;
        key.linenum = linenum;
        key.colnum = colnum;

        auto node = add_node(key);
        node->fun_name = funcname;
        return node;
    }

    int add_dd_edge(MyNode* source, MyNode* target) {

        if (!source || !target) {
            errs() << "Null node cannot be added.\n";
            return -1;
        }
        
        if (!is_exist(source) || !is_exist(target)) {
            errs() << "node(s) are not found in PDG.\n";
            return -1;
        }
        
        if (nodes[source->key]->add_dd_successor(target)) {
            errs() << "Add dd edge fail.\n";
            return -1;
        }
        return 0;
    }

    int add_cd_edge(MyNode* source, MyNode* target) {
        if (!source || !target) {
            errs() << "Null node cannot be added.\n";
            return -1;
        }
        
        if (!is_exist(source) || !is_exist(target)) {
            errs() << "node(s) are not found in PDG.\n";
            return -1;
        }
        
        if (nodes[source->key]->add_cd_successor(target)) {
            errs() << "Add cd edge fail.\n";
            return -1;
        }
        return 0;
    }

    MyNode* is_exist(MyNode* node) {
        auto res = nodes.find(node->key);
        if (res == nodes.end())
            return nullptr;
        else
            return (*res).second;
    }

    MyNode* is_exist(MyNodeKey& key) {
        auto res = nodes.find(key);
        if (res == nodes.end())
            return nullptr;
        else
            return (*res).second;
    }

    std::map<std::string, std::set<unsigned>> sliceWalk(const MyNodeKey &key, int depth, int interproc_depth) {
        std::map<std::string, std::set<unsigned>> slice;

        if (nodes.find(key) != nodes.end()) {
            MyNode* crit_node = nodes[key];
            std::set<MyNode*> visited;
            sliceWalkDFS(crit_node, 0, 0, &depth, &interproc_depth, &slice, &visited);
            return slice;
        } else {
            errs() << "[Warning]: \t criteria node not found.\n";
            return slice;
        }
    }

};

MySrcPDG mypdg;


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
        SMD.print("my-src-slicer", llvm::errs());
    }

    return M;
}


static std::string getFileName(const llvm::Value* val) {
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(val)) {
        return I->getParent()->getParent()->getSubprogram()->getFile()->getFilename();
    }
    return "NULL";
}

static std::string getFuncName(const llvm::Value* val) {
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(val)) {
        return I->getParent()->getParent()->getName();
    }
    return "NULL";
}


static int getLineCol(const llvm::Value *val, uint32_t& line, uint32_t& col) {
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(val)) {
        auto &DL = I->getDebugLoc();
        if (DL) {
            line = DL.getLine();
            col = DL.getCol();
            return 0;
        } else {
            auto Vit = valuesToVars.find(I);
            if (Vit != valuesToVars.end()) {
                auto &decl = Vit->second;
                line = decl.line;
                col = decl.col;
                return 0;
            } else {
                return -1;
            }
        }
    }
    return -1;
}


static void process_DDA(LLVMDataDependenceAnalysis* DDA) {
    for (auto *subg : DDA->getGraph()->subgraphs()) {
        for (auto *bb : subg->bblocks()) {
            for (auto *node : bb->getNodes()) {
                if (node) {
                    auto *val = DDA->getValue(node);
                    if (!val) {
                        continue;
                    }

                    // Node Info
                    std::string filename = getFileName(val);
                    std::string funcname = getFuncName(val);
                    if (filename == "NULL" || funcname == "NULL") {
                        continue;
                    }

                    uint32_t line;
                    uint32_t col;
                    if (getLineCol(val, line, col)) { // Cannot find corresponding source code line.
                        continue; 
                    }
                    // errs() << filename << "@" << line << ":" << col << "\n"; //HW: Debug
                    MyNode* mynode = mypdg.add_node(filename, line, col, funcname);
                    
                    // Edge Info
                    if (node->isUse() && !node->isPhi()) {
                        for (dg::dda::RWNode *def : DDA->getDefinitions(node)) {
                            auto *def_val = DDA->getValue(def);
                            if (!def_val)
                                continue;

                            filename = getFileName(def_val);
                            if (getLineCol(def_val, line, col)) { // Cannot find corresponding source code line.
                                continue; 
                            }

                            MyNode* defnode = mypdg.add_node(filename, line, col, funcname);
                            defnode->add_dd_successor(mynode);
                        }
                    }
                }

                
            }
        }
    }
}


static void process_CDA(LLVMControlDependenceAnalysis *CDA) {
    const auto *m = CDA->getModule();

    for (auto &F : *m) {
        for (auto &B : F) {
            for (auto &I : B) {

                std::string filename = getFileName(&I);
                std::string funcname = getFuncName(&I);
                if (filename == "NULL" || funcname == "NULL") {
                    continue;
                }

                uint32_t line;
                uint32_t col;
                if (getLineCol(&I, line, col)) { // Cannot find corresponding source code line.
                    continue; 
                }
                // errs() << filename << "@" << line << ":" << col << "\n"; //HW: Debug
                MyNode* start_node = mypdg.add_node(filename, line, col, funcname);


                for (auto *dep : CDA->getDependencies(&B)) {
                    auto *depB = llvm::cast<llvm::BasicBlock>(dep);
                    auto endB = depB->getTerminator();
                    
                    filename = getFileName(endB);
                    funcname = getFuncName(endB);
                    if (filename == "NULL" || funcname == "NULL") {
                        continue;
                    }

                    if (getLineCol(endB, line, col)) { // Cannot find corresponding source code line.
                        continue; 
                    }

                    MyNode* endB_node = mypdg.add_node(filename, line, col, funcname);
                    start_node->add_cd_successor(endB_node);
                }

                for (auto *end : CDA->getDependencies(&I)) {

                    filename = getFileName(end);
                    funcname = getFuncName(end);
                    if (filename == "NULL" || funcname == "NULL") {
                        continue;
                    }

                    if (getLineCol(end, line, col)) { // Cannot find corresponding source code line.
                        continue; 
                    }

                    MyNode* end_node = mypdg.add_node(filename, line, col, funcname);
                    start_node->add_cd_successor(end_node);
                }
            }
        }
    }
}


int parse_mycriteria(std::string crit, std::string *file, uint32_t *line, uint32_t *col) {
    size_t pos = 0;
    std::string token;
    std::vector<std::string> buffer;

    while ((pos = crit.find("#")) != std::string::npos) {
        token = crit.substr(0, pos);
        buffer.push_back(token);
        crit.erase(0, pos + 1);
    }
    buffer.push_back(crit);

    if (buffer.size() != 3) {
        errs() << "[Error]: \t invalid mycriteria\n";
        return -1;
    }

    *file = buffer[0];
    *line = std::stoi(buffer[1]);
    *col = std::stoi(buffer[2]);
    return 0;
}


// lines with matching braces per file
typedef std::vector<std::pair<unsigned, unsigned>> MatchingBracesVector;
std::map<std::string, MatchingBracesVector> matching_braces_per_file;
// mapping line->index in matching_braces
std::map<std::string, std::map<unsigned, unsigned>> nesting_structure_per_file;

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
    SlicerOptions options = parseSlicerOptions(argc, argv);

    if (my_criteria.empty()) {
        errs() << "[Error]:\t Criteria has to be provided.\n";
        errs() << "\tUse: <filename>#<line>#<column>\n";
        return 1;
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

    debug::TimeMeasure tm;

    DGLLVMPointerAnalysis PTA(M.get(), options.dgOptions.PTAOptions);
    tm.start();
    PTA.run();

    tm.stop();
    tm.report("INFO: Pointer analysis took");

    
    LLVMDataDependenceAnalysis DDA(M.get(), &PTA, options.dgOptions.DDAOptions);
    tm.start();

    DDA.run();
    
    tm.stop();
    tm.report("INFO: Data dependence analysis took");

    
    LLVMControlDependenceAnalysis CDA(M.get(), options.dgOptions.CDAOptions);
    tm.start();

    CDA.compute();
    
    tm.stop();
    tm.report("INFO: Control dependence analysis took");

    

#if (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR < 7)
    llvm::errs() << "WARNING: Variables names matching is not supported "
                    "for LLVM older than 3.7\n";
#else
    valuesToVars = allocasToVars(*M);
#endif // LLVM > 3.6
    if (valuesToVars.empty()) {
        llvm::errs() << "WARNING: No debugging information found, "
                        << "the C lines output will be corrupted\n";
    }

    process_DDA(&DDA);
    // process_CDA(&CDA);

    MyNodeKey crit_key;

    if (parse_mycriteria(my_criteria, &crit_key.filename, &crit_key.linenum, &crit_key.colnum)) {
        return 1;
    }

    // mypdg.list_nodes();
    // mypdg.print_dd_edge("commands.c", 767, 28);
    // mypdg.print_cd_edge("commands.c", 1248, 29);
    int wd = std::stoi(walk_depth);
    int wdi = std::stoi(walk_depth_interproc);

    auto line_dict = mypdg.sliceWalk(crit_key, wd, wdi);

    // for (auto lit : line_dict) {
    //     for (auto l : lit.second) {
    //         errs() << lit.first << ":" << l << "\n";
    //     }
        
    // }

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

    return 0;
}