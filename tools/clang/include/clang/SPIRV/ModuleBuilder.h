//===-- ModuleBuilder.h - SPIR-V builder ----------------------*- C++ -*---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_SPIRV_MODULEBUILDER_H
#define LLVM_CLANG_SPIRV_MODULEBUILDER_H

#include <memory>

#include "clang/SPIRV/InstBuilder.h"
#include "clang/SPIRV/SPIRVContext.h"
#include "clang/SPIRV/Structure.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"

namespace clang {
namespace spirv {

/// \brief SPIR-V module builder.
///
/// This class exports API for constructing SPIR-V binary interactively.
/// At any time, there can only exist at most one function under building;
/// but there can exist multiple basic blocks under construction.
///
/// Call `takeModule()` to get the SPIR-V words after finishing building the
/// module.
class ModuleBuilder {
public:
  /// \brief Constructs a ModuleBuilder with the given SPIR-V context.
  explicit ModuleBuilder(SPIRVContext *);

  /// \brief Returns the associated SPIRVContext.
  inline SPIRVContext *getSPIRVContext();

  /// \brief Takes the SPIR-V module under building. This will consume the
  /// module under construction.
  std::vector<uint32_t> takeModule();

  // === Function and Basic Block ===

  /// \brief Begins building a SPIR-V function. Returns the <result-id> for the
  /// function on success. Returns zero on failure.
  ///
  /// If the resultId supplied is not zero, the created function will use it;
  /// otherwise, an unused <result-id> will be assgined.
  /// At any time, there can only exist at most one function under building.
  uint32_t beginFunction(uint32_t funcType, uint32_t returnType,
                         llvm::StringRef name = "", uint32_t resultId = 0);

  /// \brief Registers a function parameter of the given pointer type in the
  /// current function and returns its <result-id>.
  uint32_t addFnParameter(uint32_t ptrType, llvm::StringRef name = "");

  /// \brief Creates a local variable of the given pointer type in the current
  /// function and returns its <result-id>.
  uint32_t addFnVariable(uint32_t ptrType, llvm::StringRef name = "",
                         llvm::Optional<uint32_t> init = llvm::None);

  /// \brief Ends building of the current function. Returns true of success,
  /// false on failure. All basic blocks constructed from the beginning or
  /// after ending the previous function will be collected into this function.
  bool endFunction();

  /// \brief Creates a SPIR-V basic block. On success, returns the <label-id>
  /// for the basic block. On failure, returns zero.
  uint32_t createBasicBlock(llvm::StringRef name = "");

  /// \brief Adds the basic block with the given label as a successor to the
  /// current basic block.
  void addSuccessor(uint32_t successorLabel);

  /// \brief Sets the merge target to the basic block with the given <label-id>.
  /// The caller must make sure the current basic block contains an
  /// OpSelectionMerge or OpLoopMerge instruction.
  void setMergeTarget(uint32_t mergeLabel);

  /// \brief Sets the continue target to the basic block with the given
  /// <label-id>. The caller must make sure the current basic block contains an
  /// OpLoopMerge instruction.
  void setContinueTarget(uint32_t continueLabel);

  /// \brief Returns true if the current basic block inserting into is
  /// terminated.
  inline bool isCurrentBasicBlockTerminated() const;

  /// \brief Sets insertion point to the basic block with the given <label-id>.
  void setInsertPoint(uint32_t labelId);

  // === Instruction at the current Insertion Point ===

  /// \brief Creates a composite construct instruction with the given
  /// <result-type> and constituents and returns the <result-id> for the
  /// composite.
  uint32_t createCompositeConstruct(uint32_t resultType,
                                    llvm::ArrayRef<uint32_t> constituents);

  /// \brief Creates a composite extract instruction. The given composite is
  /// indexed using the given literal indexes to obtain the resulting element.
  /// Returns the <result-id> for the extracted element.
  uint32_t createCompositeExtract(uint32_t resultType, uint32_t composite,
                                  llvm::ArrayRef<uint32_t> indexes);

  /// \brief Creates a vector shuffle instruction of selecting from the two
  /// vectors using selectors and returns the <result-id> of the result vector.
  uint32_t createVectorShuffle(uint32_t resultType, uint32_t vector1,
                               uint32_t vector2,
                               llvm::ArrayRef<uint32_t> selectors);

  /// \brief Creates a load instruction loading the value of the given
  /// <result-type> from the given pointer. Returns the <result-id> for the
  /// loaded value.
  uint32_t createLoad(uint32_t resultType, uint32_t pointer);
  /// \brief Creates a store instruction storing the given value into the given
  /// address.
  void createStore(uint32_t address, uint32_t value);

  /// \brief Creates a function call instruction and returns the <result-id> for
  /// the return value.
  uint32_t createFunctionCall(uint32_t returnType, uint32_t functionId,
                              llvm::ArrayRef<uint32_t> params);

  /// \brief Creates an access chain instruction to retrieve the element from
  /// the given base by walking through the given indexes. Returns the
  /// <result-id> for the pointer to the element.
  uint32_t createAccessChain(uint32_t resultType, uint32_t base,
                             llvm::ArrayRef<uint32_t> indexes);

  /// \brief Creates a unary operation with the given SPIR-V opcode. Returns
  /// the <result-id> for the result.
  uint32_t createUnaryOp(spv::Op op, uint32_t resultType, uint32_t operand);

  /// \brief Creates a binary operation with the given SPIR-V opcode. Returns
  /// the <result-id> for the result.
  uint32_t createBinaryOp(spv::Op op, uint32_t resultType, uint32_t lhs,
                          uint32_t rhs);

  /// \brief Creates a select operation with the given values for true and false
  /// cases and returns the <result-id> for the result.
  uint32_t createSelect(uint32_t resultType, uint32_t condition,
                        uint32_t trueValue, uint32_t falseValue);

  // \brief Creates an unconditional branch to the given target label.
  void createBranch(uint32_t targetLabel);

  // \brief Creates a conditional branch. An OpSelectionMerge instruction
  // will be created if mergeLabel is not 0 and continueLabel is 0.
  // An OpLoopMerge instruction will also be created if both continueLabel
  // and mergeLabel are not 0. For other cases, mergeLabel and continueLabel
  // will be ignored.
  void createConditionalBranch(uint32_t condition, uint32_t trueLabel,
                               uint32_t falseLabel, uint32_t mergeLabel = 0,
                               uint32_t continueLabel = 0);

  /// \brief Creates a return instruction.
  void createReturn();
  /// \brief Creates a return value instruction.
  void createReturnValue(uint32_t value);

  // === SPIR-V Module Structure ===

  inline void requireCapability(spv::Capability);

  inline void setAddressingModel(spv::AddressingModel);
  inline void setMemoryModel(spv::MemoryModel);

  /// \brief Adds an entry point for the module under construction. We only
  /// support a single entry point per module for now.
  inline void addEntryPoint(spv::ExecutionModel em, uint32_t targetId,
                            std::string targetName,
                            llvm::ArrayRef<uint32_t> interfaces);

  /// \brief Adds an execution mode to the module under construction.
  void addExecutionMode(uint32_t entryPointId, spv::ExecutionMode em,
                        const std::vector<uint32_t> &params);

  /// \brief Adds a stage input/ouput variable whose value is of the given type.
  ///
  /// The corresponding pointer type of the given type will be constructed in
  /// this method for the variable itself.
  uint32_t addStageIOVariable(uint32_t type, spv::StorageClass storageClass);

  /// \brief Adds a stage builtin variable whose value is of the given type.
  ///
  /// The corresponding pointer type of the given type will be constructed in
  /// this method for the variable itself.
  uint32_t addStageBuiltinVariable(uint32_t type, spv::BuiltIn);

  /// \brief Decorates the given target <result-id> with the given location.
  void decorateLocation(uint32_t targetId, uint32_t location);

  // === Type ===

  uint32_t getVoidType();
  uint32_t getBoolType();
  uint32_t getInt32Type();
  uint32_t getUint32Type();
  uint32_t getFloat32Type();
  uint32_t getVecType(uint32_t elemType, uint32_t elemCount);
  uint32_t getPointerType(uint32_t pointeeType, spv::StorageClass);
  uint32_t getStructType(llvm::ArrayRef<uint32_t> fieldTypes);
  uint32_t getFunctionType(uint32_t returnType,
                           llvm::ArrayRef<uint32_t> paramTypes);

  // === Constant ===
  uint32_t getConstantBool(bool value);
  uint32_t getConstantInt32(int32_t value);
  uint32_t getConstantUint32(uint32_t value);
  uint32_t getConstantFloat32(float value);
  uint32_t getConstantComposite(uint32_t typeId,
                                llvm::ArrayRef<uint32_t> constituents);

private:
  /// \brief Map from basic blocks' <label-id> to their structured
  /// representation.
  ///
  /// We need MapVector here to remember the order of insertion. Order matters
  /// here since, for example, we'll know for sure the first basic block is the
  /// entry block.
  using OrderedBasicBlockMap =
      llvm::MapVector<uint32_t, std::unique_ptr<BasicBlock>>;

  /// \brief Returns the basic block with the given <label-id>.
  BasicBlock *getBasicBlock(uint32_t label);

  SPIRVContext &theContext; ///< The SPIR-V context.
  SPIRVModule theModule;    ///< The module under building.

  std::unique_ptr<Function> theFunction; ///< The function under building.
  OrderedBasicBlockMap basicBlocks;      ///< The basic blocks under building.
  BasicBlock *insertPoint;               ///< The current insertion point.

  /// An InstBuilder associated with the current ModuleBuilder.
  /// It can be used to contruct instructions on the fly.
  /// The constructed instruction will appear in constructSite.
  InstBuilder instBuilder;
  std::vector<uint32_t> constructSite; ///< InstBuilder construction site.
};

SPIRVContext *ModuleBuilder::getSPIRVContext() { return &theContext; }

bool ModuleBuilder::isCurrentBasicBlockTerminated() const {
  return insertPoint && insertPoint->isTerminated();
}

void ModuleBuilder::setAddressingModel(spv::AddressingModel am) {
  theModule.setAddressingModel(am);
}

void ModuleBuilder::setMemoryModel(spv::MemoryModel mm) {
  theModule.setMemoryModel(mm);
}

void ModuleBuilder::requireCapability(spv::Capability cap) {
  theModule.addCapability(cap);
}

void ModuleBuilder::addEntryPoint(spv::ExecutionModel em, uint32_t targetId,
                                  std::string targetName,
                                  llvm::ArrayRef<uint32_t> interfaces) {
  theModule.addEntryPoint(em, targetId, std::move(targetName), interfaces);
}

} // end namespace spirv
} // end namespace clang

#endif
