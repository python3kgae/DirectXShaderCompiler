//===--- EmitSPIRVAction.cpp - EmitSPIRVAction implementation -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/SPIRV/EmitSPIRVAction.h"

#include "dxc/HlslIntrinsicOp.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/SPIRV/DeclResultIdMapper.h"
#include "clang/SPIRV/ModuleBuilder.h"
#include "clang/SPIRV/TypeTranslator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"

namespace clang {
namespace spirv {

namespace {

// TODO: Maybe we should move these type probing functions to TypeTranslator.

/// Returns true if the given type is a bool or vector of bool type.
bool isBoolOrVecOfBoolType(QualType type) {
  return type->isBooleanType() ||
         (hlsl::IsHLSLVecType(type) &&
          hlsl::GetHLSLVecElementType(type)->isBooleanType());
}

/// Returns true if the given type is a signed integer or vector of signed
/// integer type.
bool isSintOrVecOfSintType(QualType type) {
  return type->isSignedIntegerType() ||
         (hlsl::IsHLSLVecType(type) &&
          hlsl::GetHLSLVecElementType(type)->isSignedIntegerType());
}

/// Returns true if the given type is an unsigned integer or vector of unsigned
/// integer type.
bool isUintOrVecOfUintType(QualType type) {
  return type->isUnsignedIntegerType() ||
         (hlsl::IsHLSLVecType(type) &&
          hlsl::GetHLSLVecElementType(type)->isUnsignedIntegerType());
}

/// Returns true if the given type is a float or vector of float type.
bool isFloatOrVecOfFloatType(QualType type) {
  return type->isFloatingType() ||
         (hlsl::IsHLSLVecType(type) &&
          hlsl::GetHLSLVecElementType(type)->isFloatingType());
}

/// Returns true if the given type is a bool or vector/matrix of bool type.
bool isBoolOrVecMatOfBoolType(QualType type) {
  return isBoolOrVecOfBoolType(type) ||
         (hlsl::IsHLSLMatType(type) &&
          hlsl::GetHLSLMatElementType(type)->isBooleanType());
}

/// Returns true if the given type is a signed integer or vector/matrix of
/// signed integer type.
bool isSintOrVecMatOfSintType(QualType type) {
  return isSintOrVecOfSintType(type) ||
         (hlsl::IsHLSLMatType(type) &&
          hlsl::GetHLSLMatElementType(type)->isSignedIntegerType());
}

/// Returns true if the given type is an unsigned integer or vector/matrix of
/// unsigned integer type.
bool isUintOrVecMatOfUintType(QualType type) {
  return isUintOrVecOfUintType(type) ||
         (hlsl::IsHLSLMatType(type) &&
          hlsl::GetHLSLMatElementType(type)->isUnsignedIntegerType());
}

/// Returns true if the given type is a float or vector/matrix of float type.
bool isFloatOrVecMatOfFloatType(QualType type) {
  return isFloatOrVecOfFloatType(type) ||
         (hlsl::IsHLSLMatType(type) &&
          hlsl::GetHLSLMatElementType(type)->isFloatingType());
}

bool isCompoundAssignment(BinaryOperatorKind opcode) {
  switch (opcode) {
  case BO_AddAssign:
  case BO_SubAssign:
  case BO_MulAssign:
  case BO_DivAssign:
  case BO_RemAssign:
  case BO_AndAssign:
  case BO_OrAssign:
  case BO_XorAssign:
  case BO_ShlAssign:
  case BO_ShrAssign:
    return true;
  default:
    return false;
  }
}

} // namespace

/// SPIR-V emitter class. It consumes the HLSL AST and emits SPIR-V words.
///
/// This class only overrides the HandleTranslationUnit() method; Traversing
/// through the AST is done manually instead of using ASTConsumer's harness.
class SPIRVEmitter : public ASTConsumer {
public:
  explicit SPIRVEmitter(CompilerInstance &ci)
      : theCompilerInstance(ci), astContext(ci.getASTContext()),
        diags(ci.getDiagnostics()),
        entryFunctionName(ci.getCodeGenOpts().HLSLEntryFunction),
        shaderStage(getSpirvShaderStageFromHlslProfile(
            ci.getCodeGenOpts().HLSLProfile.c_str())),
        theContext(), theBuilder(&theContext),
        declIdMapper(shaderStage, theBuilder, diags),
        typeTranslator(theBuilder, diags), entryFunctionId(0),
        curFunction(nullptr) {}

  spv::ExecutionModel getSpirvShaderStageFromHlslProfile(const char *profile) {
    assert(profile && "nullptr passed as HLSL profile.");

    // DXIL Models are:
    // Profile (DXIL Model) : HLSL Shader Kind : SPIR-V Shader Stage
    // vs_<version>         : Vertex Shader    : Vertex Shader
    // hs_<version>         : Hull Shader      : Tassellation Control Shader
    // ds_<version>         : Domain Shader    : Tessellation Evaluation Shader
    // gs_<version>         : Geometry Shader  : Geometry Shader
    // ps_<version>         : Pixel Shader     : Fragment Shader
    // cs_<version>         : Compute Shader   : Compute Shader
    switch (profile[0]) {
    case 'v':
      return spv::ExecutionModel::Vertex;
    case 'h':
      return spv::ExecutionModel::TessellationControl;
    case 'd':
      return spv::ExecutionModel::TessellationEvaluation;
    case 'g':
      return spv::ExecutionModel::Geometry;
    case 'p':
      return spv::ExecutionModel::Fragment;
    case 'c':
      return spv::ExecutionModel::GLCompute;
    default:
      emitError("Unknown HLSL Profile: %0") << profile;
      return spv::ExecutionModel::Fragment;
    }
  }

  void AddRequiredCapabilitiesForExecutionModel(spv::ExecutionModel em) {
    if (em == spv::ExecutionModel::TessellationControl ||
        em == spv::ExecutionModel::TessellationEvaluation) {
      theBuilder.requireCapability(spv::Capability::Tessellation);
      emitError("Tasselation shaders are currently not supported.");
    } else if (em == spv::ExecutionModel::Geometry) {
      theBuilder.requireCapability(spv::Capability::Geometry);
      emitError("Geometry shaders are currently not supported.");
    } else {
      theBuilder.requireCapability(spv::Capability::Shader);
    }
  }

  /// \brief Adds the execution mode for the given entry point based on the
  /// execution model.
  void AddExecutionModeForEntryPoint(spv::ExecutionModel execModel,
                                     uint32_t entryPointId) {
    if (execModel == spv::ExecutionModel::Fragment) {
      // TODO: Implement the logic to determine the proper Execution Mode for
      // fragment shaders. Currently using OriginUpperLeft as default.
      theBuilder.addExecutionMode(entryPointId,
                                  spv::ExecutionMode::OriginUpperLeft, {});
      emitWarning("Execution mode for fragment shaders is "
                  "currently set to OriginUpperLeft by default.");
    } else {
      emitWarning(
          "Execution mode is currently only defined for fragment shaders.");
      // TODO: Implement logic for adding proper execution mode for other
      // shader stages. Silently skipping for now.
    }
  }

  void HandleTranslationUnit(ASTContext &context) override {
    const spv::ExecutionModel em = getSpirvShaderStageFromHlslProfile(
        theCompilerInstance.getCodeGenOpts().HLSLProfile.c_str());
    AddRequiredCapabilitiesForExecutionModel(em);

    // Addressing and memory model are required in a valid SPIR-V module.
    theBuilder.setAddressingModel(spv::AddressingModel::Logical);
    theBuilder.setMemoryModel(spv::MemoryModel::GLSL450);

    TranslationUnitDecl *tu = context.getTranslationUnitDecl();

    // The entry function is the seed of the queue.
    for (auto *decl : tu->decls()) {
      if (auto *funcDecl = dyn_cast<FunctionDecl>(decl)) {
        if (funcDecl->getName() == entryFunctionName) {
          workQueue.insert(funcDecl);
        }
      }
    }
    // TODO: enlarge the queue upon seeing a function call.

    // Translate all functions reachable from the entry function.
    // The queue can grow in the meanwhile; so need to keep evaluating
    // workQueue.size().
    for (uint32_t i = 0; i < workQueue.size(); ++i) {
      doDecl(workQueue[i]);
    }

    theBuilder.addEntryPoint(shaderStage, entryFunctionId, entryFunctionName,
                             declIdMapper.collectStageVariables());

    AddExecutionModeForEntryPoint(shaderStage, entryFunctionId);

    // Add Location decorations to stage input/output variables.
    declIdMapper.finalizeStageIOLocations();

    // Output the constructed module.
    std::vector<uint32_t> m = theBuilder.takeModule();
    theCompilerInstance.getOutStream()->write(
        reinterpret_cast<const char *>(m.data()), m.size() * 4);
  }

  void doDecl(const Decl *decl) {
    if (const auto *varDecl = dyn_cast<VarDecl>(decl)) {
      doVarDecl(varDecl);
    } else if (const auto *funcDecl = dyn_cast<FunctionDecl>(decl)) {
      doFunctionDecl(funcDecl);
    } else {
      // TODO: Implement handling of other Decl types.
      emitWarning("Decl type '%0' is not supported yet.")
          << std::string(decl->getDeclKindName());
    }
  }

  void doFunctionDecl(const FunctionDecl *decl) {
    curFunction = decl;

    const llvm::StringRef funcName = decl->getName();

    uint32_t funcId;

    if (funcName == entryFunctionName) {
      // First create stage variables for the entry point.
      declIdMapper.createStageVarFromFnReturn(decl);
      for (const auto *param : decl->params())
        declIdMapper.createStageVarFromFnParam(param);

      // Construct the function signature.
      const uint32_t voidType = theBuilder.getVoidType();
      const uint32_t funcType = theBuilder.getFunctionType(voidType, {});

      // The entry function surely does not have pre-assigned <result-id> for
      // it like other functions that got added to the work queue following
      // function calls.
      funcId = theBuilder.beginFunction(funcType, voidType, funcName);

      // Record the entry function's <result-id>.
      entryFunctionId = funcId;
    } else {
      const uint32_t retType =
          typeTranslator.translateType(decl->getReturnType());

      // Construct the function signature.
      llvm::SmallVector<uint32_t, 4> paramTypes;
      for (const auto *param : decl->params()) {
        const uint32_t valueType =
            typeTranslator.translateType(param->getType());
        const uint32_t ptrType =
            theBuilder.getPointerType(valueType, spv::StorageClass::Function);
        paramTypes.push_back(ptrType);
      }
      const uint32_t funcType = theBuilder.getFunctionType(retType, paramTypes);

      // Non-entry functions are added to the work queue following function
      // calls. We have already assigned <result-id>s for it when translating
      // its call site. Query it here.
      funcId = declIdMapper.getDeclResultId(decl);
      theBuilder.beginFunction(funcType, retType, funcName, funcId);

      // Create all parameters.
      for (uint32_t i = 0; i < decl->getNumParams(); ++i) {
        const ParmVarDecl *paramDecl = decl->getParamDecl(i);
        const uint32_t paramId =
            theBuilder.addFnParameter(paramTypes[i], paramDecl->getName());
        declIdMapper.registerDeclResultId(paramDecl, paramId);
      }
    }

    if (decl->hasBody()) {
      // The entry basic block.
      const uint32_t entryLabel = theBuilder.createBasicBlock("bb.entry");
      theBuilder.setInsertPoint(entryLabel);

      // Process all statments in the body.
      doStmt(decl->getBody());

      // We have processed all Stmts in this function and now in the last
      // basic block. Make sure we have OpReturn if missing.
      if (!theBuilder.isCurrentBasicBlockTerminated()) {
        theBuilder.createReturn();
      }
    }

    theBuilder.endFunction();

    curFunction = nullptr;
  }

  void doVarDecl(const VarDecl *decl) {
    if (decl->isLocalVarDecl()) {
      const uint32_t ptrType = theBuilder.getPointerType(
          typeTranslator.translateType(decl->getType()),
          spv::StorageClass::Function);

      // Handle initializer. SPIR-V requires that "initializer must be an <id>
      // from a constant instruction or a global (module scope) OpVariable
      // instruction."
      llvm::Optional<uint32_t> constInitializer = llvm::None;
      uint32_t varInitializer = 0;
      if (decl->hasInit()) {
        const Expr *declInit = decl->getInit();

        // First try to evaluate the initializer as a constant expression
        Expr::EvalResult evalResult;
        if (declInit->EvaluateAsRValue(evalResult, astContext) &&
            !evalResult.HasSideEffects) {
          constInitializer = llvm::Optional<uint32_t>(
              translateAPValue(evalResult.Val, decl->getType()));
        }

        // If we cannot evaluate the initializer as a constant expression, we'll
        // need use OpStore to write the initializer to the variable.
        if (!constInitializer.hasValue()) {
          varInitializer = doExpr(declInit);
        }
      }

      const uint32_t varId =
          theBuilder.addFnVariable(ptrType, decl->getName(), constInitializer);
      declIdMapper.registerDeclResultId(decl, varId);

      if (varInitializer) {
        theBuilder.createStore(varId, varInitializer);
      }
    } else {
      // TODO: handle global variables
      emitError("Global variables are not supported yet.");
    }
  }

  void doStmt(const Stmt *stmt, llvm::ArrayRef<const Attr *> attrs = {}) {
    if (const auto *compoundStmt = dyn_cast<CompoundStmt>(stmt)) {
      for (auto *st : compoundStmt->body())
        doStmt(st);
    } else if (const auto *retStmt = dyn_cast<ReturnStmt>(stmt)) {
      doReturnStmt(retStmt);
    } else if (const auto *declStmt = dyn_cast<DeclStmt>(stmt)) {
      for (auto *decl : declStmt->decls()) {
        doDecl(decl);
      }
    } else if (const auto *ifStmt = dyn_cast<IfStmt>(stmt)) {
      doIfStmt(ifStmt);
    } else if (const auto *switchStmt = dyn_cast<SwitchStmt>(stmt)) {
      doSwitchStmt(switchStmt, attrs);
    } else if (const auto *caseStmt = dyn_cast<CaseStmt>(stmt)) {
      processCaseStmtOrDefaultStmt(stmt);
    } else if (const auto *defaultStmt = dyn_cast<DefaultStmt>(stmt)) {
      processCaseStmtOrDefaultStmt(stmt);
    } else if (const auto *breakStmt = dyn_cast<BreakStmt>(stmt)) {
      doBreakStmt(breakStmt);
    } else if (const auto *forStmt = dyn_cast<ForStmt>(stmt)) {
      doForStmt(forStmt);
    } else if (const auto *nullStmt = dyn_cast<NullStmt>(stmt)) {
      // For the null statement ";". We don't need to do anything.
    } else if (const auto *expr = dyn_cast<Expr>(stmt)) {
      // All cases for expressions used as statements
      doExpr(expr);
    } else if (const auto *attrStmt = dyn_cast<AttributedStmt>(stmt)) {
      doStmt(attrStmt->getSubStmt(), attrStmt->getAttrs());
    } else {
      emitError("Stmt '%0' is not supported yet.") << stmt->getStmtClassName();
    }
  }

  void doReturnStmt(const ReturnStmt *stmt) {
    // For normal functions, just return in the normal way.
    if (curFunction->getName() != entryFunctionName) {
      theBuilder.createReturnValue(doExpr(stmt->getRetValue()));
      return;
    }

    // SPIR-V requires the signature of entry functions to be void(), while
    // in HLSL we can have non-void parameter and return types for entry points.
    // So we should treat the ReturnStmt in entry functions specially.
    //
    // We need to walk through the return type, and for each subtype attached
    // with semantics, write out the value to the corresponding stage variable
    // mapped to the semantic.

    const uint32_t stageVarId =
        declIdMapper.getRemappedDeclResultId(curFunction);

    if (stageVarId) {
      // The return value is mapped to a single stage variable. We just need
      // to store the value into the stage variable instead.
      theBuilder.createStore(stageVarId, doExpr(stmt->getRetValue()));
      theBuilder.createReturn();
      return;
    }

    QualType retType = stmt->getRetValue()->getType();

    if (const auto *structType = retType->getAsStructureType()) {
      // We are trying to return a value of struct type.

      // First get the return value. Clang AST will use an LValueToRValue cast
      // for returning a struct variable. We need to ignore the cast to avoid
      // creating OpLoad instruction since we need the pointer to the variable
      // for creating access chain later.
      const uint32_t retValue =
          doExpr(stmt->getRetValue()->IgnoreParenLValueCasts());

      // Then go through all fields.
      uint32_t fieldIndex = 0;
      for (const auto *field : structType->getDecl()->fields()) {
        // Load the value from the current field.
        const uint32_t valueType =
            typeTranslator.translateType(field->getType());
        // TODO: We may need to change the storage class accordingly.
        const uint32_t ptrType = theBuilder.getPointerType(
            typeTranslator.translateType(field->getType()),
            spv::StorageClass::Function);
        const uint32_t indexId = theBuilder.getConstantInt32(fieldIndex++);
        const uint32_t valuePtr =
            theBuilder.createAccessChain(ptrType, retValue, {indexId});
        const uint32_t value = theBuilder.createLoad(valueType, valuePtr);
        // Store it to the corresponding stage variable.
        const uint32_t targetVar = declIdMapper.getDeclResultId(field);
        theBuilder.createStore(targetVar, value);
      }
    } else {
      emitError("Return type '%0' for entry function is not supported yet.")
          << retType->getTypeClassName();
    }
  }

  /// \brief Returns true iff *all* the case values in the given switch
  /// statement are integer literals. In such cases OpSwitch can be used to
  /// represent the switch statement.
  /// We only care about the case values to be compared with the selector. They
  /// may appear in the top level CaseStmt or be nested in a CompoundStmt.Fall
  /// through cases will result in the second situation.
  bool allSwitchCasesAreIntegerLiterals(const Stmt *root) {
    if (!root)
      return false;

    const auto *caseStmt = dyn_cast<CaseStmt>(root);
    const auto *compoundStmt = dyn_cast<CompoundStmt>(root);
    if (!caseStmt && !compoundStmt)
      return true;

    if (caseStmt) {
      const Expr *caseExpr = caseStmt->getLHS();
      return caseExpr && caseExpr->isEvaluatable(astContext);
    }

    // Recurse down if facing a compound statement.
    for (auto *st : compoundStmt->body())
      if (!allSwitchCasesAreIntegerLiterals(st))
        return false;

    return true;
  }

  /// \brief Recursively discovers all CaseStmt and DefaultStmt under the
  /// sub-tree of the given root. Recursively goes down the tree iff it finds a
  /// CaseStmt, DefaultStmt, or CompoundStmt. It does not recurse on other
  /// statement types. For each discovered case, a basic block is created and
  /// registered within the module, and added as a successor to the current
  /// active basic block.
  ///
  /// Writes a vector of (integer, basic block label) pairs for all cases to the
  /// given 'targets' argument. If a DefaultStmt is found, it also returns the
  /// label for the default basic block through the defaultBB parameter. This
  /// method panics if it finds a case value that is not an integer literal.
  void discoverAllCaseStmtInSwitchStmt(
      const Stmt *root, uint32_t *defaultBB,
      std::vector<std::pair<uint32_t, uint32_t>> *targets) {
    if (!root)
      return;

    // A switch case can only appear in DefaultStmt, CaseStmt, or
    // CompoundStmt. For the rest, we can just return.
    const auto *defaultStmt = dyn_cast<DefaultStmt>(root);
    const auto *caseStmt = dyn_cast<CaseStmt>(root);
    const auto *compoundStmt = dyn_cast<CompoundStmt>(root);
    if (!defaultStmt && !caseStmt && !compoundStmt)
      return;

    // Recurse down if facing a compound statement.
    if (compoundStmt) {
      for (auto *st : compoundStmt->body())
        discoverAllCaseStmtInSwitchStmt(st, defaultBB, targets);
      return;
    }

    std::string caseLabel;
    uint32_t caseValue = 0;
    if (defaultStmt) {
      // This is the default branch.
      caseLabel = "switch.default";
    } else if (caseStmt) {
      // This is a non-default case.
      // When using OpSwitch, we only allow integer literal cases. e.g:
      // case <literal_integer>: {...; break;}
      const Expr *caseExpr = caseStmt->getLHS();
      assert(caseExpr && caseExpr->isEvaluatable(astContext));
      auto bitWidth = astContext.getIntWidth(caseExpr->getType());
      if (bitWidth != 32)
        emitError("Switch statement translation currently only supports 32-bit "
                  "integer case values.");
      Expr::EvalResult evalResult;
      caseExpr->EvaluateAsRValue(evalResult, astContext);
      const int64_t value = evalResult.Val.getInt().getSExtValue();
      caseValue = static_cast<uint32_t>(value);
      caseLabel = "switch." + std::string(value < 0 ? "n" : "") +
                  llvm::itostr(std::abs(value));
    }
    const uint32_t caseBB = theBuilder.createBasicBlock(caseLabel);
    theBuilder.addSuccessor(caseBB);
    stmtBasicBlock[root] = caseBB;

    // Add all cases to the 'targets' vector.
    if (caseStmt)
      targets->emplace_back(caseValue, caseBB);

    // The default label is not part of the 'targets' vector that is passed
    // to the OpSwitch instruction.
    // If default statement was discovered, return its label via defaultBB.
    if (defaultStmt)
      *defaultBB = caseBB;

    // Process cases nested in other cases. It happens when we have fall through
    // cases. For example:
    // case 1: case 2: ...; break;
    // will result in the CaseSmt for case 2 nested in the one for case 1.
    discoverAllCaseStmtInSwitchStmt(caseStmt ? caseStmt->getSubStmt()
                                             : defaultStmt->getSubStmt(),
                                    defaultBB, targets);
  }

  void processSwitchStmtUsingSpirvOpSwitch(const SwitchStmt *switchStmt) {
    // First handle the condition variable DeclStmt if one exists.
    // For example: handle 'int a = b' in the following:
    // switch (int a = b) {...}
    const auto *condVarDeclStmt = switchStmt->getConditionVariableDeclStmt();
    if (condVarDeclStmt)
      doStmt(condVarDeclStmt);

    const uint32_t selector = doExpr(switchStmt->getCond());

    // We need a merge block regardless of the number of switch cases.
    // Since OpSwitch always requires a default label, if the switch statement
    // does not have a default branch, we use the merge block as the default
    // target.
    const uint32_t mergeBB = theBuilder.createBasicBlock("switch.merge");
    theBuilder.setMergeTarget(mergeBB);
    breakStack.push(mergeBB);
    uint32_t defaultBB = mergeBB;

    // (literal, labelId) pairs to pass to the OpSwitch instruction.
    std::vector<std::pair<uint32_t, uint32_t>> targets;
    discoverAllCaseStmtInSwitchStmt(switchStmt->getBody(), &defaultBB,
                                    &targets);

    // Create the OpSelectionMerge and OpSwitch.
    theBuilder.createSwitch(mergeBB, selector, defaultBB, targets);

    // Handle the switch body.
    doStmt(switchStmt->getBody());

    if (!theBuilder.isCurrentBasicBlockTerminated())
      theBuilder.createBranch(mergeBB);
    theBuilder.setInsertPoint(mergeBB);
    breakStack.pop();
  }

  void processSwitchStmtUsingIfStmts(const SwitchStmt *switchStmt) {
    emitError("Translating Switch statements using If statements is not "
              "implemented yet.");
  }

  void doSwitchStmt(const SwitchStmt *switchStmt,
                    llvm::ArrayRef<const Attr *> attrs = {}) {
    // Switch statements are composed of:
    //   switch (<condition variable>) {
    //     <CaseStmt>
    //     <CaseStmt>
    //     <CaseStmt>
    //     <DefaultStmt> (optional)
    //   }
    //
    //                             +-------+
    //                             | check |
    //                             +-------+
    //                                 |
    //         +-------+-------+----------------+---------------+
    //         | 1             | 2              | 3             | (others)
    //         v               v                v               v
    //     +-------+      +-------------+     +-------+     +------------+
    //     | case1 |      | case2       |     | case3 | ... | default    |
    //     |       |      |(fallthrough)|---->|       |     | (optional) |
    //     +-------+      |+------------+     +-------+     +------------+
    //         |                                  |                |
    //         |                                  |                |
    //         |   +-------+                      |                |
    //         |   |       | <--------------------+                |
    //         +-> | merge |                                       |
    //             |       | <-------------------------------------+
    //             +-------+

    // If no attributes are given, or if "forcecase" attribute was provided,
    // we'll do our best to use OpSwitch if possible.
    // If any of the cases compares to a variable (rather than an integer
    // literal), we cannot use OpSwitch because OpSwitch expects literal
    // numbers as parameters.
    const bool isAttrForceCase =
        !attrs.empty() && attrs.front()->getKind() == attr::HLSLForceCase;
    const bool canUseSpirvOpSwitch =
        (attrs.empty() || isAttrForceCase) &&
        allSwitchCasesAreIntegerLiterals(switchStmt->getBody());

    if (isAttrForceCase && !canUseSpirvOpSwitch)
      emitWarning("Ignored 'forcecase' attribute for the switch statement "
                  "since one or more case values are not integer literals.");

    if (canUseSpirvOpSwitch)
      processSwitchStmtUsingSpirvOpSwitch(switchStmt);
    else
      processSwitchStmtUsingIfStmts(switchStmt);
  }

  void processCaseStmtOrDefaultStmt(const Stmt *stmt) {
    auto *caseStmt = dyn_cast<CaseStmt>(stmt);
    auto *defaultStmt = dyn_cast<DefaultStmt>(stmt);
    assert(caseStmt || defaultStmt);

    uint32_t caseBB = stmtBasicBlock[stmt];
    if (!theBuilder.isCurrentBasicBlockTerminated()) {
      // We are about to handle the case passed in as parameter. If the current
      // basic block is not terminated, it means the previous case is a fall
      // through case. We need to link it to the case to be processed.
      theBuilder.createBranch(caseBB);
      theBuilder.addSuccessor(caseBB);
    }
    theBuilder.setInsertPoint(caseBB);
    doStmt(caseStmt ? caseStmt->getSubStmt() : defaultStmt->getSubStmt());
  }

  void doBreakStmt(const BreakStmt *breakStmt) {
    uint32_t breakTargetBB = breakStack.top();
    theBuilder.addSuccessor(breakTargetBB);
    theBuilder.createBranch(breakTargetBB);
  }

  void doIfStmt(const IfStmt *ifStmt) {
    // if statements are composed of:
    //   if (<check>) { <then> } else { <else> }
    //
    // To translate if statements, we'll need to emit the <check> expressions
    // in the current basic block, and then create separate basic blocks for
    // <then> and <else>. Additionally, we'll need a <merge> block as per
    // SPIR-V's structured control flow requirements. Depending whether there
    // exists the else branch, the final CFG should normally be like the
    // following. Exceptions will occur with non-local exits like loop breaks
    // or early returns.
    //             +-------+                        +-------+
    //             | check |                        | check |
    //             +-------+                        +-------+
    //                 |                                |
    //         +-------+-------+                  +-----+-----+
    //         | true          | false            | true      | false
    //         v               v         or       v           |
    //     +------+         +------+           +------+       |
    //     | then |         | else |           | then |       |
    //     +------+         +------+           +------+       |
    //         |               |                  |           v
    //         |   +-------+   |                  |     +-------+
    //         +-> | merge | <-+                  +---> | merge |
    //             +-------+                            +-------+

    // First emit the instruction for evaluating the condition.
    const uint32_t condition = doExpr(ifStmt->getCond());

    // Then we need to emit the instruction for the conditional branch.
    // We'll need the <label-id> for the then/else/merge block to do so.
    const bool hasElse = ifStmt->getElse() != nullptr;
    const uint32_t thenBB = theBuilder.createBasicBlock("if.true");
    const uint32_t mergeBB = theBuilder.createBasicBlock("if.merge");
    const uint32_t elseBB =
        hasElse ? theBuilder.createBasicBlock("if.false") : mergeBB;

    // Create the branch instruction. This will end the current basic block.
    theBuilder.createConditionalBranch(condition, thenBB, elseBB, mergeBB);
    theBuilder.addSuccessor(thenBB);
    theBuilder.addSuccessor(elseBB);
    // The current basic block has the OpSelectionMerge instruction. We need
    // to record its merge target.
    theBuilder.setMergeTarget(mergeBB);

    // Handle the then branch
    theBuilder.setInsertPoint(thenBB);
    doStmt(ifStmt->getThen());
    if (!theBuilder.isCurrentBasicBlockTerminated())
      theBuilder.createBranch(mergeBB);
    theBuilder.addSuccessor(mergeBB);

    // Handle the else branch (if exists)
    if (hasElse) {
      theBuilder.setInsertPoint(elseBB);
      doStmt(ifStmt->getElse());
      if (!theBuilder.isCurrentBasicBlockTerminated())
        theBuilder.createBranch(mergeBB);
      theBuilder.addSuccessor(mergeBB);
    }

    // From now on, we'll emit instructions into the merge block.
    theBuilder.setInsertPoint(mergeBB);
  }

  void doForStmt(const ForStmt *forStmt) {
    // for loops are composed of:
    //   for (<init>; <check>; <continue>) <body>
    //
    // To translate a for loop, we'll need to emit all <init> statements
    // in the current basic block, and then have separate basic blocks for
    // <check>, <continue>, and <body>. Besides, since SPIR-V requires
    // structured control flow, we need two more basic blocks, <header>
    // and <merge>. <header> is the block before control flow diverges,
    // while <merge> is the block where control flow subsequently converges.
    // The <check> block can take the responsibility of the <header> block.
    // The final CFG should normally be like the following. Exceptions will
    // occur with non-local exits like loop breaks or early returns.
    //             +--------+
    //             |  init  |
    //             +--------+
    //                 |
    //                 v
    //            +----------+
    //            |  header  | <---------------+
    //            | (check)  |                 |
    //            +----------+                 |
    //                 |                       |
    //         +-------+-------+               |
    //         | false         | true          |
    //         |               v               |
    //         |            +------+     +----------+
    //         |            | body | --> | continue |
    //         v            +------+     +----------+
    //     +-------+
    //     | merge |
    //     +-------+
    //
    // For more details, see "2.11. Structured Control Flow" in the SPIR-V spec.

    // Create basic blocks
    const uint32_t checkBB = theBuilder.createBasicBlock("for.check");
    const uint32_t bodyBB = theBuilder.createBasicBlock("for.body");
    const uint32_t continueBB = theBuilder.createBasicBlock("for.continue");
    const uint32_t mergeBB = theBuilder.createBasicBlock("for.merge");

    // Process the <init> block
    if (const Stmt *initStmt = forStmt->getInit()) {
      doStmt(initStmt);
    }
    theBuilder.createBranch(checkBB);
    theBuilder.addSuccessor(checkBB);

    // Process the <check> block
    theBuilder.setInsertPoint(checkBB);
    uint32_t condition;
    if (const Expr *check = forStmt->getCond()) {
      condition = doExpr(check);
    } else {
      condition = theBuilder.getConstantBool(true);
    }
    theBuilder.createConditionalBranch(condition, bodyBB,
                                       /*false branch*/ mergeBB,
                                       /*merge*/ mergeBB, continueBB);
    theBuilder.addSuccessor(bodyBB);
    theBuilder.addSuccessor(mergeBB);
    // The current basic block has OpLoopMerge instruction. We need to set its
    // continue and merge target.
    theBuilder.setContinueTarget(continueBB);
    theBuilder.setMergeTarget(mergeBB);

    // Process the <body> block
    theBuilder.setInsertPoint(bodyBB);
    if (const Stmt *body = forStmt->getBody()) {
      doStmt(body);
    }
    theBuilder.createBranch(continueBB);
    theBuilder.addSuccessor(continueBB);

    // Process the <continue> block
    theBuilder.setInsertPoint(continueBB);
    if (const Expr *cont = forStmt->getInc()) {
      doExpr(cont);
    }
    theBuilder.createBranch(checkBB); // <continue> should jump back to header
    theBuilder.addSuccessor(checkBB);

    // Set insertion point to the <merge> block for subsequent statements
    theBuilder.setInsertPoint(mergeBB);
  }

  uint32_t doExpr(const Expr *expr) {
    if (const auto *delRefExpr = dyn_cast<DeclRefExpr>(expr)) {
      // Returns the <result-id> of the referenced Decl.
      const NamedDecl *referredDecl = delRefExpr->getFoundDecl();
      assert(referredDecl && "found non-NamedDecl referenced");
      return declIdMapper.getDeclResultId(referredDecl);
    }

    if (const auto *parenExpr = dyn_cast<ParenExpr>(expr)) {
      // Just need to return what's inside the parentheses.
      return doExpr(parenExpr->getSubExpr());
    }

    if (const auto *memberExpr = dyn_cast<MemberExpr>(expr)) {
      const uint32_t base = doExpr(memberExpr->getBase());
      const auto *memberDecl = memberExpr->getMemberDecl();
      if (const auto *fieldDecl = dyn_cast<FieldDecl>(memberDecl)) {
        const auto index =
            theBuilder.getConstantInt32(fieldDecl->getFieldIndex());
        const uint32_t fieldType =
            typeTranslator.translateType(fieldDecl->getType());
        const uint32_t ptrType =
            theBuilder.getPointerType(fieldType, spv::StorageClass::Function);
        return theBuilder.createAccessChain(ptrType, base, {index});
      } else {
        emitError("Decl '%0' in MemberExpr is not supported yet.")
            << memberDecl->getDeclKindName();
        return 0;
      }
    }

    if (const auto *castExpr = dyn_cast<CastExpr>(expr)) {
      return doCastExpr(castExpr);
    }

    if (const auto *initListExpr = dyn_cast<InitListExpr>(expr)) {
      return doInitListExpr(initListExpr);
    }

    if (const auto *boolLiteral = dyn_cast<CXXBoolLiteralExpr>(expr)) {
      const bool value = boolLiteral->getValue();
      return theBuilder.getConstantBool(value);
    }

    if (const auto *intLiteral = dyn_cast<IntegerLiteral>(expr)) {
      return translateAPInt(intLiteral->getValue(), expr->getType());
    }

    if (const auto *floatLiteral = dyn_cast<FloatingLiteral>(expr)) {
      return translateAPFloat(floatLiteral->getValue(), expr->getType());
    }

    // CompoundAssignOperator is a subclass of BinaryOperator. It should be
    // checked before BinaryOperator.
    if (const auto *compoundAssignOp = dyn_cast<CompoundAssignOperator>(expr)) {
      return doCompoundAssignOperator(compoundAssignOp);
    }

    if (const auto *binOp = dyn_cast<BinaryOperator>(expr)) {
      return doBinaryOperator(binOp);
    }

    if (const auto *unaryOp = dyn_cast<UnaryOperator>(expr)) {
      return doUnaryOperator(unaryOp);
    }

    if (const auto *vecElemExpr = dyn_cast<HLSLVectorElementExpr>(expr)) {
      return doHLSLVectorElementExpr(vecElemExpr);
    }

    if (const auto *funcCall = dyn_cast<CallExpr>(expr)) {
      return doCallExpr(funcCall);
    }

    if (const auto *condExpr = dyn_cast<ConditionalOperator>(expr)) {
      return doConditionalOperator(condExpr);
    }

    emitError("Expr '%0' is not supported yet.") << expr->getStmtClassName();
    // TODO: handle other expressions
    return 0;
  }

  uint32_t doInitListExpr(const InitListExpr *expr) {
    // First try to evaluate the expression as constant expression
    Expr::EvalResult evalResult;
    if (expr->EvaluateAsRValue(evalResult, astContext) &&
        !evalResult.HasSideEffects) {
      return translateAPValue(evalResult.Val, expr->getType());
    }

    const QualType type = expr->getType();

    // InitListExpr is tricky to handle. It can have initializers of different
    // types, and each initializer can itself be of a composite type.
    // The front end parsing only gurantees the total number of elements in
    // the initializers are the same as the one of the InitListExpr's type.

    // For builtin types, we can assume the front end parsing has injected
    // the necessary ImplicitCastExpr for type casting. So we just need to
    // return the result of processing the only initializer.
    if (type->isBuiltinType()) {
      assert(expr->getNumInits() == 1);
      return doExpr(expr->getInit(0));
    }

    // For composite types, we need to type cast the initializers if necessary.

    const auto initCount = expr->getNumInits();
    const uint32_t resultType = typeTranslator.translateType(type);

    // For InitListExpr of vector type and having one initializer, we can avoid
    // composite extraction and construction.
    if (initCount == 1 && hlsl::IsHLSLVecType(type)) {
      const Expr *init = expr->getInit(0);

      // If the initializer already have the correct type, we don't need to
      // type cast.
      if (init->getType() == type) {
        return doExpr(init);
      }

      // For the rest, we can do type cast as a whole.

      const auto targetElemType = hlsl::GetHLSLVecElementType(type);
      if (targetElemType->isBooleanType()) {
        return castToBool(init, type);
      } else if (targetElemType->isIntegerType()) {
        return castToInt(init, type);
      } else if (targetElemType->isFloatingType()) {
        return castToFloat(init, type);
      } else {
        emitError("unimplemented vector InitList cases");
        expr->dump();
        return 0;
      }
    }

    // Cases needing composite extraction and construction

    std::vector<uint32_t> constituents;
    for (size_t i = 0; i < initCount; ++i) {
      const Expr *init = expr->getInit(i);
      if (!init->getType()->isBuiltinType()) {
        emitError("unimplemented InitList initializer type");
        init->dump();
        return 0;
      }
      constituents.push_back(doExpr(init));
    }

    return theBuilder.createCompositeConstruct(resultType, constituents);
  }

  /// Tries to emit instructions for assigning to the given vector element
  /// accessing expression. Returns 0 if the trial fails and no instructions
  /// are generated.
  ///
  /// This method handles the cases that we are writing to neither one element
  /// or all elements in their original order. For other cases, 0 will be
  /// returned and the normal assignment process should be used.
  uint32_t tryToAssignToVectorElements(const Expr *lhs, const uint32_t rhs) {
    // Assigning to a vector swizzling lhs is tricky if we are neither
    // writing to one element nor all elements in their original order.
    // Under such cases, we need to create a new vector swizzling involving
    // both the lhs and rhs vectors and then write the result of this swizzling
    // into the base vector of lhs.
    // For example, for vec4.yz = vec2, we nee to do the following:
    //
    //   %vec4Val = OpLoad %v4float %vec4
    //   %vec2Val = OpLoad %v2float %vec2
    //   %shuffle = OpVectorShuffle %v4float %vec4Val %vec2Val 0 4 5 3
    //   OpStore %vec4 %shuffle
    //
    // When doing the vector shuffle, we use the lhs base vector as the first
    // vector and the rhs vector as the second vector. Therefore, all elements
    // in the second vector will be selected into the shuffle result.

    const auto *lhsExpr = dyn_cast<HLSLVectorElementExpr>(lhs);

    if (!lhsExpr)
      return 0;

    if (!isVectorShuffle(lhs)) {
      // No vector shuffle needed to be generated for this assignment.
      // Should fall back to the normal handling of assignment.
      return 0;
    }

    const Expr *base = nullptr;
    hlsl::VectorMemberAccessPositions accessor;
    condenseVectorElementExpr(lhsExpr, &base, &accessor);

    const QualType baseType = base->getType();
    assert(hlsl::IsHLSLVecType(baseType));
    const auto baseSizse = hlsl::GetHLSLVecSize(baseType);

    llvm::SmallVector<uint32_t, 4> selectors;
    selectors.resize(baseSizse);
    // Assume we are selecting all original elements first.
    for (uint32_t i = 0; i < baseSizse; ++i) {
      selectors[i] = i;
    }

    // Now fix up the elements that actually got overwritten by the rhs vector.
    // Since we are using the rhs vector as the second vector, their index
    // should be offset'ed by the size of the lhs base vector.
    for (uint32_t i = 0; i < accessor.Count; ++i) {
      uint32_t position;
      accessor.GetPosition(i, &position);
      selectors[position] = baseSizse + i;
    }

    const uint32_t baseTypeId = typeTranslator.translateType(baseType);
    const uint32_t vec1 = doExpr(base);
    const uint32_t vec1Val = theBuilder.createLoad(baseTypeId, vec1);
    const uint32_t shuffle =
        theBuilder.createVectorShuffle(baseTypeId, vec1Val, rhs, selectors);

    theBuilder.createStore(vec1, shuffle);

    // TODO: OK, this return value is incorrect for compound assignments, for
    // which cases we should return lvalues. Should at least emit errors if
    // this return value is used (can be checked via ASTContext.getParents).
    return rhs;
  }

  /// Generates the necessary instructions for assigning rhs to lhs. If lhsPtr
  /// is not zero, it will be used as the pointer from lhs instead of evaluating
  /// lhs again.
  uint32_t processAssignment(const Expr *lhs, const uint32_t rhs,
                             bool isCompoundAssignment, uint32_t lhsPtr = 0) {
    // Assigning to vector swizzling should be handled differently.
    if (const uint32_t result = tryToAssignToVectorElements(lhs, rhs)) {
      return result;
    }

    // Normal assignment procedure
    if (lhsPtr == 0)
      lhsPtr = doExpr(lhs);

    theBuilder.createStore(lhsPtr, rhs);
    // Plain assignment returns a rvalue, while compound assignment returns
    // lvalue.
    return isCompoundAssignment ? lhsPtr : rhs;
  }

  /// Processes each vector within the given matrix by calling actOnEachVector.
  /// matrixVal should be the loaded value of the matrix. actOnEachVector takes
  /// three parameters for the current vector: the index, the <type-id>, and
  /// the value. It returns the <result-id> of the processed vector.
  uint32_t processEachVectorInMatrix(
      const Expr *matrix, const uint32_t matrixVal,
      llvm::function_ref<uint32_t(uint32_t, uint32_t, uint32_t)>
          actOnEachVector) {
    const auto matType = matrix->getType();
    assert(TypeTranslator::isSpirvAcceptableMatrixType(matType));
    const uint32_t vecType = typeTranslator.getComponentVectorType(matType);

    uint32_t rowCount = 0, colCount = 0;
    hlsl::GetHLSLMatRowColCount(matType, rowCount, colCount);

    llvm::SmallVector<uint32_t, 4> vectors;
    // Extract each component vector and do operation on it
    for (uint32_t i = 0; i < rowCount; ++i) {
      const uint32_t lhsVec =
          theBuilder.createCompositeExtract(vecType, matrixVal, {i});
      vectors.push_back(actOnEachVector(i, vecType, lhsVec));
    }

    // Construct the result matrix
    return theBuilder.createCompositeConstruct(
        typeTranslator.translateType(matType), vectors);
  }

  /// Generates the necessary instructions for conducting the given binary
  /// operation on lhs and rhs.
  ///
  /// This method expects that both lhs and rhs are SPIR-V acceptable matrices.
  uint32_t processMatrixBinaryOp(const Expr *lhs, const Expr *rhs,
                                 const BinaryOperatorKind opcode) {
    // TODO: some code are duplicated from processBinaryOp. Try to unify them.
    const auto lhsType = lhs->getType();
    assert(TypeTranslator::isSpirvAcceptableMatrixType(lhsType));
    const spv::Op spvOp = translateOp(opcode, lhsType);

    uint32_t rhsVal, lhsPtr, lhsVal;
    if (isCompoundAssignment(opcode)) {
      // Evalute rhs before lhs
      rhsVal = doExpr(rhs);
      lhsPtr = doExpr(lhs);
      const uint32_t lhsTy = typeTranslator.translateType(lhsType);
      lhsVal = theBuilder.createLoad(lhsTy, lhsPtr);
    } else {
      // Evalute lhs before rhs
      lhsVal = lhsPtr = doExpr(lhs);
      rhsVal = doExpr(rhs);
    }

    switch (opcode) {
    case BO_Add:
    case BO_Sub:
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign: {
      const uint32_t vecType = typeTranslator.getComponentVectorType(lhsType);
      const auto actOnEachVec = [this, spvOp, rhsVal](
          uint32_t index, uint32_t vecType, uint32_t lhsVec) {
        // For each vector of lhs, we need to load the corresponding vector of
        // rhs and do the operation on them.
        const uint32_t rhsVec =
            theBuilder.createCompositeExtract(vecType, rhsVal, {index});
        return theBuilder.createBinaryOp(spvOp, vecType, lhsVec, rhsVec);

      };
      return processEachVectorInMatrix(lhs, lhsVal, actOnEachVec);
    }
    case BO_Assign:
      llvm_unreachable("assignment should not be handled here");
    default:
      break;
    }

    emitError("BinaryOperator '%0' for matrices not supported yet")
        << BinaryOperator::getOpcodeStr(opcode);
    return 0;
  }

  /// Generates the necessary instructions for conducting the given binary
  /// operation on lhs and rhs. If lhsResultId is not nullptr, the evaluated
  /// pointer from lhs during the process will be written into it. If
  /// mandateGenOpcode is not spv::Op::Max, it will used as the SPIR-V opcode
  /// instead of deducing from Clang frontend opcode.
  uint32_t processBinaryOp(const Expr *lhs, const Expr *rhs,
                           const BinaryOperatorKind opcode,
                           const uint32_t resultType,
                           uint32_t *lhsResultId = nullptr,
                           const spv::Op mandateGenOpcode = spv::Op::Max) {

    if (TypeTranslator::isSpirvAcceptableMatrixType(lhs->getType())) {
      return processMatrixBinaryOp(lhs, rhs, opcode);
    }

    const spv::Op spvOp = (mandateGenOpcode == spv::Op::Max)
                              ? translateOp(opcode, lhs->getType())
                              : mandateGenOpcode;

    uint32_t rhsVal, lhsPtr, lhsVal;
    if (isCompoundAssignment(opcode)) {
      // Evalute rhs before lhs
      rhsVal = doExpr(rhs);
      lhsVal = lhsPtr = doExpr(lhs);
      // This is a compound assignment. We need to load the lhs value if lhs
      // does not generate a vector shuffle.
      if (!isVectorShuffle(lhs)) {
        const uint32_t lhsTy = typeTranslator.translateType(lhs->getType());
        lhsVal = theBuilder.createLoad(lhsTy, lhsPtr);
      }
    } else {
      // Evalute lhs before rhs
      lhsVal = lhsPtr = doExpr(lhs);
      rhsVal = doExpr(rhs);
    }

    if (lhsResultId)
      *lhsResultId = lhsPtr;

    switch (opcode) {
    case BO_Add:
    case BO_Sub:
    case BO_Mul:
    case BO_Div:
    case BO_Rem:
    case BO_LT:
    case BO_LE:
    case BO_GT:
    case BO_GE:
    case BO_EQ:
    case BO_NE:
    case BO_And:
    case BO_Or:
    case BO_Xor:
    case BO_Shl:
    case BO_Shr:
    case BO_LAnd:
    case BO_LOr:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
    case BO_AndAssign:
    case BO_OrAssign:
    case BO_XorAssign:
    case BO_ShlAssign:
    case BO_ShrAssign:
      return theBuilder.createBinaryOp(spvOp, resultType, lhsVal, rhsVal);
    case BO_Assign:
      llvm_unreachable("assignment should not be handled here");
    default:
      break;
    }

    emitError("BinaryOperator '%0' is not supported yet.")
        << BinaryOperator::getOpcodeStr(opcode);
    return 0;
  }

  uint32_t doBinaryOperator(const BinaryOperator *expr) {
    const auto opcode = expr->getOpcode();

    // Handle assignment first since we need to evaluate rhs before lhs.
    // For other binary operations, we need to evaluate lhs before rhs.
    if (opcode == BO_Assign)
      return processAssignment(expr->getLHS(), doExpr(expr->getRHS()), false);

    // Try to optimize floatN * float case
    if (opcode == BO_Mul) {
      if (const uint32_t result = tryToGenFloatVectorScale(expr))
        return result;
    }

    const uint32_t resultType = typeTranslator.translateType(expr->getType());
    return processBinaryOp(expr->getLHS(), expr->getRHS(), opcode, resultType);
  }

  uint32_t doCompoundAssignOperator(const CompoundAssignOperator *expr) {
    const auto opcode = expr->getOpcode();

    // Try to optimize floatN *= float case
    if (opcode == BO_MulAssign) {
      if (const uint32_t result = tryToGenFloatVectorScale(expr))
        return result;
    }

    const auto *rhs = expr->getRHS();
    const auto *lhs = expr->getLHS();

    uint32_t lhsPtr = 0;
    const uint32_t resultType = typeTranslator.translateType(expr->getType());
    const uint32_t result =
        processBinaryOp(lhs, rhs, opcode, resultType, &lhsPtr);
    return processAssignment(lhs, result, true, lhsPtr);
  }

  uint32_t doUnaryOperator(const UnaryOperator *expr) {
    const auto opcode = expr->getOpcode();
    const auto *subExpr = expr->getSubExpr();
    const auto subType = subExpr->getType();
    const auto subValue = doExpr(subExpr);
    const auto subTypeId = typeTranslator.translateType(subType);

    switch (opcode) {
    case UO_PreInc:
    case UO_PreDec:
    case UO_PostInc:
    case UO_PostDec: {
      const bool isPre = opcode == UO_PreInc || opcode == UO_PreDec;
      const bool isInc = opcode == UO_PreInc || opcode == UO_PostInc;

      const spv::Op spvOp = translateOp(isInc ? BO_Add : BO_Sub, subType);
      const uint32_t originValue = theBuilder.createLoad(subTypeId, subValue);
      const uint32_t one = hlsl::IsHLSLMatType(subType)
                               ? getMatElemValueOne(subType)
                               : getValueOne(subType);
      uint32_t incValue = 0;
      if (TypeTranslator::isSpirvAcceptableMatrixType(subType)) {
        // For matrices, we can only incremnt/decrement each vector of it.
        const auto actOnEachVec = [this, spvOp, one](
            uint32_t /*index*/, uint32_t vecType, uint32_t lhsVec) {
          return theBuilder.createBinaryOp(spvOp, vecType, lhsVec, one);
        };
        incValue =
            processEachVectorInMatrix(subExpr, originValue, actOnEachVec);
      } else {
        incValue =
            theBuilder.createBinaryOp(spvOp, subTypeId, originValue, one);
      }
      theBuilder.createStore(subValue, incValue);

      // Prefix increment/decrement operator returns a lvalue, while postfix
      // increment/decrement returns a rvalue.
      return isPre ? subValue : originValue;
    }
    case UO_Not:
      return theBuilder.createUnaryOp(spv::Op::OpNot, subTypeId, subValue);
    case UO_LNot:
      // Parsing will do the necessary casting to make sure we are applying the
      // ! operator on boolean values.
      return theBuilder.createUnaryOp(spv::Op::OpLogicalNot, subTypeId,
                                      subValue);
    case UO_Plus:
      // No need to do anything for the prefix + operator.
      return subValue;
    case UO_Minus: {
      // SPIR-V have two opcodes for negating values: OpSNegate and OpFNegate.
      const spv::Op spvOp = isFloatOrVecOfFloatType(subType)
                                ? spv::Op::OpFNegate
                                : spv::Op::OpSNegate;
      return theBuilder.createUnaryOp(spvOp, subTypeId, subValue);
    }
    default:
      break;
    }

    emitError("unary operator '%0' unimplemented yet")
        << expr->getOpcodeStr(opcode);
    expr->dump();
    return 0;
  }

  /// Processes the given expression and emits SPIR-V instructions. If the
  /// result is a GLValue, does an additional load.
  ///
  /// This method is useful for cases where ImplicitCastExpr (LValueToRValue) is
  /// missing when using an lvalue as rvalue in the AST, e.g., DeclRefExpr will
  /// not be wrapped in ImplicitCastExpr (LValueToRValue) when appearing in
  /// HLSLVectorElementExpr since the generated HLSLVectorElementExpr itself can
  /// be lvalue or rvalue.
  uint32_t loadIfGLValue(const Expr *expr) {
    const uint32_t result = doExpr(expr);
    if (expr->isGLValue()) {
      const uint32_t baseTyId = typeTranslator.translateType(expr->getType());
      return theBuilder.createLoad(baseTyId, result);
    }

    return result;
  }

  /// Condenses a sequence of HLSLVectorElementExpr starting from the given
  /// expr into one. Writes the original base into *basePtr and the condensed
  /// accessor into *flattenedAccessor.
  void condenseVectorElementExpr(
      const HLSLVectorElementExpr *expr, const Expr **basePtr,
      hlsl::VectorMemberAccessPositions *flattenedAccessor) {
    llvm::SmallVector<hlsl::VectorMemberAccessPositions, 2> accessors;
    accessors.push_back(expr->getEncodedElementAccess());

    // Recursively descending until we find the true base vector. In the
    // meanwhile, collecting accessors in the reverse order.
    *basePtr = expr->getBase();
    while (const auto *vecElemBase =
               dyn_cast<HLSLVectorElementExpr>(*basePtr)) {
      accessors.push_back(vecElemBase->getEncodedElementAccess());
      *basePtr = vecElemBase->getBase();
    }

    *flattenedAccessor = accessors.back();
    for (int32_t i = accessors.size() - 2; i >= 0; --i) {
      const auto &currentAccessor = accessors[i];

      // Apply the current level of accessor to the flattened accessor of all
      // previous levels of ones.
      hlsl::VectorMemberAccessPositions combinedAccessor;
      for (uint32_t j = 0; j < currentAccessor.Count; ++j) {
        uint32_t currentPosition = 0;
        currentAccessor.GetPosition(j, &currentPosition);
        uint32_t previousPosition = 0;
        flattenedAccessor->GetPosition(currentPosition, &previousPosition);
        combinedAccessor.SetPosition(j, previousPosition);
      }
      combinedAccessor.Count = currentAccessor.Count;
      combinedAccessor.IsValid =
          flattenedAccessor->IsValid && currentAccessor.IsValid;

      *flattenedAccessor = combinedAccessor;
    }
  }

  uint32_t doHLSLVectorElementExpr(const HLSLVectorElementExpr *expr) {
    const Expr *baseExpr = nullptr;
    hlsl::VectorMemberAccessPositions accessor;
    condenseVectorElementExpr(expr, &baseExpr, &accessor);

    const QualType baseType = baseExpr->getType();
    assert(hlsl::IsHLSLVecType(baseType));
    const auto baseSize = hlsl::GetHLSLVecSize(baseType);

    const uint32_t type = typeTranslator.translateType(expr->getType());
    const auto accessorSize = accessor.Count;

    // Depending on the number of elements selected, we emit different
    // instructions.
    // For vectors of size greater than 1, if we are only selecting one element,
    // typical access chain or composite extraction should be fine. But if we
    // are selecting more than one elements, we must resolve to vector specific
    // operations.
    // For size-1 vectors, if we are selecting their single elements multiple
    // times, we need composite construct instructions.

    if (accessorSize == 1) {
      if (baseSize == 1) {
        // Selecting one element from a size-1 vector. The underlying vector is
        // already treated as a scalar.
        return doExpr(baseExpr);
      }

      // If the base is an lvalue, we should emit an access chain instruction
      // so that we can load/store the specified element. For rvalue base,
      // we should use composite extraction. We should check the immediate base
      // instead of the original base here since we can have something like
      // v.xyyz to turn a lvalue v into rvalue.
      if (expr->getBase()->isGLValue()) { // E.g., v.x;
        // TODO: select the correct storage class
        const uint32_t ptrType =
            theBuilder.getPointerType(type, spv::StorageClass::Function);
        const uint32_t index = theBuilder.getConstantInt32(accessor.Swz0);
        // We need a lvalue here. Do not try to load.
        return theBuilder.createAccessChain(ptrType, doExpr(baseExpr), {index});
      } else { // E.g., (v + w).x;
        // The original base vector may not be a rvalue. Need to load it if
        // it is lvalue since ImplicitCastExpr (LValueToRValue) will be missing
        // for that case.
        return theBuilder.createCompositeExtract(type, loadIfGLValue(baseExpr),
                                                 {accessor.Swz0});
      }
    }

    if (baseSize == 1) {
      // Selecting one element from a size-1 vector. Construct the vector.
      llvm::SmallVector<uint32_t, 4> components(
          static_cast<size_t>(accessorSize), loadIfGLValue(baseExpr));
      return theBuilder.createCompositeConstruct(type, components);
    }

    llvm::SmallVector<uint32_t, 4> selectors;
    selectors.resize(accessorSize);
    // Whether we are selecting elements in the original order
    bool originalOrder = baseSize == accessorSize;
    for (uint32_t i = 0; i < accessorSize; ++i) {
      accessor.GetPosition(i, &selectors[i]);
      // We can select more elements than the vector provides. This handles
      // that case too.
      originalOrder &= selectors[i] == i;
    }

    if (originalOrder)
      return doExpr(baseExpr);

    const uint32_t baseVal = loadIfGLValue(baseExpr);
    // Use base for both vectors. But we are only selecting values from the
    // first one.
    return theBuilder.createVectorShuffle(type, baseVal, baseVal, selectors);
  }

  /// Returns true if the given expression will be translated into a vector
  /// shuffle instruction in SPIR-V.
  ///
  /// We emit a vector shuffle instruction iff
  /// * We are not selecting only one element from the vector (OpAccessChain
  ///   or OpCompositeExtract for such case);
  /// * We are not selecting all elements in their original order (essentially
  ///   the original vector, no shuffling needed).
  bool isVectorShuffle(const Expr *expr) {
    // TODO: the following check is essentially duplicated from
    // doHLSLVectorElementExpr. Should unify them.
    if (const auto *vecElemExpr = dyn_cast<HLSLVectorElementExpr>(expr)) {
      const Expr *base = nullptr;
      hlsl::VectorMemberAccessPositions accessor;
      condenseVectorElementExpr(vecElemExpr, &base, &accessor);

      const auto accessorSize = accessor.Count;
      if (accessorSize == 1) {
        // Selecting only one element. OpAccessChain or OpCompositeExtract for
        // such cases.
        return false;
      }

      const auto baseSize = hlsl::GetHLSLVecSize(base->getType());
      if (accessorSize != baseSize)
        return true;

      for (uint32_t i = 0; i < accessorSize; ++i) {
        uint32_t position;
        accessor.GetPosition(i, &position);
        if (position != i)
          return true;
      }

      // Selecting exactly the original vector. No vector shuffle generated.
      return false;
    }

    return false;
  }

  uint32_t doCastExpr(const CastExpr *expr) {
    const Expr *subExpr = expr->getSubExpr();
    const QualType toType = expr->getType();

    switch (expr->getCastKind()) {
    case CastKind::CK_LValueToRValue: {
      const uint32_t fromValue = doExpr(subExpr);
      if (isVectorShuffle(subExpr)) {
        // By reaching here, it means the vector element accessing operation is
        // an lvalue. If we generated a vector shuffle for it and trying to use
        // it as a rvalue, we cannot do the load here as normal. Need the upper
        // nodes in the AST tree to handle it properly.
        return fromValue;
      }

      // Using lvalue as rvalue means we need to OpLoad the contents from
      // the parameter/variable first.
      const uint32_t resultType = typeTranslator.translateType(toType);
      return theBuilder.createLoad(resultType, fromValue);
    }
    case CastKind::CK_NoOp:
      return doExpr(subExpr);
    case CastKind::CK_IntegralCast:
    case CastKind::CK_FloatingToIntegral:
    case CastKind::CK_HLSLCC_IntegralCast:
    case CastKind::CK_HLSLCC_FloatingToIntegral: {
      // Integer literals in the AST are represented using 64bit APInt
      // themselves and then implicitly casted into the expected bitwidth.
      // We need special treatment of integer literals here because generating
      // a 64bit constant and then explicit casting in SPIR-V requires Int64
      // capability. We should avoid introducing unnecessary capabilities to
      // our best.
      llvm::APSInt intValue;
      if (expr->EvaluateAsInt(intValue, astContext, Expr::SE_NoSideEffects)) {
        return translateAPInt(intValue, toType);
      }

      return castToInt(subExpr, toType);
    }
    case CastKind::CK_FloatingCast:
    case CastKind::CK_IntegralToFloating:
    case CastKind::CK_HLSLCC_FloatingCast:
    case CastKind::CK_HLSLCC_IntegralToFloating: {
      // First try to see if we can do constant folding for floating point
      // numbers like what we are doing for integers in the above.
      Expr::EvalResult evalResult;
      if (expr->EvaluateAsRValue(evalResult, astContext) &&
          !evalResult.HasSideEffects) {
        return translateAPFloat(evalResult.Val.getFloat(), toType);
      }

      return castToFloat(subExpr, toType);
    }
    case CastKind::CK_IntegralToBoolean:
    case CastKind::CK_FloatingToBoolean:
    case CastKind::CK_HLSLCC_IntegralToBoolean:
    case CastKind::CK_HLSLCC_FloatingToBoolean: {
      // First try to see if we can do constant folding.
      bool boolVal;
      if (!expr->HasSideEffects(astContext) &&
          expr->EvaluateAsBooleanCondition(boolVal, astContext)) {
        return theBuilder.getConstantBool(boolVal);
      }

      return castToBool(subExpr, toType);
    }
    case CastKind::CK_HLSLVectorSplat: {
      const size_t size = hlsl::GetHLSLVecSize(expr->getType());

      return createVectorSplat(subExpr, size);
    }
    case CastKind::CK_HLSLVectorTruncationCast: {
      const uint32_t toVecTypeId = typeTranslator.translateType(toType);
      const uint32_t elemTypeId =
          typeTranslator.translateType(hlsl::GetHLSLVecElementType(toType));
      const auto toSize = hlsl::GetHLSLVecSize(toType);

      const uint32_t composite = doExpr(subExpr);
      llvm::SmallVector<uint32_t, 4> elements;

      for (uint32_t i = 0; i < toSize; ++i) {
        elements.push_back(
            theBuilder.createCompositeExtract(elemTypeId, composite, {i}));
      }

      if (toSize == 1) {
        return elements.front();
      }

      return theBuilder.createCompositeConstruct(toVecTypeId, elements);
    }
    case CastKind::CK_HLSLVectorToScalarCast: {
      // The underlying should already be a vector of size 1.
      assert(hlsl::GetHLSLVecSize(subExpr->getType()) == 1);
      return doExpr(subExpr);
    }
    case CastKind::CK_HLSLVectorToMatrixCast: {
      // The target type should already be a 1xN matrix type.
      assert(TypeTranslator::is1xNMatrixType(toType));
      return doExpr(subExpr);
    }
    case CastKind::CK_HLSLMatrixSplat: {
      // From scalar to matrix
      uint32_t rowCount = 0, colCount = 0;
      hlsl::GetHLSLMatRowColCount(toType, rowCount, colCount);

      // Handle degenerated cases first
      if (rowCount == 1 && colCount == 1)
        return doExpr(subExpr);

      if (colCount == 1)
        return createVectorSplat(subExpr, rowCount);

      bool isConstVec = false;
      const uint32_t vecSplat =
          createVectorSplat(subExpr, colCount, &isConstVec);
      if (rowCount == 1)
        return vecSplat;

      const uint32_t matType = typeTranslator.translateType(toType);
      llvm::SmallVector<uint32_t, 4> vectors(size_t(rowCount), vecSplat);

      if (isConstVec) {
        return theBuilder.getConstantComposite(matType, vectors);
      } else {
        return theBuilder.createCompositeConstruct(matType, vectors);
      }
    }
    case CastKind::CK_HLSLMatrixToScalarCast: {
      // The underlying should already be a matrix of 1x1.
      assert(TypeTranslator::is1x1MatrixType(subExpr->getType()));
      return doExpr(subExpr);
    }
    case CastKind::CK_HLSLMatrixToVectorCast: {
      // The underlying should already be a matrix of 1xN.
      assert(TypeTranslator::is1xNMatrixType(subExpr->getType()));
      return doExpr(subExpr);
    }
    case CastKind::CK_FunctionToPointerDecay:
      // Just need to return the function id
      return doExpr(subExpr);
    default:
      emitError("ImplictCast Kind '%0' is not supported yet.")
          << expr->getCastKindName();
      expr->dump();
      return 0;
    }
  }

  uint32_t processIntrinsicDot(const CallExpr *callExpr) {
    const QualType returnType = callExpr->getType();
    const uint32_t returnTypeId =
        typeTranslator.translateType(callExpr->getType());

    // Get the function parameters. Expect 2 vectors as parameters.
    assert(callExpr->getNumArgs() == 2u);
    const Expr *arg0 = callExpr->getArg(0);
    const Expr *arg1 = callExpr->getArg(1);
    const uint32_t arg0Id = doExpr(arg0);
    const uint32_t arg1Id = doExpr(arg1);
    QualType arg0Type = arg0->getType();
    QualType arg1Type = arg1->getType();
    const size_t vec0Size = hlsl::GetHLSLVecSize(arg0Type);
    const size_t vec1Size = hlsl::GetHLSLVecSize(arg1Type);
    const QualType vec0ComponentType = hlsl::GetHLSLVecElementType(arg0Type);
    const QualType vec1ComponentType = hlsl::GetHLSLVecElementType(arg1Type);
    assert(returnType == vec1ComponentType);
    assert(vec0ComponentType == vec1ComponentType);
    assert(vec0Size == vec1Size);
    assert(vec0Size >= 1 && vec0Size <= 4);

    // According to HLSL reference, the dot function only works on integers
    // and floats.
    assert(returnType->isFloatingType() || returnType->isIntegerType());

    // Special case: dot product of two vectors, each of size 1. That is
    // basically the same as regular multiplication of 2 scalars.
    if (vec0Size == 1) {
      const spv::Op spvOp = translateOp(BO_Mul, arg0Type);
      return theBuilder.createBinaryOp(spvOp, returnTypeId, arg0Id, arg1Id);
    }

    // If the vectors are of type Float, we can use OpDot.
    if (returnType->isFloatingType()) {
      return theBuilder.createBinaryOp(spv::Op::OpDot, returnTypeId, arg0Id,
                                       arg1Id);
    }
    // Vector component type is Integer (signed or unsigned).
    // Create all instructions necessary to perform a dot product on
    // two integer vectors. SPIR-V OpDot does not support integer vectors.
    // Therefore, we use other SPIR-V instructions (addition and
    // multiplication).
    else {
      uint32_t result = 0;
      llvm::SmallVector<uint32_t, 4> multIds;
      const spv::Op multSpvOp = translateOp(BO_Mul, arg0Type);
      const spv::Op addSpvOp = translateOp(BO_Add, arg0Type);

      // Extract members from the two vectors and multiply them.
      for (unsigned int i = 0; i < vec0Size; ++i) {
        const uint32_t vec0member =
            theBuilder.createCompositeExtract(returnTypeId, arg0Id, {i});
        const uint32_t vec1member =
            theBuilder.createCompositeExtract(returnTypeId, arg1Id, {i});
        const uint32_t multId = theBuilder.createBinaryOp(
            multSpvOp, returnTypeId, vec0member, vec1member);
        multIds.push_back(multId);
      }
      // Add all the multiplications.
      result = multIds[0];
      for (unsigned int i = 1; i < vec0Size; ++i) {
        const uint32_t additionId = theBuilder.createBinaryOp(
            addSpvOp, returnTypeId, result, multIds[i]);
        result = additionId;
      }
      return result;
    }
  }

  /// Generates necessary SPIR-V instructions to create a vector splat out of
  /// the given scalarExpr. The generated vector will have the same element
  /// type as scalarExpr and of the given size. If resultIsConstant is not
  /// nullptr, writes true to it if the generated instruction is a constant.
  uint32_t createVectorSplat(const Expr *scalarExpr, uint32_t size,
                             bool *resultIsConstant = nullptr) {
    bool isConstVal = false;
    uint32_t scalarVal = 0;

    // Try to evaluate the element as constant first. If successful, then we
    // can generate constant instructions for this vector splat.
    Expr::EvalResult evalResult;
    if (scalarExpr->EvaluateAsRValue(evalResult, astContext) &&
        !evalResult.HasSideEffects) {
      isConstVal = true;
      scalarVal = translateAPValue(evalResult.Val, scalarExpr->getType());
    } else {
      scalarVal = doExpr(scalarExpr);
    }

    if (resultIsConstant)
      *resultIsConstant = isConstVal;

    // Just return the scalar value for vector splat with size 1
    if (size == 1)
      return scalarVal;

    const uint32_t vecType = theBuilder.getVecType(
        typeTranslator.translateType(scalarExpr->getType()), size);
    llvm::SmallVector<uint32_t, 4> elements(size_t(size), scalarVal);

    if (isConstVal) {
      return theBuilder.getConstantComposite(vecType, elements);
    } else {
      return theBuilder.createCompositeConstruct(vecType, elements);
    }
  }

  /// Processes the given expr, casts the result into the given bool (vector)
  /// type and returns the <result-id> of the casted value.
  uint32_t castToBool(const Expr *expr, QualType toBoolType) {
    // Converting to bool means comparing with value zero.

    const uint32_t fromVal = doExpr(expr);

    if (isBoolOrVecOfBoolType(expr->getType()))
      return fromVal;

    const spv::Op spvOp = translateOp(BO_NE, expr->getType());
    const uint32_t boolType = typeTranslator.translateType(toBoolType);
    const uint32_t zeroVal = getValueZero(expr->getType());

    return theBuilder.createBinaryOp(spvOp, boolType, fromVal, zeroVal);
  }

  /// Processes the given expr, casts the result into the given integer (vector)
  /// type and returns the <result-id> of the casted value.
  uint32_t castToInt(const Expr *expr, QualType toIntType) {
    const QualType fromType = expr->getType();
    const uint32_t intType = typeTranslator.translateType(toIntType);
    const uint32_t fromVal = doExpr(expr);

    if (isBoolOrVecOfBoolType(fromType)) {
      const uint32_t one = getValueOne(toIntType);
      const uint32_t zero = getValueZero(toIntType);
      return theBuilder.createSelect(intType, fromVal, one, zero);
    } else if (isSintOrVecOfSintType(fromType) ||
               isUintOrVecOfUintType(fromType)) {
      if (fromType == toIntType)
        return fromVal;
      // TODO: handle different bitwidths
      return theBuilder.createUnaryOp(spv::Op::OpBitcast, intType, fromVal);
    } else if (isFloatOrVecOfFloatType(fromType)) {
      if (isSintOrVecOfSintType(toIntType)) {
        return theBuilder.createUnaryOp(spv::Op::OpConvertFToS, intType,
                                        fromVal);
      } else if (isUintOrVecOfUintType(toIntType)) {
        return theBuilder.createUnaryOp(spv::Op::OpConvertFToU, intType,
                                        fromVal);
      } else {
        emitError("unimplemented casting to integer from floating point");
      }
    } else {
      emitError("unimplemented casting to integer");
    }

    expr->dump();
    return 0;
  }

  uint32_t processIntrinsicAllOrAny(const CallExpr *callExpr, spv::Op spvOp) {
    const uint32_t returnType =
        typeTranslator.translateType(callExpr->getType());

    // 'all' and 'any' take only 1 parameter.
    assert(callExpr->getNumArgs() == 1u);
    const Expr *arg = callExpr->getArg(0);
    const QualType argType = arg->getType();

    if (hlsl::IsHLSLMatType(argType)) {
      emitError("'all' and 'any' do not support matrix arguments yet.");
      return 0;
    }

    bool isSpirvAcceptableVecType =
        hlsl::IsHLSLVecType(argType) && hlsl::GetHLSLVecSize(argType) > 1;
    if (!isSpirvAcceptableVecType) {
      // For a scalar or vector of 1 scalar, we can simply cast to boolean.
      return castToBool(arg, callExpr->getType());
    } else {
      // First cast the vector to a vector of booleans, then use OpAll
      uint32_t boolVecId = castToBool(arg, callExpr->getType());
      return theBuilder.createUnaryOp(spvOp, returnType, boolVecId);
    }
  }

  /// Processes the 'asfloat', 'asint', and 'asuint' intrinsic functions.
  uint32_t processIntrinsicAsType(const CallExpr *callExpr) {
    const QualType returnType = callExpr->getType();
    const uint32_t returnTypeId = typeTranslator.translateType(returnType);
    assert(callExpr->getNumArgs() == 1u);
    const Expr *arg = callExpr->getArg(0);
    const QualType argType = arg->getType();

    // asfloat may take a float or a float vector or a float matrix as argument.
    // These cases would be a no-op.
    if (returnType.getCanonicalType() == argType.getCanonicalType())
      return doExpr(arg);

    if (hlsl::IsHLSLMatType(argType)) {
      emitError("'asfloat', 'asint', and 'asuint' do not support matrix "
                "arguments yet.");
      return 0;
    }

    return theBuilder.createUnaryOp(spv::Op::OpBitcast, returnTypeId,
                                    doExpr(arg));
  }

  uint32_t processIntrinsicCallExpr(const CallExpr *callExpr) {
    const FunctionDecl *callee = callExpr->getDirectCallee();
    assert(hlsl::IsIntrinsicOp(callee) &&
           "doIntrinsicCallExpr was called for a non-intrinsic function.");

    // Figure out which intrinsic function to translate.
    llvm::StringRef group;
    uint32_t opcode = static_cast<uint32_t>(hlsl::IntrinsicOp::Num_Intrinsics);
    hlsl::GetIntrinsicOp(callee, opcode, group);

    switch (static_cast<hlsl::IntrinsicOp>(opcode)) {
    case hlsl::IntrinsicOp::IOP_dot:
      return processIntrinsicDot(callExpr);
    case hlsl::IntrinsicOp::IOP_all:
      return processIntrinsicAllOrAny(callExpr, spv::Op::OpAll);
    case hlsl::IntrinsicOp::IOP_any:
      return processIntrinsicAllOrAny(callExpr, spv::Op::OpAny);
    case hlsl::IntrinsicOp::IOP_asfloat:
    case hlsl::IntrinsicOp::IOP_asint:
    case hlsl::IntrinsicOp::IOP_asuint:
      return processIntrinsicAsType(callExpr);
    default:
      break;
    }

    emitError("Intrinsic function '%0' not yet implemented.")
        << callee->getName();
    return 0;
  }

  uint32_t castToFloat(const Expr *expr, QualType toFloatType) {
    const QualType fromType = expr->getType();
    const uint32_t floatType = typeTranslator.translateType(toFloatType);
    const uint32_t fromVal = doExpr(expr);

    if (isBoolOrVecOfBoolType(fromType)) {
      const uint32_t one = getValueOne(toFloatType);
      const uint32_t zero = getValueZero(toFloatType);
      return theBuilder.createSelect(floatType, fromVal, one, zero);
    }

    if (isSintOrVecOfSintType(fromType)) {
      return theBuilder.createUnaryOp(spv::Op::OpConvertSToF, floatType,
                                      fromVal);
    }

    if (isUintOrVecOfUintType(fromType)) {
      return theBuilder.createUnaryOp(spv::Op::OpConvertUToF, floatType,
                                      fromVal);
    }

    if (isFloatOrVecOfFloatType(fromType)) {
      return fromVal;
    }

    emitError("unimplemented casting to floating point");
    expr->dump();
    return 0;
  }

  uint32_t doCallExpr(const CallExpr *callExpr) {
    const FunctionDecl *callee = callExpr->getDirectCallee();

    // Intrinsic functions such as 'dot' or 'mul'
    if (hlsl::IsIntrinsicOp(callee)) {
      return processIntrinsicCallExpr(callExpr);
    }

    if (callee) {
      const uint32_t returnType =
          typeTranslator.translateType(callExpr->getType());

      // Get or forward declare the function <result-id>
      const uint32_t funcId = declIdMapper.getOrRegisterDeclResultId(callee);

      // Evaluate parameters
      llvm::SmallVector<uint32_t, 4> params;
      for (const auto *arg : callExpr->arguments()) {
        // We need to create variables for holding the values to be used as
        // arguments. The variables themselves are of pointer types.
        const uint32_t ptrType = theBuilder.getPointerType(
            typeTranslator.translateType(arg->getType()),
            spv::StorageClass::Function);

        const uint32_t tempVarId = theBuilder.addFnVariable(ptrType);
        theBuilder.createStore(tempVarId, doExpr(arg));

        params.push_back(tempVarId);
      }

      // Push the callee into the work queue if it is not there.
      if (!workQueue.count(callee)) {
        workQueue.insert(callee);
      }

      return theBuilder.createFunctionCall(returnType, funcId, params);
    }

    emitError("calling non-function unimplemented");
    return 0;
  }

  uint32_t doConditionalOperator(const ConditionalOperator *expr) {
    // According to HLSL doc, all sides of the ?: expression are always
    // evaluated.
    const uint32_t type = typeTranslator.translateType(expr->getType());
    const uint32_t condition = doExpr(expr->getCond());
    const uint32_t trueBranch = doExpr(expr->getTrueExpr());
    const uint32_t falseBranch = doExpr(expr->getFalseExpr());

    return theBuilder.createSelect(type, condition, trueBranch, falseBranch);
  }

  /// Translates a floatN * float multiplication into SPIR-V instructions and
  /// returns the <result-id>. Returns 0 if the given binary operation is not
  /// floatN * float.
  uint32_t tryToGenFloatVectorScale(const BinaryOperator *expr) {
    const QualType type = expr->getType();
    // We can only translate floatN * float into OpVectorTimesScalar.
    // So the result type must be floatN.
    if (!hlsl::IsHLSLVecType(type) ||
        !hlsl::GetHLSLVecElementType(type)->isFloatingType())
      return 0;

    const Expr *lhs = expr->getLHS();
    const Expr *rhs = expr->getRHS();

    // Multiplying a float vector with a float scalar will be represented in
    // AST via a binary operation with two float vectors as operands; one of
    // the operand is from an implicit cast with kind CK_HLSLVectorSplat.

    // vector * scalar
    if (hlsl::IsHLSLVecType(lhs->getType())) {
      if (const auto *cast = dyn_cast<ImplicitCastExpr>(rhs)) {
        if (cast->getCastKind() == CK_HLSLVectorSplat) {
          const uint32_t vecType =
              typeTranslator.translateType(expr->getType());
          if (isa<CompoundAssignOperator>(expr)) {
            uint32_t lhsPtr = 0;
            const uint32_t result =
                processBinaryOp(lhs, cast->getSubExpr(), expr->getOpcode(),
                                vecType, &lhsPtr, spv::Op::OpVectorTimesScalar);
            return processAssignment(lhs, result, true, lhsPtr);
          } else {
            return processBinaryOp(lhs, cast->getSubExpr(), expr->getOpcode(),
                                   vecType, nullptr,
                                   spv::Op::OpVectorTimesScalar);
          }
        }
      }
    }

    // scalar * vector
    if (hlsl::IsHLSLVecType(rhs->getType())) {
      if (const auto *cast = dyn_cast<ImplicitCastExpr>(lhs)) {
        if (cast->getCastKind() == CK_HLSLVectorSplat) {
          const uint32_t vecType =
              typeTranslator.translateType(expr->getType());
          // We need to switch the positions of lhs and rhs here because
          // OpVectorTimesScalar requires the first operand to be a vector and
          // the second to be a scalar.
          return processBinaryOp(rhs, cast->getSubExpr(), expr->getOpcode(),
                                 vecType, nullptr,
                                 spv::Op::OpVectorTimesScalar);
        }
      }
    }

    return 0;
  }

  /// Translates the given frontend binary operator into its SPIR-V equivalent
  /// taking consideration of the operand type.
  spv::Op translateOp(BinaryOperator::Opcode op, QualType type) {
    const bool isSintType = isSintOrVecMatOfSintType(type);
    const bool isUintType = isUintOrVecMatOfUintType(type);
    const bool isFloatType = isFloatOrVecMatOfFloatType(type);

#define BIN_OP_CASE_INT_FLOAT(kind, intBinOp, floatBinOp)                      \
  \
case BO_##kind : {                                                             \
    if (isSintType || isUintType) {                                            \
      return spv::Op::Op##intBinOp;                                            \
    }                                                                          \
    if (isFloatType) {                                                         \
      return spv::Op::Op##floatBinOp;                                          \
    }                                                                          \
  }                                                                            \
  break

#define BIN_OP_CASE_SINT_UINT_FLOAT(kind, sintBinOp, uintBinOp, floatBinOp)    \
  \
case BO_##kind : {                                                             \
    if (isSintType) {                                                          \
      return spv::Op::Op##sintBinOp;                                           \
    }                                                                          \
    if (isUintType) {                                                          \
      return spv::Op::Op##uintBinOp;                                           \
    }                                                                          \
    if (isFloatType) {                                                         \
      return spv::Op::Op##floatBinOp;                                          \
    }                                                                          \
  }                                                                            \
  break

#define BIN_OP_CASE_SINT_UINT(kind, sintBinOp, uintBinOp)                      \
  \
case BO_##kind : {                                                             \
    if (isSintType) {                                                          \
      return spv::Op::Op##sintBinOp;                                           \
    }                                                                          \
    if (isUintType) {                                                          \
      return spv::Op::Op##uintBinOp;                                           \
    }                                                                          \
  }                                                                            \
  break

    switch (op) {
      BIN_OP_CASE_INT_FLOAT(Add, IAdd, FAdd);
      BIN_OP_CASE_INT_FLOAT(AddAssign, IAdd, FAdd);
      BIN_OP_CASE_INT_FLOAT(Sub, ISub, FSub);
      BIN_OP_CASE_INT_FLOAT(SubAssign, ISub, FSub);
      BIN_OP_CASE_INT_FLOAT(Mul, IMul, FMul);
      BIN_OP_CASE_INT_FLOAT(MulAssign, IMul, FMul);
      BIN_OP_CASE_SINT_UINT_FLOAT(Div, SDiv, UDiv, FDiv);
      BIN_OP_CASE_SINT_UINT_FLOAT(DivAssign, SDiv, UDiv, FDiv);
      // According to HLSL spec, "the modulus operator returns the remainder of
      // a division." "The % operator is defined only in cases where either both
      // sides are positive or both sides are negative."
      //
      // In SPIR-V, there are two reminder operations: Op*Rem and Op*Mod. With
      // the former, the sign of a non-0 result comes from Operand 1, while
      // with the latter, from Operand 2.
      //
      // For operands with different signs, technically we can map % to either
      // Op*Rem or Op*Mod since it's undefined behavior. But it is more
      // consistent with C (HLSL starts as a C derivative) and Clang frontend
      // const expression evaluation if we map % to Op*Rem.
      //
      // Note there is no OpURem in SPIR-V.
      BIN_OP_CASE_SINT_UINT_FLOAT(Rem, SRem, UMod, FRem);
      BIN_OP_CASE_SINT_UINT_FLOAT(RemAssign, SRem, UMod, FRem);
      BIN_OP_CASE_SINT_UINT_FLOAT(LT, SLessThan, ULessThan, FOrdLessThan);
      BIN_OP_CASE_SINT_UINT_FLOAT(LE, SLessThanEqual, ULessThanEqual,
                                  FOrdLessThanEqual);
      BIN_OP_CASE_SINT_UINT_FLOAT(GT, SGreaterThan, UGreaterThan,
                                  FOrdGreaterThan);
      BIN_OP_CASE_SINT_UINT_FLOAT(GE, SGreaterThanEqual, UGreaterThanEqual,
                                  FOrdGreaterThanEqual);
      BIN_OP_CASE_INT_FLOAT(EQ, IEqual, FOrdEqual);
      BIN_OP_CASE_INT_FLOAT(NE, INotEqual, FOrdNotEqual);
      BIN_OP_CASE_SINT_UINT(And, BitwiseAnd, BitwiseAnd);
      BIN_OP_CASE_SINT_UINT(AndAssign, BitwiseAnd, BitwiseAnd);
      BIN_OP_CASE_SINT_UINT(Or, BitwiseOr, BitwiseOr);
      BIN_OP_CASE_SINT_UINT(OrAssign, BitwiseOr, BitwiseOr);
      BIN_OP_CASE_SINT_UINT(Xor, BitwiseXor, BitwiseXor);
      BIN_OP_CASE_SINT_UINT(XorAssign, BitwiseXor, BitwiseXor);
      BIN_OP_CASE_SINT_UINT(Shl, ShiftLeftLogical, ShiftLeftLogical);
      BIN_OP_CASE_SINT_UINT(ShlAssign, ShiftLeftLogical, ShiftLeftLogical);
      BIN_OP_CASE_SINT_UINT(Shr, ShiftRightArithmetic, ShiftRightLogical);
      BIN_OP_CASE_SINT_UINT(ShrAssign, ShiftRightArithmetic, ShiftRightLogical);
    // According to HLSL doc, all sides of the && and || expression are always
    // evaluated.
    case BO_LAnd:
      return spv::Op::OpLogicalAnd;
    case BO_LOr:
      return spv::Op::OpLogicalOr;
    default:
      break;
    }

#undef BIN_OP_CASE_INT_FLOAT
#undef BIN_OP_CASE_SINT_UINT_FLOAT
#undef BIN_OP_CASE_SINT_UINT

    emitError("translating binary operator '%0' unimplemented")
        << BinaryOperator::getOpcodeStr(op);
    return spv::Op::OpNop;
  }

  /// Returns the <result-id> for a constant one vector of the given size and
  /// element type.
  uint32_t getVecValueOne(QualType elemType, uint32_t size) {
    const uint32_t elemOneId = getValueOne(elemType);

    if (size == 1)
      return elemOneId;

    llvm::SmallVector<uint32_t, 4> elements(size_t(size), elemOneId);
    const uint32_t vecType =
        theBuilder.getVecType(typeTranslator.translateType(elemType), size);

    return theBuilder.getConstantComposite(vecType, elements);
  }

  /// Returns the <result-id> for a constant one (vector) having the same
  /// element type as the given matrix type.
  ///
  /// If a 1x1 matrix is given, the returned value one will be a scalar;
  /// if a Mx1 or 1xN matrix is given, the returned value one will be a
  /// vector of size M or N; if a MxN matrix is given, the returned value
  /// one will be a vector of size N.
  uint32_t getMatElemValueOne(QualType type) {
    assert(hlsl::IsHLSLMatType(type));
    const auto elemType = hlsl::GetHLSLMatElementType(type);

    uint32_t rowCount = 0, colCount = 0;
    hlsl::GetHLSLMatRowColCount(type, rowCount, colCount);

    if (rowCount == 1 && colCount == 1)
      return getValueOne(elemType);
    if (colCount == 1)
      return getVecValueOne(elemType, rowCount);
    return getVecValueOne(elemType, colCount);
  }

  /// Returns the <result-id> for constant value 1 of the given type.
  uint32_t getValueOne(QualType type) {
    if (type->isSignedIntegerType()) {
      return theBuilder.getConstantInt32(1);
    }

    if (type->isUnsignedIntegerType()) {
      return theBuilder.getConstantUint32(1);
    }

    if (type->isFloatingType()) {
      return theBuilder.getConstantFloat32(1.0);
    }

    if (hlsl::IsHLSLVecType(type)) {
      const QualType elemType = hlsl::GetHLSLVecElementType(type);
      const auto size = hlsl::GetHLSLVecSize(type);
      return getVecValueOne(elemType, size);
    }

    emitError("getting value 1 for type '%0' unimplemented") << type;
    return 0;
  }

  /// Returns the <result-id> for constant value 0 of the given type.
  uint32_t getValueZero(QualType type) {
    if (type->isSignedIntegerType()) {
      return theBuilder.getConstantInt32(0);
    }

    if (type->isUnsignedIntegerType()) {
      return theBuilder.getConstantUint32(0);
    }

    if (type->isFloatingType()) {
      return theBuilder.getConstantFloat32(0.0);
    }

    if (hlsl::IsHLSLVecType(type)) {
      const QualType elemType = hlsl::GetHLSLVecElementType(type);
      const uint32_t elemZeroId = getValueZero(elemType);

      const size_t size = hlsl::GetHLSLVecSize(type);
      if (size == 1)
        return elemZeroId;

      llvm::SmallVector<uint32_t, 4> elements(size, elemZeroId);

      const uint32_t vecTypeId = typeTranslator.translateType(type);
      return theBuilder.getConstantComposite(vecTypeId, elements);
    }

    emitError("getting value 0 for type '%0' unimplemented")
        << type.getAsString();
    return 0;
  }

  /// Translates the given frontend APValue into its SPIR-V equivalent for the
  /// given targetType.
  uint32_t translateAPValue(const APValue &value, const QualType targetType) {
    if (targetType->isBooleanType()) {
      const bool boolValue = value.getInt().getBoolValue();
      return theBuilder.getConstantBool(boolValue);
    }

    if (targetType->isIntegerType()) {
      const llvm::APInt &intValue = value.getInt();
      return translateAPInt(intValue, targetType);
    }

    if (targetType->isFloatingType()) {
      const llvm::APFloat &floatValue = value.getFloat();
      return translateAPFloat(floatValue, targetType);
    }

    if (hlsl::IsHLSLVecType(targetType)) {
      const uint32_t vecType = typeTranslator.translateType(targetType);
      const QualType elemType = hlsl::GetHLSLVecElementType(targetType);

      const auto numElements = value.getVectorLength();
      // Special case for vectors of size 1. SPIR-V doesn't support this vector
      // size so we need to translate it to scalar values.
      if (numElements == 1) {
        return translateAPValue(value.getVectorElt(0), elemType);
      }

      llvm::SmallVector<uint32_t, 4> elements;
      for (uint32_t i = 0; i < numElements; ++i) {
        elements.push_back(translateAPValue(value.getVectorElt(i), elemType));
      }

      return theBuilder.getConstantComposite(vecType, elements);
    }

    emitError("APValue of type '%0' is not supported yet.") << value.getKind();
    value.dump();
    return 0;
  }

  /// Translates the given frontend APInt into its SPIR-V equivalent for the
  /// given targetType.
  uint32_t translateAPInt(const llvm::APInt &intValue, QualType targetType) {
    const auto bitwidth = astContext.getIntWidth(targetType);

    if (targetType->isSignedIntegerType()) {
      const int64_t value = intValue.getSExtValue();
      switch (bitwidth) {
      case 32:
        return theBuilder.getConstantInt32(static_cast<int32_t>(value));
      default:
        break;
      }
    } else {
      const uint64_t value = intValue.getZExtValue();
      switch (bitwidth) {
      case 32:
        return theBuilder.getConstantUint32(static_cast<uint32_t>(value));
      default:
        break;
      }
    }

    emitError("APInt for target bitwidth '%0' is not supported yet.")
        << bitwidth;
    return 0;
  }

  /// Translates the given frontend APFloat into its SPIR-V equivalent for the
  /// given targetType.
  uint32_t translateAPFloat(const llvm::APFloat &floatValue,
                            QualType targetType) {
    const auto &semantics = astContext.getFloatTypeSemantics(targetType);
    const auto bitwidth = llvm::APFloat::getSizeInBits(semantics);

    switch (bitwidth) {
    case 32:
      return theBuilder.getConstantFloat32(floatValue.convertToFloat());
    default:
      break;
    }

    emitError("APFloat for target bitwidth '%0' is not supported yet.")
        << bitwidth;
    return 0;
  }

private:
  /// \brief Wrapper method to create an error message and report it
  /// in the diagnostic engine associated with this consumer.
  template <unsigned N> DiagnosticBuilder emitError(const char (&message)[N]) {
    const auto diagId =
        diags.getCustomDiagID(clang::DiagnosticsEngine::Error, message);
    return diags.Report(diagId);
  }

  /// \brief Wrapper method to create a warning message and report it
  /// in the diagnostic engine associated with this consumer
  template <unsigned N>
  DiagnosticBuilder emitWarning(const char (&message)[N]) {
    const auto diagId =
        diags.getCustomDiagID(clang::DiagnosticsEngine::Warning, message);
    return diags.Report(diagId);
  }

private:
  CompilerInstance &theCompilerInstance;
  ASTContext &astContext;
  DiagnosticsEngine &diags;

  /// Entry function name and shader stage. Both of them are derived from the
  /// command line and should be const.
  const llvm::StringRef entryFunctionName;
  const spv::ExecutionModel shaderStage;

  SPIRVContext theContext;
  ModuleBuilder theBuilder;
  DeclResultIdMapper declIdMapper;
  TypeTranslator typeTranslator;

  /// A queue of decls reachable from the entry function. Decls inserted into
  /// this queue will persist to avoid duplicated translations. And we'd like
  /// a deterministic order of iterating the queue for finding the next decl
  /// to translate. So we need SetVector here.
  llvm::SetVector<const DeclaratorDecl *> workQueue;
  /// <result-id> for the entry function. Initially it is zero and will be reset
  /// when starting to translate the entry function.
  uint32_t entryFunctionId;
  /// The current function under traversal.
  const FunctionDecl *curFunction;

  /// For loops, while loops, and switch statements may encounter "break"
  /// statements that alter their control flow. At any point the break statement
  /// is observed, the control flow jumps to the inner-most scope's merge block.
  /// For instance: the break in the following example should cause a branch to
  /// the SwitchMergeBB, not ForLoopMergeBB:
  /// for (...) {
  ///   switch(...) {
  ///     case 1: break;
  ///   }
  ///   <--- SwitchMergeBB ---->
  /// }
  /// <----- ForLoopMergeBB --->
  /// This stack keeps track of the basic blocks to which branching could occur.
  std::stack<uint32_t> breakStack;

  /// Maps a given statement to the basic block that is associated with it.
  llvm::DenseMap<const Stmt *, uint32_t> stmtBasicBlock;
};

} // end namespace spirv

std::unique_ptr<ASTConsumer>
EmitSPIRVAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  return llvm::make_unique<spirv::SPIRVEmitter>(CI);
}
} // end namespace clang
