// Copyright Hugh Perkins 2016

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// input: IR from cuda compilation

#include "ir-to-opencl.h"

#include "ir-to-opencl-common.h"
#include "struct_clone.h"
#include "handle_branching.h"
#include "branching_transforms.h"
#include "branches_as_switch/branches_as_switch.h"
#include "function_names_map.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>

using namespace llvm;
using namespace std;
using namespace cocl;

static llvm::LLVMContext context;
static std::map<std::string, Value *> NamedValues;
static std::map<string, bool> iskernel_by_name;

// map<Value *, string> nameByValue; // name here might be an entire subepxression
// map<Value *, string> origNameByValue; //origName here is just like v18, or v19, no expressions; its for debugging
// static int nextNameIdx;
// static string currentFunctionSharedDeclarations = "";
static map<string, string> currentFunctionPhiDeclarationsByName;
// static string globalDeclarations = "";
// static string structpointershimcode = "";
// static set<Value *> functionNeededForwardDeclarations;
static map<BasicBlock *, int> functionBlockIndex;
static set<Value *> currentFunctionAlreadyDeclaredShared;
// static set<Value *>valuesAreExpressions;

static set<Function *> dumpedFunctions;
static set<Function *>functionsToDump;

static int instructions_processed = 0;
static bool debug = false;
static bool add_ir_to_cl = false;


static string cl_add_definitions = R"(
inline float __shfl_down_3(local int *scratch, float v0, int v1, int v2) {
    // local float mem[1024];
    local float *mem = (local float *)scratch;
    int tid = get_local_id(0);
    int warpid = tid % 32;
    int warpstart = tid - warpid;
    mem[tid] = v0;
    //barrier(CLK_LOCAL_MEM_FENCE);
    int warpsrc = warpid + v1;
    warpsrc = warpsrc >= 32 ? warpid : warpsrc;
    return mem[warpstart + warpsrc];
}

inline float __shfl_down_2(local int *scratch, float v0, int v1) {
    return __shfl_down_3(scratch, v0, v1, 32);
}

// based on https://community.amd.com/thread/167462
inline int __atomic_inc(global volatile int *ptr, int val) {
    while( true ){
        int old = *ptr;
        if( old >= val ) {
            if(old == atomic_cmpxchg(ptr, old, 0)) {
               break;
            }
        }
        else {
            if(old == atomic_cmpxchg(ptr, old, old+1)) {
                break;
            }
        }
    }
    return 0;
}

//float __atomic_add(global volatile float *ptr, float val);

inline float __atomic_add(global volatile float *ptr, float val) { // we need to actually implement this
    return 555;
}

)";

std::string dumpValue(Value *value) {
    std::string gencode = "";
    if(nameByValue.find(value) != nameByValue.end()) {
        string name = nameByValue[value];
        gencode += name;
        return gencode;
    }

    if(Constant *constant = dyn_cast<Constant>(value)) {
        if(ConstantInt *constInt = dyn_cast<ConstantInt>(constant)) {
            // COCL_PRINT(cout << "dumpValue, constint" << endl);
            int intvalue = readInt32Constant(constInt);
            string asstring = toString(intvalue);
            nameByValue[value] = asstring;
            origNameByValue[value] = asstring;
            return asstring;
        }
        if(ConstantFP *constFP = dyn_cast<ConstantFP>(constant)) {
            // COCL_PRINT(cout << "dumpValue, float" << endl);
            float floatvalue = readFloatConstant(constFP);
            string asstring = toString(floatvalue);
            if(asstring == "inf") {
                return "INFINITY";
            }
            if(asstring == "-inf") {
                return "-INFINITY";
            }
            if(asstring.find('.') == string::npos) {
                asstring += ".0";
            }
            asstring += "f";
            nameByValue[value] = asstring;
            origNameByValue[value] = asstring;
            return asstring;
        }
    }

    // mark it as needing to be declared, then return it
    storeValueName(value);
    // origNameByValue[value] = nameByValue[value];
    functionNeededForwardDeclarations.insert(value);
    // COCL_PRINT(cout << "adding to needs forward declaration " << nameByValue[value] << endl);
    value->dump();
    outs() << "\n";
    return nameByValue[value];
}

void declareGlobal(GlobalValue *global) {
    string gencode = "";
    if(GlobalVariable *var = dyn_cast<GlobalVariable>(global)) {
        string name = getName(global);
        if(name == "$str") {
            return;  // lazily skip $str for now...
        }
        gencode += "constant " + dumpType(global->getType()->getPointerElementType()) + " " + name;
        if(PointerType *pointerType = dyn_cast<PointerType>(global->getType())) {
            int addressspace = pointerType->getAddressSpace();
            if(addressspace == 3) { // shared/local => skip
                return;
            } else {
                updateAddressSpace(var, 4);
            }
        }
        if(var->hasInitializer()) {
            Constant *initializer = var->getInitializer();
            gencode += " = {";
            if(ConstantStruct *constStruct = dyn_cast<ConstantStruct>(initializer)) {
                int i = 0;
                while(Value *aggel = constStruct->getAggregateElement(i)) {
                    if(i == 0) {
                    } else {
                        gencode += ", ";
                    }
                    gencode += dumpOperand(aggel);
                    i++;
                }
                if(i > 0) {
                }
            }
            gencode += "}";
        } else {
            gencode += " = {}";
        }
    } else {
        global->dump();
        throw runtime_error("unimplemented declareglobalvalue for this type");
    }
    gencode += ";\n";
    globalDeclarations += gencode + "\n";
}

string dumpChainedNextOp(int level, Value *op0) {
    string op0string = "";
    if(ConstantExpr*expr = dyn_cast<ConstantExpr>(op0)) {
        Instruction *childinstr = expr->getAsInstruction();
        string childresult = dumpChainedInstruction(level + 1, childinstr);
        op0string = "(" + childresult + ")";
    } else if(Constant*constant = dyn_cast<Constant>(op0)) {
        string constantstring = dumpConstant(constant);
        nameByValue[op0] = constantstring;
        origNameByValue[op0] = constantstring;
        return constantstring;
    } else {
        op0->dump();
        throw runtime_error("dumpchained gep unknown operand1 type ");
    }
    nameByValue[op0] = op0string;
    return op0string;
}

string dumpChainedInstruction(int level, Instruction * instr) {
    if(GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(instr)) {
        string thisinstrstring = dumpGetElementPtrRhs(gep);
        nameByValue[instr] = thisinstrstring;
        return thisinstrstring;
    } else if(BitCastInst *bitcast = dyn_cast<BitCastInst>(instr)) {
        string thisinstrstring = dumpBitCastRhs(bitcast);
        nameByValue[bitcast] = thisinstrstring;
        return thisinstrstring;
    } else if(AddrSpaceCastInst *addrspacecast = dyn_cast<AddrSpaceCastInst>(instr)) {
        string thisinstrstring = dumpAddrSpaceCastRhs(addrspacecast);
        nameByValue[addrspacecast] = thisinstrstring;
        return thisinstrstring;
    } else {
        instr->dump();
        throw runtime_error("dumpchained unknown instruction type ");
    }
}

// string dumpConstant(Constant *constant) {
//     unsigned int valueTy = constant->getValueID();
//     ostringstream oss;
//     if(ConstantInt *constantInt = dyn_cast<ConstantInt>(constant)) {
//         oss << constantInt->getSExtValue();
//         string constantintval = oss.str();
//         return constantintval;
//     } else if(isa<ConstantStruct>(constant)) {
//         throw runtime_error("constantStruct not implemented in dumpconstnat");
//     } else if(ConstantExpr *expr = dyn_cast<ConstantExpr>(constant)) {
//         Instruction *instr = expr->getAsInstruction();
//         copyAddressSpace(constant, instr);
//         string dcires = dumpChainedInstruction(0, instr);
//         copyAddressSpace(instr, constant);
//         nameByValue[constant] = dcires;
//         return dcires;
//     } else if(ConstantFP *constantFP = dyn_cast<ConstantFP>(constant)) {
//         float floatvalue = readFloatConstant(constantFP);
//         // float floatvalue = constantFP->getValueAPF().convertToFloat();
//         ostringstream oss;
//         oss << floatvalue;
//         string floatvaluestr = oss.str();
//         if(floatvaluestr == "inf") {
//             return "INFINITY";
//         } else if(floatvaluestr == "-inf") {
//             return "-INFINITY";
//         }
//         if(single_precision) {
//             if(floatvaluestr.find('.') == string::npos) {
//                 floatvaluestr += ".0";
//             }
//             floatvaluestr += "f";
//         }
//         return floatvaluestr;
//     } else if(GlobalValue *global = dyn_cast<GlobalValue>(constant)) {
//          if(PointerType *pointerType = dyn_cast<PointerType>(global->getType())) {
//              int addressspace = pointerType->getAddressSpace();
//              string name = getName(global);
//              if(addressspace == 3) {  // if it's local memory, it's not really 'global', juts return the name
//                  return name;
//              }
//          }
//          if(nameByValue.find(constant) != nameByValue.end()) {
//             return nameByValue[constant];
//          }
//          string name = getName(global);
//          string ourinstrstr = "(&" + name + ")";
//          updateAddressSpace(constant, 4);
//          nameByValue[constant] = ourinstrstr;

//          return ourinstrstr;
//     } else if(isa<UndefValue>(constant)) {
//         return "";
//     } else if(isa<ConstantPointerNull>(constant)) {
//         return "0";
//     } else {
//         cout << "valueTy " << valueTy << endl;
//         oss << "unknown";
//         constant->dump();
//         throw runtime_error("unknown constnat type");
//     }
//     return oss.str();
// }

std::string dumpInttoPtr(IntToPtrInst *instr) {
    string gencode = "";
    string typestr = dumpType(instr->getType());
    gencode += "(" + typestr + ")" + dumpValue(instr->getOperand(0));
    return gencode;
}

std::string dumpPtrToInt(PtrToIntInst *instr) {
    string gencode = "";
    string typestr = dumpType(instr->getType());
    gencode += "(" + typestr + ")" + dumpValue(instr->getOperand(0));
    return gencode;
}

std::string dumpModule(Module *M, string specificFunction = "") {
    string gencode;

    // get struct declarations
    // global_begin/end returns all the bits that start with '@', at the top of the .ll
    // cout << "begin declare global variables" << endl;
    // for(auto it=M->global_begin(); it != M->global_end(); it++) {
    //     GlobalVariable *glob = &*it;
    //     string name = getName(glob);
    //     if(name == "llvm.used") {
    //         continue;
    //     }
    //     if(name.find(".str") == 0) {
    //         // ignore global strings for now (probably add in locally; though I dont think opencl really uses strings..)
    //         continue;
    //     }
    //     if(name == "llvm.global_ctors") {
    //         // we should handle these sooner or later, but skip for now
    //         cerr << "warning: skipping @llvm.global_ctors" << endl;
    //         continue;
    //     }
    //     if(ignoredGlobalVariables.find(name) != ignoredGlobalVariables.end()) {
    //         continue;
    //     }
    //     glob->dump();
    //     declareGlobal(glob);
    // }

    // figure out which functions are kernels
    for(auto it=M->named_metadata_begin(); it != M->named_metadata_end(); it++) {
        NamedMDNode *namedMDNode = &*it;
        for(auto it2=namedMDNode->op_begin(); it2 != namedMDNode->op_end(); it2++) {
            MDNode *mdNode = *it2;
            bool isKernel = false;
            string kernelName = "";
            for(auto it3=mdNode->op_begin(); it3 != mdNode->op_end(); it3++) {
                const MDOperand *op = it3;
                Metadata *metadata = op->get();
                if(metadata != 0) {
                    if(MDString::classof(metadata)) {
                        string meta_value = string((cast<MDString>(metadata))->getString());
                        if(meta_value == "kernel") {
                            isKernel = true;
                        }
                    } else if(ConstantAsMetadata::classof(metadata)) {
                        Constant *constant = (cast<ConstantAsMetadata>(metadata))->getValue();
                        if(GlobalValue::classof(constant)) {
                            GlobalValue *global = cast<GlobalValue>(constant);
                            if(global->getType()->getPointerElementType()->getTypeID() == Type::FunctionTyID) {
                                string functionName = getName(constant);
                                kernelName = functionName;
                            }
                        }
                    }
                }
            }
            if(isKernel) {
                iskernel_by_name[kernelName] = true;
            }
        }
    }

    // dump function declarations
    // cout << "beginning function declarations" << endl;
    // for(auto it = M->begin(); it != M->end(); it++) {
    //     Function *F = &*it;
    //     string name = getName(F);
    //     if(iskernel_by_name[name]) {
    //         continue;  // no point in declaring kernels I think
    //     }
    //     if(specificFunction != "" && name != specificFunction) {
    //         continue;
    //     }
    //     // hack for tensorflow: remove anything with 4Half in it, which one we dont use and two copies pointers inside
    //     // pointers to structs, as kernel parameters...
    //     if(name.find("_4half") != string::npos) {
    //         continue;
    //     }
    //     if(ignoredFunctionNames.find(name) == ignoredFunctionNames.end() &&
    //             knownFunctionsMap.find(name) == knownFunctionsMap.end()) {
    //         string declaration = dumpFunctionDeclaration(F) + ";";
    //         // cout << declaration << endl;
    //         gencode += declaration + "\n";
    //     }
    // }
    // gencode += "\n";
    // cout << "done writing function declarations" << endl;
    // cout << endl;

    if(specificFunction != "") {
        Function *F = M->getFunction(StringRef(specificFunction));
        functionsToDump.insert(F);
       gencode += dumpFunctions(functionsToDump);
       string declarations = "";
       for(auto it=dumpedFunctions.begin(); it != dumpedFunctions.end(); it++) {
            Function *F = *it;
            string name = F->getName();
            if(!function_names_map_isIgnoredFunction(name) &&
                !function_names_map_isMappedFunction(name)) {
                string declaration = dumpFunctionDeclaration(F) + ";";
                // cout << declaration << endl;
                declarations += declaration + "\n";
            }
       }
       gencode = "// Declarations:\n\n" + declarations + "\n\n// Definitions:\n\n" + gencode;
    } else {
        int i = 0;
        for(auto it = M->begin(); it != M->end(); it++) {
            nameByValue.clear();
            nextNameIdx = 0;
            Function *F = &*it;
            string name = getName(F);
            // COCL_PRINT(cout << "dumping functoin " << name << endl);
            // hack for tensorflow: remove anything with 4Half in it, which one we dont use and two copies pointers inside
            // pointers to structs, as kernel parameters...
            if(name.find("_4half") != string::npos) {
                continue;
            }
            if(function_names_map_isIgnoredFunction(name)) {
                continue;
            }
            if(function_names_map_isMappedFunction(name)) {
                continue;
            }
            if(specificFunction != "" and name != specificFunction) {
                continue;
            }
            if(i > 0) {
                gencode += "\n";
            }
            gencode += dumpFunction(F);
            i++;
        }
    }
    gencode = getDeclarationsToWrite() + "\n" + globalDeclarations + "\n" + gencode;
    return gencode;
}

string convertModuleToCl(Module *M, string specificFunction) {
    function_names_map_populateKnownValues();
    string gencode = "";
    gencode += cl_add_definitions;
    // COCL_PRINT(cout << "cl_add_definitions " << cl_add_definitions << endl);
    try {
        gencode += dumpModule(M, specificFunction);
    } catch(const runtime_error &e) {
        cout << "instructions processed before crash " << instructions_processed << endl;
        throw e;
    } catch(...) {
        cout << "some unknown exception" << endl;
    }
    return gencode;
}

string convertLlStringToCl(string llString, string specificFunction) {
    StringRef llStringRef(llString);
    cout << "got llstringref" << endl;
    unique_ptr<MemoryBuffer> llMemoryBuffer = MemoryBuffer::getMemBuffer(llStringRef);
    cout << "got memory buffer " << endl;
    SMDiagnostic smDiagnostic;
    unique_ptr<Module> M = parseIR(llMemoryBuffer->getMemBufferRef(), smDiagnostic,
                                context);
    cout << "parsed module" << endl;
    if(!M) {
        smDiagnostic.print("irtopencl", errs());
        // return "";
        throw runtime_error("failed to parse IR");
    }
    string gencode = convertModuleToCl(M.get(), specificFunction);
    // cout << "gencode " << gencode << endl;;
    return gencode;
}

void convertLlFileToClFile(string llFilename, string ClFilename, string specificFunction) {
    SMDiagnostic smDiagnostic;
    std::unique_ptr<llvm::Module> M = parseIRFile(llFilename, smDiagnostic, context);
    if(!M) {
        smDiagnostic.print("irtoopencl", errs());
        // return 1;
        throw runtime_error("failed to parse IR");
    }
    string gencode = convertModuleToCl(M.get(), specificFunction);
    ofstream of;
    of.open(ClFilename, ios_base::out);
    of << gencode;
    of.close();
}
