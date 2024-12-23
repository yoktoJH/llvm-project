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
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define UINT32 Type::getInt32Ty(context)
#define INT32 Type::getInt32Ty(context)
#define PCHAR PointerType::getUnqual(Type::getInt8Ty(context))
#define UINT64 Type::getInt64Ty(context)
#define INT64 Type::getInt64Ty(context)
#define BOOL Type::getInt1Ty(context)
#define PVOID PointerType::getUnqual(Type::getInt8Ty(context))

// TODO rewrite babyyyy
Value *getAddress(Instruction &inst) {
  if (inst.getOpcode() == Instruction::Load) {
    return cast<LoadInst>(inst).getPointerOperand();
  }
  return cast<StoreInst>(inst).getPointerOperand();
}
// rewrite under all circumstances
auto getSize(Instruction &inst, Module *module) {
  if (inst.getOpcode() == Instruction::Load) {
    return module->getDataLayout().getTypeAllocSize(inst.getType());
  }
  auto T = cast<StoreInst>(inst).getOperand(0);
  return module->getDataLayout().getTypeAllocSize(T->getType());
}

auto anac_getTypeName(Instruction &inst) {

  std::string type_str;
  llvm::raw_string_ostream rso(type_str);

  if (inst.getOpcode() == Instruction::Load) {
    inst.getType()->print(rso);
  } else {
    auto T = cast<StoreInst>(inst).getOperand(0);
    T->getType()->print(rso);
  }

  return rso.str();
}
void getVarInfo(Instruction &inst,std::string &var_name, std::string &type_name) {

  Value *pointerOp = nullptr;
  if (inst.getOpcode() == Instruction::Load) {
    pointerOp = cast<LoadInst>(inst).getPointerOperand();
  } else {
    pointerOp = cast<StoreInst>(inst).getPointerOperand();
  }

  auto DVRDeclares = findDVRDeclares(pointerOp);
  //pointerOp->getNameOrAsOperand
    //errs()<< DVRDeclares.size() << " the size of declares should be 1 \n";
  // there is a chance that the size is 0 for some reason, in this case we will use placeholder values
  if (DVRDeclares.size()==0){
    var_name = "unknown var";
    type_name = "unknown type";
    return;
  }
  auto declare = DVRDeclares[0];
  auto divariableloc = declare->getVariable();
  var_name = divariableloc->getName().str();
  auto ditype = divariableloc->getType();
  type_name = ditype->getName().str();
}

void getFileInfo(Instruction &inst,std::string& file, int& line ){
  auto dbgloc = inst.getDebugLoc();
  if (dbgloc.get()==nullptr)
  {
    file = "not in a file";
    return;
  }
  line = dbgloc.getLine();
  auto scope = dyn_cast<DIScope>(dbgloc.getScope());
  if (scope == nullptr)
  {
    errs()<<"scope cast failed\n";
    return;
  }
  file = scope->getFilename().str();
  
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
    // here go atomics if they are ever impelementede
  }
}

void instrumentLoadStore(inst_iterator I, LLVMContext &context,
                         Module *module) {

  // read(ADDRINT=uint64_t 1 addr, uint32_t 2 size, char* 3 var_name, char* 4
  // var_type, uint32_t 5 var_offset, char* 6 loc_file,int32_t 7 loc_line, bool
  // 8 is_local, ADDRINT 9 ins)

  SmallVector<Type *, 9> runtime_mem_parameters = {
      PVOID, UINT32, PCHAR, PCHAR, UINT32, PCHAR, INT32, BOOL, UINT64};

  auto runtime_mem_type = FunctionType::get(Type::getVoidTy(context),
                                            runtime_mem_parameters, false);

  Instruction &inst = *I;
  if (inst.getOpcode() != Instruction::Load &&
      inst.getOpcode() != Instruction::Store) {
    return;
  }

  FunctionCallee runtime_before_func;
  FunctionCallee runtime_after_func;

  getRuntimeFunctions(runtime_before_func,runtime_after_func,inst,module,runtime_mem_type);


  auto before_builder = IRBuilder<>(&inst);
  auto after_I = I;
  ++after_I;
  auto after_builder = IRBuilder<>(&*after_I);


  // DIFile *dbgfile = cast<DIFile>(dbgloc.getScope());
  /*errs() << "dbglock is" ;
  dbgloc.getAsMDNode()->print(errs());
  errs()<<"\n";*/

  std::string name;
  std::string type_name;
  getVarInfo(inst, name, type_name);
  std::string file_name;
  int line_num =0;
  getFileInfo(inst,file_name,line_num);
  llvm::GlobalVariable *var_name =
      before_builder.CreateGlobalString(name);
  llvm::GlobalVariable *var_type =
      before_builder.CreateGlobalString(type_name);
  llvm::GlobalVariable *loc_file =
      before_builder.CreateGlobalString(file_name);

  Value *address = getAddress(inst);
  auto size = getSize(inst, module);

  // read(ADDRINT=uint64_t 1 addr, uint32_t 2 size, char* 3 var_name, char* 4
  // var_type, uint32_t 5 var_offset, char* 6 loc_file,int32_t 7 loc_line,
  // bool 8 is_local, ADDRINT 9 ins)

  auto create_call_lambda = [&](IRBuilder<> &builder, auto &fun) {
    builder.CreateCall(fun,
                       {address, builder.getInt32(size), var_name, var_type,
                        builder.getInt32(0), loc_file, builder.getInt32(line_num),
                        builder.getTrue(), builder.getInt64(0)});
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

void instrumentInitFunctions(Function &F){

  LLVMContext &context = F.getContext();
  Module *module = F.getParent();

  auto runtime_type = FunctionType::get(
        Type::getVoidTy(context),{}, false);

    auto runtime_func =
        module->getOrInsertFunction("atomrace_init", runtime_type);

    auto builder = IRBuilder<>(&F.front().front());
    
    builder.CreateCall(runtime_func, {});
}
PreservedAnalyses AnacondaPass::run(Function &F, FunctionAnalysisManager &AM) {

  // get some basic values necessary for instrumentation
  LLVMContext &context = F.getContext();
  Module *module = F.getParent();

  if (F.getName().compare("main")==0){
    instrumentInitFunctions(F);
  }

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    

    instrumentLoadStore(I, context, module);
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
  return PreservedAnalyses::none();
}
