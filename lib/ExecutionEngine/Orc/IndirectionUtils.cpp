//===---- IndirectionUtils.cpp - Utilities for call indirection in Orc ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ExecutionEngine/Orc/OrcABISupport.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Format.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <sstream>

using namespace llvm;
using namespace llvm::orc;

namespace {

class CompileCallbackMaterializationUnit : public orc::MaterializationUnit {
public:
  using CompileFunction = JITCompileCallbackManager::CompileFunction;

  CompileCallbackMaterializationUnit(SymbolStringPtr Name,
                                     CompileFunction Compile)
      : MaterializationUnit(SymbolFlagsMap({{Name, JITSymbolFlags::Exported}})),
        Name(std::move(Name)), Compile(std::move(Compile)) {}

private:
  void materialize(MaterializationResponsibility R) {
    SymbolMap Result;
    Result[Name] = JITEvaluatedSymbol(Compile(), JITSymbolFlags::Exported);
    R.resolve(Result);
    R.emit();
  }

  void discard(const JITDylib &JD, SymbolStringPtr Name) {
    llvm_unreachable("Discard should never occur on a LMU?");
  }

  SymbolStringPtr Name;
  CompileFunction Compile;
};

} // namespace

namespace llvm {
namespace orc {

void JITCompileCallbackManager::anchor() {}
void IndirectStubsManager::anchor() {}

Expected<JITTargetAddress>
JITCompileCallbackManager::getCompileCallback(CompileFunction Compile) {
  if (auto TrampolineAddr = getAvailableTrampolineAddr()) {
    auto CallbackName = ES.getSymbolStringPool().intern(
        std::string("cc") + std::to_string(++NextCallbackId));

    std::lock_guard<std::mutex> Lock(CCMgrMutex);
    AddrToSymbol[*TrampolineAddr] = CallbackName;
    cantFail(CallbacksJD.define(
        llvm::make_unique<CompileCallbackMaterializationUnit>(
            std::move(CallbackName), std::move(Compile))));
    return *TrampolineAddr;
  } else
    return TrampolineAddr.takeError();
}

JITTargetAddress JITCompileCallbackManager::executeCompileCallback(
    JITTargetAddress TrampolineAddr) {
  SymbolStringPtr Name;

  {
    std::unique_lock<std::mutex> Lock(CCMgrMutex);
    auto I = AddrToSymbol.find(TrampolineAddr);

    // If this address is not associated with a compile callback then report an
    // error to the execution session and return ErrorHandlerAddress to the
    // callee.
    if (I == AddrToSymbol.end()) {
      Lock.unlock();
      std::string ErrMsg;
      {
        raw_string_ostream ErrMsgStream(ErrMsg);
        ErrMsgStream << "No compile callback for trampoline at "
                     << format("0x%016x", TrampolineAddr);
      }
      ES.reportError(
          make_error<StringError>(std::move(ErrMsg), inconvertibleErrorCode()));
      return ErrorHandlerAddress;
    } else
      Name = I->second;
  }

  if (auto Sym = lookup({&CallbacksJD}, Name))
    return Sym->getAddress();
  else {
    // If anything goes wrong materializing Sym then report it to the session
    // and return the ErrorHandlerAddress;
    ES.reportError(Sym.takeError());
    return ErrorHandlerAddress;
  }
}

std::unique_ptr<JITCompileCallbackManager>
createLocalCompileCallbackManager(const Triple &T, ExecutionSession &ES,
                                  JITTargetAddress ErrorHandlerAddress) {
  switch (T.getArch()) {
    default: return nullptr;

    case Triple::aarch64: {
      typedef orc::LocalJITCompileCallbackManager<orc::OrcAArch64> CCMgrT;
      return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
    }

    case Triple::x86: {
      typedef orc::LocalJITCompileCallbackManager<orc::OrcI386> CCMgrT;
      return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
    }

    case Triple::mips: {
      typedef orc::LocalJITCompileCallbackManager<orc::OrcMips32Be> CCMgrT;
      return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
    }
    case Triple::mipsel: {
      typedef orc::LocalJITCompileCallbackManager<orc::OrcMips32Le> CCMgrT;
      return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
    }
 
    case Triple::mips64:
    case Triple::mips64el: {
      typedef orc::LocalJITCompileCallbackManager<orc::OrcMips64> CCMgrT;
      return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
    }
 
    case Triple::x86_64: {
      if ( T.getOS() == Triple::OSType::Win32 ) {
        typedef orc::LocalJITCompileCallbackManager<orc::OrcX86_64_Win32> CCMgrT;
        return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
      } else {
        typedef orc::LocalJITCompileCallbackManager<orc::OrcX86_64_SysV> CCMgrT;
        return llvm::make_unique<CCMgrT>(ES, ErrorHandlerAddress);
      }
    }

  }
}

std::function<std::unique_ptr<IndirectStubsManager>()>
createLocalIndirectStubsManagerBuilder(const Triple &T) {
  switch (T.getArch()) {
    default:
      return [](){
        return llvm::make_unique<
                       orc::LocalIndirectStubsManager<orc::OrcGenericABI>>();
      };

    case Triple::aarch64:
      return [](){
        return llvm::make_unique<
                       orc::LocalIndirectStubsManager<orc::OrcAArch64>>();
      };

    case Triple::x86:
      return [](){
        return llvm::make_unique<
                       orc::LocalIndirectStubsManager<orc::OrcI386>>();
      };

    case Triple::mips:
      return [](){
          return llvm::make_unique<
                      orc::LocalIndirectStubsManager<orc::OrcMips32Be>>();
      };

    case Triple::mipsel:
      return [](){
          return llvm::make_unique<
                      orc::LocalIndirectStubsManager<orc::OrcMips32Le>>();
      };

    case Triple::mips64:
    case Triple::mips64el:
      return [](){
          return llvm::make_unique<
                      orc::LocalIndirectStubsManager<orc::OrcMips64>>();
      };
      
    case Triple::x86_64:
      if (T.getOS() == Triple::OSType::Win32) {
        return [](){
          return llvm::make_unique<
                     orc::LocalIndirectStubsManager<orc::OrcX86_64_Win32>>();
        };
      } else {
        return [](){
          return llvm::make_unique<
                     orc::LocalIndirectStubsManager<orc::OrcX86_64_SysV>>();
        };
      }

  }
}

Constant* createIRTypedAddress(FunctionType &FT, JITTargetAddress Addr) {
  Constant *AddrIntVal =
    ConstantInt::get(Type::getInt64Ty(FT.getContext()), Addr);
  Constant *AddrPtrVal =
    ConstantExpr::getCast(Instruction::IntToPtr, AddrIntVal,
                          PointerType::get(&FT, 0));
  return AddrPtrVal;
}

GlobalVariable* createImplPointer(PointerType &PT, Module &M,
                                  const Twine &Name, Constant *Initializer) {
  auto IP = new GlobalVariable(M, &PT, false, GlobalValue::ExternalLinkage,
                               Initializer, Name, nullptr,
                               GlobalValue::NotThreadLocal, 0, true);
  IP->setVisibility(GlobalValue::HiddenVisibility);
  return IP;
}

void makeStub(Function &F, Value &ImplPointer) {
  assert(F.isDeclaration() && "Can't turn a definition into a stub.");
  assert(F.getParent() && "Function isn't in a module.");
  Module &M = *F.getParent();
  BasicBlock *EntryBlock = BasicBlock::Create(M.getContext(), "entry", &F);
  IRBuilder<> Builder(EntryBlock);
  LoadInst *ImplAddr = Builder.CreateLoad(&ImplPointer);
  std::vector<Value*> CallArgs;
  for (auto &A : F.args())
    CallArgs.push_back(&A);
  CallInst *Call = Builder.CreateCall(ImplAddr, CallArgs);
  Call->setTailCall();
  Call->setAttributes(F.getAttributes());
  if (F.getReturnType()->isVoidTy())
    Builder.CreateRetVoid();
  else
    Builder.CreateRet(Call);
}

// Utility class for renaming global values and functions during partitioning.
class GlobalRenamer {
public:

  static bool needsRenaming(const Value &New) {
    return !New.hasName() || New.getName().startswith("\01L");
  }

  const std::string& getRename(const Value &Orig) {
    // See if we have a name for this global.
    {
      auto I = Names.find(&Orig);
      if (I != Names.end())
        return I->second;
    }

    // Nope. Create a new one.
    // FIXME: Use a more robust uniquing scheme. (This may blow up if the user
    //        writes a "__orc_anon[[:digit:]]* method).
    unsigned ID = Names.size();
    std::ostringstream NameStream;
    NameStream << "__orc_anon" << ID++;
    auto I = Names.insert(std::make_pair(&Orig, NameStream.str()));
    return I.first->second;
  }
private:
  DenseMap<const Value*, std::string> Names;
};

static void raiseVisibilityOnValue(GlobalValue &V, GlobalRenamer &R) {
  if (V.hasLocalLinkage()) {
    if (R.needsRenaming(V))
      V.setName(R.getRename(V));
    V.setLinkage(GlobalValue::ExternalLinkage);
    V.setVisibility(GlobalValue::HiddenVisibility);
  }
  V.setUnnamedAddr(GlobalValue::UnnamedAddr::None);
  assert(!R.needsRenaming(V) && "Invalid global name.");
}

void makeAllSymbolsExternallyAccessible(Module &M) {
  GlobalRenamer Renamer;

  for (auto &F : M)
    raiseVisibilityOnValue(F, Renamer);

  for (auto &GV : M.globals())
    raiseVisibilityOnValue(GV, Renamer);

  for (auto &A : M.aliases())
    raiseVisibilityOnValue(A, Renamer);
}

Function* cloneFunctionDecl(Module &Dst, const Function &F,
                            ValueToValueMapTy *VMap) {
  Function *NewF =
    Function::Create(cast<FunctionType>(F.getValueType()),
                     F.getLinkage(), F.getName(), &Dst);
  NewF->copyAttributesFrom(&F);

  if (VMap) {
    (*VMap)[&F] = NewF;
    auto NewArgI = NewF->arg_begin();
    for (auto ArgI = F.arg_begin(), ArgE = F.arg_end(); ArgI != ArgE;
         ++ArgI, ++NewArgI)
      (*VMap)[&*ArgI] = &*NewArgI;
  }

  return NewF;
}

void moveFunctionBody(Function &OrigF, ValueToValueMapTy &VMap,
                      ValueMaterializer *Materializer,
                      Function *NewF) {
  assert(!OrigF.isDeclaration() && "Nothing to move");
  if (!NewF)
    NewF = cast<Function>(VMap[&OrigF]);
  else
    assert(VMap[&OrigF] == NewF && "Incorrect function mapping in VMap.");
  assert(NewF && "Function mapping missing from VMap.");
  assert(NewF->getParent() != OrigF.getParent() &&
         "moveFunctionBody should only be used to move bodies between "
         "modules.");

  SmallVector<ReturnInst *, 8> Returns; // Ignore returns cloned.
  CloneFunctionInto(NewF, &OrigF, VMap, /*ModuleLevelChanges=*/true, Returns,
                    "", nullptr, nullptr, Materializer);
  OrigF.deleteBody();
}

GlobalVariable* cloneGlobalVariableDecl(Module &Dst, const GlobalVariable &GV,
                                        ValueToValueMapTy *VMap) {
  GlobalVariable *NewGV = new GlobalVariable(
      Dst, GV.getValueType(), GV.isConstant(),
      GV.getLinkage(), nullptr, GV.getName(), nullptr,
      GV.getThreadLocalMode(), GV.getType()->getAddressSpace());
  NewGV->copyAttributesFrom(&GV);
  if (VMap)
    (*VMap)[&GV] = NewGV;
  return NewGV;
}

void moveGlobalVariableInitializer(GlobalVariable &OrigGV,
                                   ValueToValueMapTy &VMap,
                                   ValueMaterializer *Materializer,
                                   GlobalVariable *NewGV) {
  assert(OrigGV.hasInitializer() && "Nothing to move");
  if (!NewGV)
    NewGV = cast<GlobalVariable>(VMap[&OrigGV]);
  else
    assert(VMap[&OrigGV] == NewGV &&
           "Incorrect global variable mapping in VMap.");
  assert(NewGV->getParent() != OrigGV.getParent() &&
         "moveGlobalVariableInitializer should only be used to move "
         "initializers between modules");

  NewGV->setInitializer(MapValue(OrigGV.getInitializer(), VMap, RF_None,
                                 nullptr, Materializer));
}

GlobalAlias* cloneGlobalAliasDecl(Module &Dst, const GlobalAlias &OrigA,
                                  ValueToValueMapTy &VMap) {
  assert(OrigA.getAliasee() && "Original alias doesn't have an aliasee?");
  auto *NewA = GlobalAlias::create(OrigA.getValueType(),
                                   OrigA.getType()->getPointerAddressSpace(),
                                   OrigA.getLinkage(), OrigA.getName(), &Dst);
  NewA->copyAttributesFrom(&OrigA);
  VMap[&OrigA] = NewA;
  return NewA;
}

void cloneModuleFlagsMetadata(Module &Dst, const Module &Src,
                              ValueToValueMapTy &VMap) {
  auto *MFs = Src.getModuleFlagsMetadata();
  if (!MFs)
    return;
  for (auto *MF : MFs->operands())
    Dst.addModuleFlag(MapMetadata(MF, VMap));
}

} // End namespace orc.
} // End namespace llvm.
