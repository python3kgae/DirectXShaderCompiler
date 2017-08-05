///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilPreparePasses.cpp                                                     //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Passes to prepare DxilModule.                                             //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilGenerationPass.h"
#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilModule.h"
#include "dxc/Support/Global.h"
#include "dxc/HLSL/DxilTypeSystem.h"
#include "dxc/HLSL/DxilUtil.h"
#include "dxc/HLSL/DxilFunctionProps.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Analysis/AssumptionCache.h"
#include <memory>
#include <unordered_set>

using namespace llvm;
using namespace hlsl;

namespace {
class SimplifyInst : public FunctionPass {
public:
  static char ID;

  SimplifyInst() : FunctionPass(ID) {
    initializeScalarizerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

private:
};
}

char SimplifyInst::ID = 0;

FunctionPass *llvm::createSimplifyInstPass() { return new SimplifyInst(); }

INITIALIZE_PASS(SimplifyInst, "simplify-inst", "Simplify Instructions", false, false)

bool SimplifyInst::runOnFunction(Function &F) {
  for (Function::iterator BBI = F.begin(), BBE = F.end(); BBI != BBE; ++BBI) {
    BasicBlock *BB = BBI;
    llvm::SimplifyInstructionsInBlock(BB, nullptr);
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////

namespace {
class DxilLoadMetadata : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilLoadMetadata () : ModulePass(ID) {}

  const char *getPassName() const override { return "HLSL load DxilModule from metadata"; }

  bool runOnModule(Module &M) override {
    if (!M.HasDxilModule()) {
      (void)M.GetOrCreateDxilModule();
      return true;
    }

    return false;
  }
};
}

char DxilLoadMetadata::ID = 0;

ModulePass *llvm::createDxilLoadMetadataPass() {
  return new DxilLoadMetadata();
}

INITIALIZE_PASS(DxilLoadMetadata, "hlsl-dxilload", "HLSL load DxilModule from metadata", false, false)

///////////////////////////////////////////////////////////////////////////////

namespace {
class DxilDeadFunctionElimination : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilDeadFunctionElimination () : ModulePass(ID) {}

  const char *getPassName() const override { return "Remove all unused function except entry from DxilModule"; }

  bool runOnModule(Module &M) override {
    if (M.HasDxilModule()) {
      DxilModule &DM = M.GetDxilModule();

      bool IsLib = DM.GetShaderModel()->IsLib();
      // Remove unused functions except entry and patch constant func.
      // For library profile, only remove unused external functions.
      Function *EntryFunc = DM.GetEntryFunction();
      Function *PatchConstantFunc = DM.GetPatchConstantFunction();

      return dxilutil::RemoveUnusedFunctions(M, EntryFunc, PatchConstantFunc,
                                             IsLib);
    }

    return false;
  }
};
}

char DxilDeadFunctionElimination::ID = 0;

ModulePass *llvm::createDxilDeadFunctionEliminationPass() {
  return new DxilDeadFunctionElimination();
}

INITIALIZE_PASS(DxilDeadFunctionElimination, "dxil-dfe", "Remove all unused function except entry from DxilModule", false, false)

///////////////////////////////////////////////////////////////////////////////

namespace {

Function *StripFunctionParameter(Function *F, DxilModule &DM,
    DenseMap<const Function *, DISubprogram *> &FunctionDIs) {
  Module &M = *DM.GetModule();
  Type *VoidTy = Type::getVoidTy(M.getContext());
  FunctionType *FT = FunctionType::get(VoidTy, false);
  for (auto &arg : F->args()) {
    if (!arg.user_empty())
      return nullptr;
    DbgDeclareInst *DDI = llvm::FindAllocaDbgDeclare(&arg);
    if (DDI) {
      DDI->eraseFromParent();
    }
  }

  Function *NewFunc = Function::Create(FT, F->getLinkage());
  M.getFunctionList().insert(F, NewFunc);
  // Splice the body of the old function right into the new function.
  NewFunc->getBasicBlockList().splice(NewFunc->begin(), F->getBasicBlockList());

  // Patch the pointer to LLVM function in debug info descriptor.
  auto DI = FunctionDIs.find(F);
  if (DI != FunctionDIs.end()) {
    DISubprogram *SP = DI->second;
    SP->replaceFunction(NewFunc);
    // Ensure the map is updated so it can be reused on subsequent argument
    // promotions of the same function.
    FunctionDIs.erase(DI);
    FunctionDIs[NewFunc] = SP;
  }
  NewFunc->takeName(F);
  if (DM.HasDxilFunctionProps(F)) {
    DM.ReplaceDxilEntrySignature(F, NewFunc);
    DM.ReplaceDxilFunctionProps(F, NewFunc);
  }
  // Save function fp flag
  DxilFunctionFPFlag flag;
  flag.SetFlagValue(DM.GetTypeSystem().GetFunctionAnnotation(F)->GetFlag().GetFlagValue());
  DM.GetTypeSystem().EraseFunctionAnnotation(F);
  F->eraseFromParent();
  DM.GetTypeSystem().AddFunctionAnnotationWithFPFlag(NewFunc, &flag);
  return NewFunc;
}

void CheckInBoundForTGSM(GlobalVariable &GV, const DataLayout &DL) {
  for (User *U : GV.users()) {
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
      bool allImmIndex = true;
      for (auto Idx = GEP->idx_begin(), E = GEP->idx_end(); Idx != E; Idx++) {
        if (!isa<ConstantInt>(Idx)) {
          allImmIndex = false;
          break;
        }
      }
      if (!allImmIndex)
        GEP->setIsInBounds(false);
      else {
        Value *Ptr = GEP->getPointerOperand();
        unsigned size =
            DL.getTypeAllocSize(Ptr->getType()->getPointerElementType());
        unsigned valSize =
            DL.getTypeAllocSize(GEP->getType()->getPointerElementType());
        SmallVector<Value *, 8> Indices(GEP->idx_begin(), GEP->idx_end());
        unsigned offset =
            DL.getIndexedOffset(GEP->getPointerOperandType(), Indices);
        if ((offset + valSize) > size)
          GEP->setIsInBounds(false);
      }
    }
  }
}

class DxilFinalizeModule : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilFinalizeModule() : ModulePass(ID) {}

  const char *getPassName() const override { return "HLSL DXIL Metadata Emit"; }

  void patchValidation_1_1(Module &M) {
    for (iplist<Function>::iterator F : M.getFunctionList()) {
      for (Function::iterator BBI = F->begin(), BBE = F->end(); BBI != BBE;
           ++BBI) {
        BasicBlock *BB = BBI;
        for (BasicBlock::iterator II = BB->begin(), IE = BB->end(); II != IE;
             ++II) {
          Instruction *I = II;
          if (I->hasMetadataOtherThanDebugLoc()) {
            SmallVector<std::pair<unsigned, MDNode*>, 2> MDs;
            I->getAllMetadataOtherThanDebugLoc(MDs);
            for (auto &MD : MDs) {
              unsigned kind = MD.first;
              // Remove Metadata which validation_1_0 not allowed.
              bool bNeedPatch = kind == LLVMContext::MD_tbaa ||
                  kind == LLVMContext::MD_prof ||
                  (kind > LLVMContext::MD_fpmath &&
                  kind <= LLVMContext::MD_dereferenceable_or_null);
              if (bNeedPatch)
                I->setMetadata(kind, nullptr);
            }
          }
        }
      }
    }
  }

  bool runOnModule(Module &M) override {
    if (M.HasDxilModule()) {
      DxilModule &DM = M.GetDxilModule();

      bool IsLib = DM.GetShaderModel()->IsLib();
      // Skip validation patch for lib.
      if (!IsLib) {
        unsigned ValMajor = 0;
        unsigned ValMinor = 0;
        M.GetDxilModule().GetValidatorVersion(ValMajor, ValMinor);
        if (ValMajor == 1 && ValMinor <= 1) {
          patchValidation_1_1(M);
        }
      }

      // Remove store undef output.
      hlsl::OP *hlslOP = M.GetDxilModule().GetOP();
      RemoveStoreUndefOutput(M, hlslOP);

      RemoveUnusedStaticGlobal(M);

      // Clear inbound for GEP which has none-const index.
      LegalizeShareMemoryGEPInbound(M);

      // Strip parameters of entry function.
      StripEntryParameters(M, DM, IsLib);

      // Skip shader flag for library.
      if (!IsLib) {
        DM.CollectShaderFlags(); // Update flags to reflect any changes.
                                 // Update Validator Version
        DM.UpgradeToMinValidatorVersion();
      }
      return true;
    }

    return false;
  }

private:
  void RemoveUnusedStaticGlobal(Module &M) {
    // Remove unused internal global.
    std::vector<GlobalVariable *> staticGVs;
    for (GlobalVariable &GV : M.globals()) {
      if (dxilutil::IsStaticGlobal(&GV) ||
          dxilutil::IsSharedMemoryGlobal(&GV)) {
        staticGVs.emplace_back(&GV);
      }
    }

    for (GlobalVariable *GV : staticGVs) {
      bool onlyStoreUse = true;
      for (User *user : GV->users()) {
        if (isa<StoreInst>(user))
          continue;
        if (isa<ConstantExpr>(user) && user->user_empty())
          continue;
        onlyStoreUse = false;
        break;
      }
      if (onlyStoreUse) {
        for (auto UserIt = GV->user_begin(); UserIt != GV->user_end();) {
          Value *User = *(UserIt++);
          if (Instruction *I = dyn_cast<Instruction>(User)) {
            I->eraseFromParent();
          } else {
            ConstantExpr *CE = cast<ConstantExpr>(User);
            CE->dropAllReferences();
          }
        }
        GV->eraseFromParent();
      }
    }
  }

  void RemoveStoreUndefOutput(Module &M, hlsl::OP *hlslOP) {
    for (iplist<Function>::iterator F : M.getFunctionList()) {
      if (!hlslOP->IsDxilOpFunc(F))
        continue;
      DXIL::OpCodeClass opClass;
      bool bHasOpClass = hlslOP->GetOpCodeClass(F, opClass);
      DXASSERT_LOCALVAR(bHasOpClass, bHasOpClass, "else not a dxil op func");
      if (opClass != DXIL::OpCodeClass::StoreOutput)
        continue;

      for (auto it = F->user_begin(); it != F->user_end();) {
        CallInst *CI = dyn_cast<CallInst>(*(it++));
        if (!CI)
          continue;

        Value *V = CI->getArgOperand(DXIL::OperandIndex::kStoreOutputValOpIdx);
        // Remove the store of undef.
        if (isa<UndefValue>(V))
          CI->eraseFromParent();
      }
    }
  }

  void LegalizeShareMemoryGEPInbound(Module &M) {
    const DataLayout &DL = M.getDataLayout();
    // Clear inbound for GEP which has none-const index.
    for (GlobalVariable &GV : M.globals()) {
      if (dxilutil::IsSharedMemoryGlobal(&GV)) {
        CheckInBoundForTGSM(GV, DL);
      }
    }
  }

  void StripEntryParameters(Module &M, DxilModule &DM, bool IsLib) {
    DenseMap<const Function *, DISubprogram *> FunctionDIs =
        makeSubprogramMap(M);
    // Strip parameters of entry function.
    if (!IsLib) {
      if (Function *PatchConstantFunc = DM.GetPatchConstantFunction()) {
        PatchConstantFunc =
            StripFunctionParameter(PatchConstantFunc, DM, FunctionDIs);
        if (PatchConstantFunc)
          DM.SetPatchConstantFunction(PatchConstantFunc);
      }

      if (Function *EntryFunc = DM.GetEntryFunction()) {
        StringRef Name = DM.GetEntryFunctionName();
        EntryFunc->setName(Name);
        EntryFunc = StripFunctionParameter(EntryFunc, DM, FunctionDIs);
        if (EntryFunc)
          DM.SetEntryFunction(EntryFunc);
      }
    } else {
      std::vector<Function *> entries;
      for (iplist<Function>::iterator F : M.getFunctionList()) {
        if (DM.HasDxilFunctionProps(F)) {
          entries.emplace_back(F);
        }
      }
      for (Function *entry : entries) {
        DxilFunctionProps &props = DM.GetDxilFunctionProps(entry);
        if (props.IsHS()) {
          // Strip patch constant function first.
          Function *patchConstFunc = StripFunctionParameter(
              props.ShaderProps.HS.patchConstantFunc, DM, FunctionDIs);
          props.ShaderProps.HS.patchConstantFunc = patchConstFunc;
        }
        StripFunctionParameter(entry, DM, FunctionDIs);
      }
    }
  }
};
}

char DxilFinalizeModule::ID = 0;

ModulePass *llvm::createDxilFinalizeModulePass() {
  return new DxilFinalizeModule();
}

INITIALIZE_PASS(DxilFinalizeModule, "hlsl-dxilfinalize", "HLSL DXIL Finalize Module", false, false)


///////////////////////////////////////////////////////////////////////////////

namespace {

class DxilEmitMetadata : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilEmitMetadata() : ModulePass(ID) {}

  const char *getPassName() const override { return "HLSL DXIL Metadata Emit"; }

  bool runOnModule(Module &M) override {
    if (M.HasDxilModule()) {
      M.GetDxilModule().EmitDxilMetadata();
      return true;
    }

    return false;
  }
};
}

char DxilEmitMetadata::ID = 0;

ModulePass *llvm::createDxilEmitMetadataPass() {
  return new DxilEmitMetadata();
}

INITIALIZE_PASS(DxilEmitMetadata, "hlsl-dxilemit", "HLSL DXIL Metadata Emit", false, false)
