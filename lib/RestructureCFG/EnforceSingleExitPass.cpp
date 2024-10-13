//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/GraphWriter.h"

#include "revng/Support/IRHelpers.h"

#include "revng-c/RestructureCFG/EnforceSingleExitPass.h"

using namespace llvm;

// Implementation class used to run the `IDB` transformation
class EnforceSingleExitPassImpl {
  llvm::Function &F;

public:
  EnforceSingleExitPassImpl(llvm::Function &F) : F(F) {}

public:
  void run() {
    // Get the PostDominatorTree
    auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();

    // Get the LoopInfo
    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

    // Step 1: Check if we have a concrete or virtual root in the PDT
    DomTreeNode *Root = PDT.getRootNode();
    if (!Root) {
      // No root node, skip transformation
      return false;
    }

    bool hasMultipleExits = Root->getBlock() == nullptr;

    // If there's a concrete root (one exit node), no need to transform
    if (!hasMultipleExits) {
      return false;
    }

    // Step 2: Find all exit nodes (nodes without successors)
    std::vector<BasicBlock *> ExitNodes;
    for (auto &BB : F) {
      if (succ_empty(&BB)) {
        ExitNodes.push_back(&BB);
      }
    }

    // Step 3: Identify loops without exits
    for (Loop *L : LI) {
      BasicBlock *Header = L->getHeader();
      bool HasExit = false;

      // Check if the loop has any exit blocks
      for (BasicBlock *BB : L->blocks()) {
        if (!succ_empty(BB)) {
          for (BasicBlock *Succ : successors(BB)) {
            if (!L->contains(Succ)) {
              HasExit = true; // Found an exit outside the loop
              break;
            }
          }
        }
        if (HasExit)
          break;
      }

      // If the loop has no exit, treat its header as an exit node
      if (!HasExit) {
        ExitNodes.push_back(Header);
      }
    }

    // If no exit nodes are found, or there's only one exit, skip the
    // transformation
    if (ExitNodes.size() <= 1) {
      return false;
    }

    // Step 4: Create a new entry block
    BasicBlock *OriginalEntry = &F.getEntryBlock();
    IRBuilder<> Builder(F.getContext());

    BasicBlock *NewEntry = BasicBlock::Create(F.getContext(),
                                              "new_entry",
                                              &F,
                                              OriginalEntry);
    Builder.SetInsertPoint(NewEntry);
    Builder.CreateBr(OriginalEntry); // Branch from the new entry to the
                                     // original entry block

    // Step 5: Create the new sink block
    BasicBlock *SinkBB = BasicBlock::Create(F.getContext(), "sink_block", &F);
    Builder.SetInsertPoint(SinkBB);

    // Create the conditional branch: back to the original entry or sink
    // (infinite loop)
    Value *Condition = Builder
                         .CreateICmpEQ(ConstantInt::getTrue(F.getContext()),
                                       ConstantInt::getTrue(F.getContext()));
    Builder.CreateCondBr(Condition, OriginalEntry, SinkBB);

    // Add UnreachableInst to the sink block
    Builder.SetInsertPoint(SinkBB);
    Builder.CreateUnreachable();

    // Step 6: Redirect all exit nodes to the new sink block
    for (BasicBlock *ExitNode : ExitNodes) {
      Instruction *Term = ExitNode->getTerminator();
      Builder.SetInsertPoint(Term);
      Term->eraseFromParent(); // Remove the old terminator
      Builder.CreateBr(SinkBB); // Replace with a branch to the sink block
    }

    return true; // The function was modified
  }
};

char EnforceSingleExitPass::ID = 0;

static constexpr const char *Flag = "inline-divergent-branches";
using Reg = llvm::RegisterPass<EnforceSingleExitPass>;
static Reg
  X(Flag, "Perform the inline of divergent branches canonicalization process");

bool EnforceSingleExitPass::runOnFunction(llvm::Function &F) {

  // Instantiate and call the `Impl` class
  EnforceSingleExitPassImpl ESEImpl(F);
  ESEImpl.run();

  // This is a pass which can transform the CFG by inserting blocks and
  // redirecting edges, and therefore does not preserve the CFG
  return true;
}

void EnforceSingleExitPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {

  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
}
