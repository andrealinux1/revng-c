//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"

#include "revng-c/RestructureCFG/GenericRegionInfo.h"
#include "revng-c/RestructureCFG/GenericRegionPass.h"
#include "revng-c/RestructureCFG/InlineDivergentBranchesPass.h"
#include "revng-c/RestructureCFG/ScopeCloserGraphTraits.h"
#include "revng-c/RestructureCFG/ScopeCloserUtils.h"

// Implementation class used to run the `IDB` transformation
class InlineDivergentBranchesImpl {
  llvm::Function &F;

public:
  InlineDivergentBranchesImpl(llvm::Function &F) : F(F) {}

public:
  void run() {

    // When the `SCMBuilder` is constructed, the
    ScopeCloserMarkerBuilder SCMBuilder(&F);

    // TODO: remove this, which is only for quick testing purpose
    // Insert a self-looping `ScopeCloser` (just to have a destination for our
    // edge)
    for (llvm::BasicBlock &BB : F) {

      // We cannot obtain a `BlockAddress` for the entry block of a function
      if (not(&BB == &F.getEntryBlock())) {
        SCMBuilder.setInsertPoint(&BB);
        SCMBuilder.insertScopeCloserTarget(&BB);
      }
    }

    // Test iteration which uses `llvm::depth_first` on the `llvm::Dashed` graph
    llvm::BasicBlock *EntryBlock = &F.getEntryBlock();
    for (llvm::BasicBlock *BB : llvm::depth_first(llvm::Dashed(EntryBlock))) {
      dbg << "Block " << BB->getName().str() << " scope graph successors:\n";
      using ScopeGraph = llvm::GraphTraits<llvm::Dashed<llvm::BasicBlock *>>;

      for (auto Succ = ScopeGraph::child_begin(BB);
           Succ != ScopeGraph::child_end(BB);
           ++Succ) {
        dbg << "  " << (*Succ)->getName().str() << "\n";
      }
    }

    for (llvm::BasicBlock &BB : F) {
      dbg << "Block " << BB.getName().str() << " scope graph successors:\n";
      using ScopeGraph = llvm::GraphTraits<llvm::Dashed<llvm::BasicBlock *>>;
      for (auto Succ = ScopeGraph::child_begin(&BB);
           Succ != ScopeGraph::child_end(&BB);
           ++Succ) {
        dbg << " " << (*Succ)->getName().str() << "\n";
      }
    }
  }
};

char InlineDivergentBranchesPass::ID = 0;

static constexpr const char *Flag = "inline-divergent-branches";
using Reg = llvm::RegisterPass<InlineDivergentBranchesPass>;
static Reg
  X(Flag, "Perform the inline of divergent branches canonicalization process");

bool InlineDivergentBranchesPass::runOnFunction(llvm::Function &F) {

  // Instantiate and call the `Impl` class
  InlineDivergentBranchesImpl IDBImpl(F);
  IDBImpl.run();

  // This is a pass which can transform the CFG by inserting blocks and
  // redirecting edges, and therefore does not preserve the CFG
  return true;
}

void InlineDivergentBranchesPass::getAnalysisUsage(llvm::AnalysisUsage &AU)
  const {

  // We need the information about the computed `GenericRegion`s in order to
  // perform the DAGify operation on the graph
  // TODO: verify this is needed in this step
  AU.addRequired<GenericRegionPass>();
}
