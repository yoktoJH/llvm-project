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
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <map>

using namespace llvm;

struct VariableMetadata {
  GlobalVariable *Name;
  GlobalVariable *Type;
};

struct GlobalVariableMetadata : VariableMetadata {
  bool IsLocal = false;
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

static Value *getValueFromInst(Instruction &Inst) {
  Value *PointerOp = nullptr;
  if (Inst.getOpcode() == Instruction::Load) {
    PointerOp = cast<LoadInst>(Inst).getPointerOperand();
  } else {
    PointerOp = cast<StoreInst>(Inst).getPointerOperand();
  }

  return PointerOp;
}

static std::string getLLVMTypeName(const Value *Val) {
  std::string TypeStr;
  llvm::raw_string_ostream Rso(TypeStr);
  Val->getType()->print(Rso);
  std::string Name = Rso.str();
  if (Name.size() == 0) {
    return "unknown type";
  }
  return Name;
}

static void getVarInfo(Value *PointerOp, std::string &VarName,
                       std::string &TypeName) {

  const auto DVRDeclares = findDVRDeclares(PointerOp);
  // pointerOp->getNameOrAsOperand
  // errs()<< DVRDeclares.size() << " the size of declares should be 1 \n";
  // there is a chance that the size is 0 for some reason, in this case we will
  // use placeholder values
  if (DVRDeclares.size() == 0) {
    VarName = "unknown var";
    TypeName = getLLVMTypeName(PointerOp->stripPointerCasts());
    return;
  }

  auto *const Declare = DVRDeclares[0];
  auto *const DiVariableLoc = Declare->getVariable();
  VarName = DiVariableLoc->getName().str();
  auto *DIType = DiVariableLoc->getType();
  TypeName = DIType->getName().str();
}

static int getFileLine(Instruction &Inst) {
  DebugLoc Dbgloc = Inst.getDebugLoc();
  if (Dbgloc.get() == nullptr) {
    return 0;
  }
  return Dbgloc.getLine();
}

static void getRuntimeFunctions(FunctionCallee &BeforeFunc,
                                FunctionCallee &AfterFunc, Instruction &Inst,
                                Module *Module, FunctionType *RuntimeMemType) {
  if (Inst.getOpcode() == Instruction::Load) {

    BeforeFunc =
        Module->getOrInsertFunction("anaconda_before_read", RuntimeMemType);

    AfterFunc =
        Module->getOrInsertFunction("anaconda_after_read", RuntimeMemType);

  } else if (Inst.getOpcode() == Instruction::Store) {

    BeforeFunc =
        Module->getOrInsertFunction("anaconda_before_write", RuntimeMemType);

    AfterFunc =
        Module->getOrInsertFunction("anaconda_after_write", RuntimeMemType);

  } else {
    // here go atomics if they are ever impelementede  good engrish btw
  }
}

static void instrumentLoadStore(inst_iterator I, LLVMContext &context,
                                Module *Module, variable_map &LocalVariables,
                                GlobalVariable *LocFile,
                                global_variable_map &GlobalVariables) {

  Instruction &Inst = *I;

  // read(ADDRINT=uint64_t 1 addr, uint32_t 2 size, char* 3 var_name, char* 4
  // var_type, uint32_t 5 var_offset, char* 6 loc_file,int32_t 7 loc_line, bool
  // 8 is_local, ADDRINT 9 ins)

  const static SmallVector<Type *, 9> RuntimeMemParameters = {
      PVOID, UINT32, PCHAR, PCHAR, UINT32, PCHAR, INT32, BOOL, UINT64};

  auto *RuntimeMemType =
      FunctionType::get(Type::getVoidTy(context), RuntimeMemParameters, false);

  FunctionCallee RuntimeBeforeFunc;
  FunctionCallee RuntimeAfterFunc;

  getRuntimeFunctions(RuntimeBeforeFunc, RuntimeAfterFunc, Inst, Module,
                      RuntimeMemType);

  auto BeforeBuilder = IRBuilder<>(&Inst);
  auto AfterI = I;
  ++AfterI;
  auto AfterBuilder = IRBuilder<>(&*AfterI);

  // DIFile *dbgfile = cast<DIFile>(dbgloc.getScope());
  /*errs() << "dbglock is" ;
  dbgloc.getAsMDNode()->print(errs());
  errs()<<"\n";*/
  Value *PointerOp = getValueFromInst(Inst);

  GlobalVariable *VarName;
  GlobalVariable *VarType;
  bool IsLocal = false;
  int LineNum = getFileLine(Inst);

  if (LocalVariables.find(PointerOp) != LocalVariables.end()) {
    // it is a local variable
    auto &[Name, Type] = LocalVariables.at(PointerOp);
    VarName = Name;
    VarType = Type;

  } else if (GlobalVariables.find(PointerOp) != GlobalVariables.end()) {
    GlobalVariableMetadata &Data = GlobalVariables.at(PointerOp);
    VarName = Data.Name;
    VarType = Data.Type;
    IsLocal = Data.IsLocal;

  } else { // new local variable
    std::string Name;
    std::string TypeName;
    getVarInfo(PointerOp, Name, TypeName);
    VarName = BeforeBuilder.CreateGlobalString(Name);
    VarType = BeforeBuilder.CreateGlobalString(TypeName);
    LocalVariables.emplace(PointerOp, VariableMetadata{VarName, VarType});
  }

  auto Size = Module->getDataLayout().getTypeAllocSize(PointerOp->getType());

  // read(ADDRINT=uint64_t 1 addr, uint32_t 2 size, char* 3 var_name, char* 4
  // var_type, uint32_t 5 var_offset, char* 6 loc_file,int32_t 7 loc_line,
  // bool 8 is_local, ADDRINT 9 ins)

  auto CreateCallLambda = [&](IRBuilder<> &Builder, auto &Fun) {
    Builder.CreateCall(Fun,
                       {PointerOp, Builder.getInt32(Size), VarName, VarType,
                        Builder.getInt32(0), LocFile, Builder.getInt32(LineNum),
                        IsLocal ? Builder.getTrue() : Builder.getFalse(),
                        Builder.getInt64(0)});
  };

  CreateCallLambda(BeforeBuilder, RuntimeBeforeFunc);
  CreateCallLambda(AfterBuilder, RuntimeAfterFunc);
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


static void insertBacktraceStepInCall(Function &F, LLVMContext &context,
                                 Module *Module) {

  auto *RuntimeType = FunctionType::get(
      Type::getVoidTy(context),
      {PointerType::getUnqual(Type::getInt8Ty(context))}, false);

  auto RuntimeFunc = Module->getOrInsertFunction(
      "anaconda_step_into_function", RuntimeType);
  
  auto Builder = IRBuilder<>(&F.front().front());

  auto *FuncNameGlobal = Builder.CreateGlobalString(F.getName());

  Builder.CreateCall(RuntimeFunc, {FuncNameGlobal});
}

static void insertBacktraceStepOutCall(inst_iterator I, LLVMContext &context,
  Module *Module){
  auto *RuntimeType = FunctionType::get(Type::getVoidTy(context), {},false);

  const auto RuntimeFunc = Module->getOrInsertFunction("anaconda_step_out_of_function",RuntimeType);
  
  IRBuilder<>(&*I).CreateCall(RuntimeFunc,{});
}

static void insertInitFunctions(Function &F) {

  LLVMContext &Context = F.getContext();
  Module *Module = F.getParent();

  auto *RuntimeType = FunctionType::get(Type::getVoidTy(Context), {}, false);

  auto RuntimeFunc = Module->getOrInsertFunction("atomrace_init", RuntimeType);

  auto Builder = IRBuilder<>(&F.front().front());

  Builder.CreateCall(RuntimeFunc, {});
}
/*
void instrumentLocks(inst_iterator I, LLVMContext &context, Module *module,
                     GlobalVariable *loc_file) {}
*/
// gives runtime information about where was thread started
static void insertThreadCreate(inst_iterator I, LLVMContext &context,
                               Module *Module, GlobalVariable *LocFile) {

  inst_iterator II = I;
  ++II;
  auto &Inst = cast<CallInst>(*I);

  static FunctionType *OriginalType = Inst.getFunctionType();
  // thread_create_anaconda(retval,threadid, ,char* 6 loc_file,int32_t 7
  // loc_line)
  static auto *RuntimeType = FunctionType::get(
      Type::getVoidTy(context),
      {I->getType(), OriginalType->getFunctionParamType(0), PCHAR, INT32},
      false);

  FunctionCallee Func =
      Module->getOrInsertFunction("anaconda_thread_create", RuntimeType);

  auto Builder = IRBuilder<>(&*II);
  Builder.CreateCall(Func, {&*I, Inst.getOperand(0), LocFile,
                            Builder.getInt32(getFileLine(Inst))});
}

void instrumentFunctionCall(inst_iterator &I, LLVMContext &context,
                            Module *Module, GlobalVariable *LocFile) {
  
  
  static const char ThreadCreateFuncName[] = "pthread_create";

  // here we will add backtrace shenanigans and thread creation and locking and
  // unlocking-
  inst_iterator II = I;
  ++II;
  auto &Inst = cast<CallInst>(*I);

  static FunctionType *BeforeCallType =
      FunctionType::get(Type::getVoidTy(context), {PCHAR, INT32}, false);
  static FunctionType *AfterCallType =
      FunctionType::get(Type::getVoidTy(context), {}, false);

  static auto BeforeFunction =
      Module->getOrInsertFunction("anaconda_before_call", BeforeCallType);
  static auto AfterFunction =
      Module->getOrInsertFunction("anaconda_after_call", AfterCallType);

  auto Builder = IRBuilder<>(&*I);
  Builder.CreateCall(BeforeFunction,
                     {LocFile, Builder.getInt32(getFileLine(Inst))});
  IRBuilder<>(&*II).CreateCall(AfterFunction, {});

  if (Inst.getCalledFunction()->getName().compare(ThreadCreateFuncName) == 0) {
    insertThreadCreate(I, context, Module, LocFile);
    // skip the thread creation callback
    ++I;
  }
}

static void instrumentFunction(Function &F, GlobalVariable *LocFile,
                               global_variable_map &GlobalVariables) {
  // get some basic values necessary for instrumentation
  variable_map LocalVariables;
  LLVMContext &Context = F.getContext();
  Module *Module = F.getParent();

  if (F.getName().compare("main") == 0) {
    insertInitFunctions(F);
  }


  
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    switch (Instruction &Inst = *I; Inst.getOpcode()) {
    case Instruction::Load:

    case Instruction::Store:
      instrumentLoadStore(I, Context, Module, LocalVariables, LocFile,
                          GlobalVariables);
      // move the iterator in order to skip the call we just inserted
      ++I;
      break;

    case Instruction::Call:
      instrumentFunctionCall(I, Context, Module, LocFile);
      // move the iterator in order to skip the call we just inserted
      ++I;
      break;

    case Instruction::Ret:
      insertBacktraceStepOutCall(I,Context,Module);
      break;
    default:
      break;
    }
    // instrumentLocks(I, Context, Module, LocFile);
  }

  //skip epty functions, as those are probably just declarations
  if (F.size()!=0){
    insertBacktraceStepInCall(F,Context,Module);  
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

static GlobalVariable *createInsertGlobalString(Module &M,
                                                const StringRef Str) {
  Constant *StrConstant =
      ConstantDataArray::getString(M.getContext(), Str, true);

  auto *GV = new GlobalVariable(M, StrConstant->getType(), true,
                                GlobalValue::PrivateLinkage, StrConstant, "",
                                nullptr, GlobalVariable::NotThreadLocal, 0U);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));
  return GV;
}

static void findGlobalVariablesDebugInfo(Module &M,
                                         global_variable_map &GlobalVariables) {
  for (GlobalVariable &GV : M.globals()) {
    SmallVector<DIGlobalVariableExpression *, 1> Debuginfo;
    GV.getDebugInfo(Debuginfo);
    if (Debuginfo.size() == 0) {
      errs() << "global variable " << GV.getName() << " has no debug info\n";
      return;
    }
    DIGlobalVariableExpression *Digve = Debuginfo[0];
    DIGlobalVariable *Digv = Digve->getVariable();
    auto *const VarName = createInsertGlobalString(M, Digv->getName());
    auto *const TypeName =
        createInsertGlobalString(M, Digv->getType()->getName());
    GlobalVariables.emplace(&GV, GlobalVariableMetadata{{VarName, TypeName},
                                                        Digv->isLocalToUnit()});
  }
}

PreservedAnalyses AnacondaPass::run(Module &M, ModuleAnalysisManager &AM) {

  const std::string FileName = M.getSourceFileName();
  auto *GV = createInsertGlobalString(M, FileName);
  global_variable_map GlobalVariables;
  findGlobalVariablesDebugInfo(M, GlobalVariables);
  for (Function &F : M.functions()) {
    instrumentFunction(F, GV, GlobalVariables);
  }
  return PreservedAnalyses::none();
}
