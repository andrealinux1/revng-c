#pragma once

//
// Copyright (c) rev.ng Labs Srl. See LICENSE.md for details.
//

#include "revng/Support/TaggedFunctionPass.h"

struct PromoteStackPointerPass : public TaggedFunctionPass {
public:
  static char ID;

  PromoteStackPointerPass() :
    TaggedFunctionPass(ID,
                       &FunctionTags::Isolated,
                       &FunctionTags::StackPointerPromoted) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  bool runOnFunction(llvm::Function &F) override;
};
