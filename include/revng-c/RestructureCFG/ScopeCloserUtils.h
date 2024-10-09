#pragma once

//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/GraphTraits.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"

#include "revng/Support/IRHelpers.h"

// TODO: implement the builder class for the markers used to represent scope
//       closer edges

// We keep a globale pointer for the `ScopeCloserMarkerFunction` so we can
// dynamically add it in the module if a version is not already present
llvm::Function *ScopeCloserMarkerFunction = nullptr;

/// This is a helper class used to create the markers in the basic blocks needed
/// to handle the dashed edges. Its role should be similar somewhat to the
/// `llvm::IRBuilder` role.
class ScopeCloserMarkerBuilder {
private:
  llvm::BasicBlock *BB;

  // TODO: it would be nice if the pointer to the `ScopeCloserMarkerFunction`
  //       would stored locally in this builder, or retrieved through the
  //       `FunctionTag` machinery provided by `revng`, instead of having a
  //       global object like now
  // llvm::Function *ScopeCloserMarkerFunction;

public:
  ScopeCloserMarkerBuilder(llvm::Function *F) {

    if (ScopeCloserMarkerFunction) {
      return;
    }

    // Create the `ScopeCloser` marker function definition, that will be needed
    // by the inserted markers
    llvm::LLVMContext &C = getContext(F);
    llvm::Module *M = getModule(F);
    llvm::Type *BlockAddressTy = llvm::Type::getInt8PtrTy(C);
    auto *FT = llvm::FunctionType::get(llvm::Type::getVoidTy(C),
                                       { BlockAddressTy },
                                       false);

    using llvm::Function;
    using llvm::GlobalValue;
    ScopeCloserMarkerFunction = Function::Create(FT,
                                                 GlobalValue::ExternalLinkage,
                                                 "scope_closer",
                                                 M);
  }

public:
  // Set the insertion point of the `SCMBuilder`
  void setInsertPoint(llvm::BasicBlock *NewBB) { BB = NewBB; }

  // Retrieve the `ScopeCloser` BasicBlock target
  llvm::BasicBlock *getScopeCloserTarget(llvm::BasicBlock *BB) {

    // We must have an insertion point
    revng_assert(BB);

    // Find the last but one instruction, where the marker containing the target
    // `BasicBlockAddress` is stored
    auto BBIt = BB->rbegin();
    ++BBIt;

    if (BBIt != BB->rend()) {
      llvm::Instruction &SecondLastInst = *BBIt;
      if (auto *MarkerCall = llvm::dyn_cast<llvm::CallInst>(&SecondLastInst)) {
        if (MarkerCall->getCalledFunction() == ScopeCloserMarkerFunction) {
          llvm::BlockAddress *ScopeCloserBlockAddress = llvm::cast<
            llvm::BlockAddress>(MarkerCall->getArgOperand(0));
          using llvm::BasicBlock;
          BasicBlock *ScopeCloserBB = ScopeCloserBlockAddress->getBasicBlock();

          return ScopeCloserBB;
        }
      }
    }

    return nullptr;
  }

  void insertScopeCloserTarget(llvm::BasicBlock *BasicBlockTarget) {

    // We must have an insertion point
    revng_assert(BB);

    // Find the terminator instruction in the `BasicBlock` where we want to
    // store the `ScopeCloser` marker
    llvm::IRBuilder<> Builder(BB);
    llvm::Instruction *Terminator = BB->getTerminator();
    Builder.SetInsertPoint(Terminator);
    auto *BasicBlockAddressTarget = llvm::BlockAddress::get(BasicBlockTarget);
    revng_assert(BasicBlockAddressTarget);
    Builder.CreateCall(ScopeCloserMarkerFunction, BasicBlockAddressTarget);
  }
};
