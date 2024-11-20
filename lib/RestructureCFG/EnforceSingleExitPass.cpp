//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/GraphWriter.h"

#include "revng/Support/IRHelpers.h"

#include "revng-c/RestructureCFG/EnforceSingleExitPass.h"

using namespace llvm;

// Implementation class used to run the `ESE` transformation
class EnforceSingleExitPassImpl {
  Function &F;

  // We use the `PostDominatorTree` to count the exit nodes present in the CFG
  PostDominatorTree &PDT;

  // We use the `LoopInfo` analysis to find loops without an exit node
  LoopInfo &LI;

public:
  EnforceSingleExitPassImpl(Function &F, PostDominatorTree &PDT, LoopInfo &LI) :
    F(F), PDT(PDT), LI(LI) {}

public:
  bool run() {

    // 1: Get the concrete root node in the `PostDominatorTree`,
    DomTreeNode *PDTRoot = PDT.getRootNode();
    revng_assert(PDTRoot);

    // If the `PDTRoot` does not correspond to a concrete `BasicBlock`, it means
    // that we have a virtual root, so whether multiple exit nodes or loops
    // without exits
    bool hasMultipleExits = PDTRoot->getBlock() == nullptr;

    // It there is a concrete `BasicBlock`, we have a single exit node, so we do
    // not need to transform the graph
    if (!hasMultipleExits) {
      return false;
    }

    // 2: Find all the exit nodes (identified as `BasicBlock`s without
    //    successors)
    SmallVector<BasicBlock *> ExitBlocks;
    for (BasicBlock &BB : F) {
      if (succ_empty(&BB)) {
        ExitBlocks.push_back(&BB);
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

              // We found an exit from the loop
              HasExit = true;
              break;
            }
          }
        }
        if (HasExit)
          break;
      }

      // If the loop has no exit, treat its header as an exit node
      if (!HasExit) {
        ExitBlocks.push_back(Header);
      }
    }

    // If we collected zero or one candidate exit `BasicBlock`s, we do not need
    // to perform any transformation to the `Module`
    if (ExitBlocks.size() <= 1) {
      return false;
    }

    // 4: Create the new sink block
    LLVMContext &Context = getContext(&F);
    BasicBlock *SinkBlock = BasicBlock::Create(Context, "sink_block", &F);

    // Add an `UnreachableInst` to end the `SinkBlock`
    IRBuilder<> Builder(Context);
    Builder.SetInsertPoint(SinkBlock);
    Builder.CreateUnreachable();

    // 5: If more than one candidate exit `BasicBlock` is present, we need to:
    //    create a new entry block, ending with a conditional branch (using the
    //    `true` constant as condition), whose `true` branch goes to the
    //    original entry `BasicBlock`, and whose `false` branch goes to newly
    //    created sink node
    BasicBlock *OriginalEntry = &F.getEntryBlock();
    BasicBlock *NewEntryBlock = BasicBlock::Create(Context,
                                                   "new_entry_block",
                                                   &F,
                                                   OriginalEntry);
    Builder.SetInsertPoint(NewEntryBlock);
    Value *ConditionTrue = Builder.getTrue();
    Builder.CreateCondBr(ConditionTrue, OriginalEntry, SinkBlock);

    // 6: Connect all the exit nodes to the newly create sink block
    for (BasicBlock *ExitBlock : ExitBlocks) {
      Instruction *Terminator = ExitBlock->getTerminator();

      // TODO: what do with the previous terminator instruction?
      Terminator->eraseFromParent();

      Builder.SetInsertPoint(ExitBlock);
      Builder.CreateBr(SinkBlock);
    }

    // The function was modified
    return true;
  }
};

char EnforceSingleExitPass::ID = 0;

static constexpr const char *Flag = "ese";
using Reg = llvm::RegisterPass<EnforceSingleExitPass>;
static Reg X(Flag, "Enforce the Single Exit Property on the CFG");

bool EnforceSingleExitPass::runOnFunction(llvm::Function &F) {

  // Get the PostDominatorTree
  auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();

  // Get the LoopInfo
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

  // Instantiate and call the `Impl` class, by passing the `PostDominatorTree`
  // and the `LoopInfo` analyses to the `Impl` class
  EnforceSingleExitPassImpl ESEImpl(F, PDT, LI);
  bool FunctionChanged = ESEImpl.run();

  // This is a pass which can transform the CFG by inserting blocks and
  // redirecting edges, and therefore may not preserve the CFG, and we need to
  // signal this to the `FunctionPassManager`
  return FunctionChanged;
}

void EnforceSingleExitPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {

  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
}
