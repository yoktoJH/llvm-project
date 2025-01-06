#include "llvm/Transforms/Instrumentation/Anaconda.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

using namespace llvm;

struct VariableMetadata {
  GlobalVariable *name;
  GlobalVariable *type;
};

struct GlobalVariableMetadata : VariableMetadata {
  bool isLocal = false;
};

using variable_map = std::map<const Value *, VariableMetadata>;
using global_variable_map = std::map<Value *, GlobalVariableMetadata>;

#define UINT32 Type::getInt32Ty(context)
#define INT32 Type::getInt32Ty(context)
#define PCHAR PointerType::getUnqual(Type::getInt8Ty(context))
#define UINT64 Type::getInt64Ty(context)
#define INT64 Type::getInt64Ty(context)
#define BOOL Type::getInt1Ty(context)
#define PVOID PointerType::getUnqual(Type::getInt8Ty(context))

Value *getValueFromInst(Instruction &inst) {
  Value *pointerOp = nullptr;
  if (inst.getOpcode() == Instruction::Load) {
    pointerOp = cast<LoadInst>(inst).getPointerOperand();
  } else {
    pointerOp = cast<StoreInst>(inst).getPointerOperand();
  }

  return pointerOp;
}

std::string getLLVMTypeName(Value *val) {
  std::string type_str;
  llvm::raw_string_ostream rso(type_str);
  val->getType()->print(rso);
  std::string name = rso.str();
  if (name.size() == 0) {
    return "unknown type";
  }
  return name;
}

void getVarInfo(Value *pointerOp, std::string &var_name,
                std::string &type_name) {

  auto DVRDeclares = findDVRDeclares(pointerOp);
  // pointerOp->getNameOrAsOperand
  // errs()<< DVRDeclares.size() << " the size of declares should be 1 \n";
  // there is a chance that the size is 0 for some reason, in this case we will
  // use placeholder values
  if (DVRDeclares.size() == 0) {
    var_name = "unknown var";
    type_name = getLLVMTypeName(pointerOp->stripPointerCasts());
    return;
  }

  auto declare = DVRDeclares[0];
  auto divariableloc = declare->getVariable();
  var_name = divariableloc->getName().str();
  auto ditype = divariableloc->getType();
  type_name = ditype->getName().str();

}

void getFileLine(Instruction &inst, int &line) {
  DebugLoc dbgloc = inst.getDebugLoc();
  if (dbgloc.get() == nullptr) {
    return;
  }
  line = dbgloc.getLine();
}

void getRuntimeFunctions(FunctionCallee &before_func,
                         FunctionCallee &after_func, Instruction &inst,
                         Module *module, FunctionType *runtime_mem_type) {
  if (inst.getOpcode() == Instruction::Load) {

    before_func =
        module->getOrInsertFunction("anaconda_before_read", runtime_mem_type);

    after_func =
        module->getOrInsertFunction("anaconda_after_read", runtime_mem_type);
  } else if (inst.getOpcode() == Instruction::Store) {

    before_func =
        module->getOrInsertFunction("anaconda_before_write", runtime_mem_type);

    after_func =
        module->getOrInsertFunction("anaconda_after_write", runtime_mem_type);
  } else {
    // here go atomics if they are ever impelementede  good engrish btw
  }
}

void instrumentLoadStore(inst_iterator I, LLVMContext &context, Module *module,
                         variable_map &local_variables,
                         GlobalVariable *loc_file,
                         global_variable_map &global_variables) {

  Instruction &inst = *I;
  if (inst.getOpcode() != Instruction::Load &&
      inst.getOpcode() != Instruction::Store) {
    return;
  }

  // read(ADDRINT=uint64_t 1 addr, uint32_t 2 size, char* 3 var_name, char* 4
  // var_type, uint32_t 5 var_offset, char* 6 loc_file,int32_t 7 loc_line, bool
  // 8 is_local, ADDRINT 9 ins)

  SmallVector<Type *, 9> runtime_mem_parameters = {
      PVOID, UINT32, PCHAR, PCHAR, UINT32, PCHAR, INT32, BOOL, UINT64};

  auto runtime_mem_type = FunctionType::get(Type::getVoidTy(context),
                                            runtime_mem_parameters, false);

  FunctionCallee runtime_before_func;
  FunctionCallee runtime_after_func;

  getRuntimeFunctions(runtime_before_func, runtime_after_func, inst, module,
                      runtime_mem_type);

  auto before_builder = IRBuilder<>(&inst);
  auto after_I = I;
  ++after_I;
  auto after_builder = IRBuilder<>(&*after_I);

  // DIFile *dbgfile = cast<DIFile>(dbgloc.getScope());
  /*errs() << "dbglock is" ;
  dbgloc.getAsMDNode()->print(errs());
  errs()<<"\n";*/
  Value *pointerOp = getValueFromInst(inst);

  GlobalVariable *var_name;
  GlobalVariable *var_type;
  bool isLocal = false;
  int line_num = 0;

  getFileLine(inst, line_num);

  if (local_variables.find(pointerOp) != local_variables.end()) {
    // it is a local variable
    VariableMetadata &data = local_variables.at(pointerOp);
    var_name = data.name;
    var_type = data.type;

  } else if (global_variables.find(pointerOp) != global_variables.end()) {
    GlobalVariableMetadata &data = global_variables.at(pointerOp);
    var_name = data.name;
    var_type = data.type;
    isLocal = data.isLocal;

  } else { // new local variable
    std::string name;
    std::string type_name;
    getVarInfo(pointerOp, name, type_name);
    var_name = before_builder.CreateGlobalString(name);
    var_type = before_builder.CreateGlobalString(type_name);
    local_variables.emplace(pointerOp, VariableMetadata{var_name, var_type});
  }

  auto size = module->getDataLayout().getTypeAllocSize(pointerOp->getType());

  // read(ADDRINT=uint64_t 1 addr, uint32_t 2 size, char* 3 var_name, char* 4
  // var_type, uint32_t 5 var_offset, char* 6 loc_file,int32_t 7 loc_line,
  // bool 8 is_local, ADDRINT 9 ins)

  auto create_call_lambda = [&](IRBuilder<> &builder, auto &fun) {
    builder.CreateCall(fun, {pointerOp, builder.getInt32(size), var_name,
                             var_type, builder.getInt32(0), loc_file,
                             builder.getInt32(line_num),
                             isLocal ? builder.getTrue() : builder.getFalse(),
                             builder.getInt64(0)});
  };

  create_call_lambda(before_builder, runtime_before_func);
  create_call_lambda(after_builder, runtime_after_func);
  /*before_builder.CreateCall(
      runtime_before_func,
      {address, before_builder.getInt32(size), var_name, var_type,
       before_builder.getInt32(0), loc_file, before_builder.getInt32(0),
       before_builder.getFalse(), before_builder.getInt64(0)});

  after_builder.CreateCall(
      runtime_after_func,
      {address, after_builder.getInt32(size), var_name, var_type,
       after_builder.getInt32(0), loc_file, after_builder.getInt32(0),
       after_builder.getFalse(), after_builder.getInt64(0)});*/
}

void insertInitFunctions(Function &F) {

  LLVMContext &context = F.getContext();
  Module *module = F.getParent();

  auto runtime_type = FunctionType::get(Type::getVoidTy(context), {}, false);

  auto runtime_func =
      module->getOrInsertFunction("atomrace_init", runtime_type);

  auto builder = IRBuilder<>(&F.front().front());

  builder.CreateCall(runtime_func, {});
}

void instrumentFunction(Function &F, GlobalVariable *loc_file,
                        global_variable_map &global_variables) {
  // get some basic values necessary for instrumentation
  variable_map local_variables;
  LLVMContext &context = F.getContext();
  Module *module = F.getParent();

  if (F.getName().compare("main") == 0) {
    insertInitFunctions(F);
  }

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {

    instrumentLoadStore(I, context, module, local_variables, loc_file,
                        global_variables);
  }
  /*
    // insert the instrumentation function declaration into the module(file)
    auto runtime_type = FunctionType::get(
        Type::getVoidTy(context),
        {PointerType::getUnqual(Type::getInt8Ty(context))}, false);

    auto runtime_func =
        module->getOrInsertFunction("function_called", runtime_type);

    auto func_name = F.getName();

    auto builder = IRBuilder<>(&F.front().front());

    auto func_name_global = builder.CreateGlobalString(func_name);
    builder.CreateCall(runtime_func, {func_name_global});*/

  // errs() << F.getName() << "\n";
}

GlobalVariable *createInsertGlobalSring(Module &M, StringRef str) {
  Constant *StrConstant =
      ConstantDataArray::getString(M.getContext(), str, true);

  auto *GV = new GlobalVariable(M, StrConstant->getType(), true,
                                GlobalValue::PrivateLinkage, StrConstant, "",
                                nullptr, GlobalVariable::NotThreadLocal, 0U);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));
  return GV;
}

void findGlobalVariablesDebufInfo(Module &M,
                                  global_variable_map &global_variables) {
  for (GlobalVariable &GV : M.globals()) {
    SmallVector<DIGlobalVariableExpression *, 1> debuginfo;
    GV.getDebugInfo(debuginfo);
    if (debuginfo.size() == 0) {
      errs() << "global variable " << GV.getName() << " has no debug info\n";
      return;
    }
    DIGlobalVariableExpression *digve = debuginfo[0];
    DIGlobalVariable *digv = digve->getVariable();
    auto var_name = createInsertGlobalSring(M, digv->getName());
    auto type_name = createInsertGlobalSring(M, digv->getType()->getName());
    global_variables.emplace(
        &GV,
        GlobalVariableMetadata{{var_name, type_name}, digv->isLocalToUnit()});
  }
}

PreservedAnalyses AnacondaPass::run(Module &M, ModuleAnalysisManager &AM) {

  std::string file_name = M.getSourceFileName();

  auto *GV = createInsertGlobalSring(M, file_name);
  global_variable_map global_variables;
  findGlobalVariablesDebufInfo(M, global_variables);
  for (Function &F : M.functions()) {
    instrumentFunction(F, GV, global_variables);
  }
  return PreservedAnalyses::none();
}
