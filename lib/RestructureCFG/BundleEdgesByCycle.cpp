//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/IR/IRBuilder.h"

#include "revng/Support/Assert.h"

#include "revng-c/RestructureCFG/BundleEdgesByCycle.h"
#include "revng-c/RestructureCFG/CycleEquivalenceAnalysis.h"
#include "revng-c/RestructureCFG/CycleEquivalencePass.h"

char BundleEdgesByCyclePass::ID = 0;

static constexpr const char *Flag = "bundle-edges-by-cycle";
using Reg = llvm::RegisterPass<BundleEdgesByCyclePass>;
static Reg X(Flag, "Bundle Edges by Cycle Equivalence Class ID");

using namespace llvm;

bool BundleEdgesByCyclePass::runOnFunction(llvm::Function &F) {

  // Retrieve the `CycleEquivalence` classes
  using CER = CycleEquivalenceAnalysis<Function *>::CycleEquivalenceResult;
  using EdgeDescriptor = CER::EdgeDescriptor;
  using NakedEdgeDescriptor = CER::NakedEdgeDescriptor;

  // We need to obtain a copy of the `CycleEquivalenceResult`, because in this
  // pass we need to insert the mapping for the additional edges that we
  // introduce during the transformation
  CER CycleEquivalenceResult = getAnalysis<CycleEquivalencePass>().getResult();

  // We iterate over the `BasicBlock`s in the function in postorder
  for (BasicBlock *BB : post_order(&F)) {

    // For each node, we divide the predecessors, if they belong to different
    // `Cycle Equivalence Class ID`s, so that they are grouped into different
    // entry nodes
    // TODO: at this stage of the implementation, we blindly create a new
    //       predecessor node for each incoming `Cycle Equivalence Class ID`,
    //       instead of doing it lazily only when necessary. This needs to be
    //       addressed.
    std::map<size_t, BasicBlock *> CycleEquivalenceClassIDToBlockPredecessorMap;

    llvm::SmallVector<BasicBlock *> Predecessors;
    for (BasicBlock *Predecessor : predecessors(BB)) {
      Predecessors.push_back(Predecessor);
    }

    for (BasicBlock *Predecessor : Predecessors) {
      BasicBlock *DispatcherPredecessor = nullptr;

      // We verify if we already have a predecessor block for a certain `cycle
      // equivalence class ID`, and in case we materialize one
      auto PredecessorEdge = NakedEdgeDescriptor(Predecessor, BB);
      size_t
        PredecessorEdgeID = CycleEquivalenceResult
                              .getExactCycleEquivalenceClassID(PredecessorEdge);
      auto It = CycleEquivalenceClassIDToBlockPredecessorMap
                  .find(PredecessorEdgeID);
      if (It != CycleEquivalenceClassIDToBlockPredecessorMap.end()) {
        DispatcherPredecessor = It->second;
      } else {

        // We create a new dedicated block for grouping all the predecessor
        // edges belonging to a specific `Cycle Equivalence Class ID`. We add to
        // the block name as a suffix the `Cycle Equivalence Class ID`.
        size_t Index = PredecessorEdgeID;
        DispatcherPredecessor = BasicBlock::Create(BB->getContext(),
                                                   BB->getName() + "_pred_ceci_"
                                                     + std::to_string(Index),
                                                   &F);

        // We connect the new BasicBlock to `BB`
        IRBuilder<> Builder(F.getContext());
        Builder.SetInsertPoint(DispatcherPredecessor);
        Builder.CreateBr(BB);

        // Assign the `Cycle Equivalence Class ID` for the new edge. The
        // `SuccNum` is by construction 0 (the inserted edge is the first and
        // only successor edge for the `DispatcherPredecessor` block).
        CycleEquivalenceResult.insert(EdgeDescriptor(DispatcherPredecessor,
                                                     BB,
                                                     0),
                                      PredecessorEdgeID);

        // Populate the `CycleEquivalenceClassID` to the `DispatcherPredecessor`
        // mapping
        CycleEquivalenceClassIDToBlockPredecessorMap
          [PredecessorEdgeID] = DispatcherPredecessor;
      }

      // Connect `Predecessor` to `DispatcherPredecessor` instead of `BB`
      llvm::Instruction *Terminator = Predecessor->getTerminator();

      // We need to explicitly keep track of the index of the successor we are
      // replacing, so that we can correctly use it to as the `SuccNum`
      // parameter when populating the `CycleEquivalenceResult` mapping
      bool Substituted = false;
      for (size_t Idx = 0, NumSuccessors = Terminator->getNumSuccessors();
           Idx < NumSuccessors;
           ++Idx) {
        if (Terminator->getSuccessor(Idx) == BB) {

          // Confirm that we are performing a single substitution
          revng_assert(not Substituted);
          Substituted = true;

          Terminator->setSuccessor(Idx, DispatcherPredecessor);
          CycleEquivalenceResult.insert(EdgeDescriptor(Predecessor,
                                                       DispatcherPredecessor,
                                                       Idx),
                                        PredecessorEdgeID);
        }
      }
    }

    // For each node, we divide the successors, if they belong to different
    // `Cycle Equivalence Class ID`s, so that they are grouped into different
    // exit nodes
    std::map<size_t, BasicBlock *> CycleEquivalenceClassIDToBlockSuccessorMap;

    llvm::SmallVector<BasicBlock *> Successors;
    for (BasicBlock *Successor : successors(BB)) {
      Successors.push_back(Successor);
    }

    for (BasicBlock *Successor : Successors) {
      BasicBlock *DispatcherSuccessor = nullptr;

      // We verify if we already have a successor block for a specific `cycle
      // equivalence class ID`, and in case we materialize one
      auto SuccessorEdge = NakedEdgeDescriptor(BB, Successor);
      size_t
        SuccessorEdgeID = CycleEquivalenceResult
                            .getExactCycleEquivalenceClassID(SuccessorEdge);
      auto It = CycleEquivalenceClassIDToBlockSuccessorMap
                  .find(SuccessorEdgeID);
      if (It != CycleEquivalenceClassIDToBlockSuccessorMap.end()) {
        DispatcherSuccessor = It->second;
      } else {

        // We create a new dedicated block for grouping all the successor edge
        // belonging to a specific `Cycle Equivalence Class ID`. We add to the
        // block name as a suffix the `Cycle Equivalence Class ID`.
        size_t Index = SuccessorEdgeID;
        DispatcherSuccessor = BasicBlock::Create(BB->getContext(),
                                                 BB->getName() + "_succ_ceci_"
                                                   + std::to_string(Index),
                                                 &F);

        // We connect the new BasicBlock to `BB`
        IRBuilder<> Builder(F.getContext());
        Builder.SetInsertPoint(DispatcherSuccessor);
        Builder.CreateBr(Successor);

        // Assign the `Cycle Equivalence Class ID` for the new edge. The
        // `SuccNum` is by construction 0 (the inserted edge is the first and
        // only successor edge for the `DispatcherSuccessor` block).
        CycleEquivalenceResult.insert(EdgeDescriptor(DispatcherSuccessor,
                                                     Successor,
                                                     0),
                                      SuccessorEdgeID);

        // Populate the `CycleEquivalenceClassID` to the `DispatcherSuccessor`
        // mapping
        CycleEquivalenceClassIDToBlockSuccessorMap
          [SuccessorEdgeID] = DispatcherSuccessor;
      }

      // Connect `BB` to `DispatcherSuccessor` instead of `Successor`
      llvm::Instruction *Terminator = BB->getTerminator();

      // We need to explicitly keep track of the index of the successor we are
      // replacing, so that we can correctly use it to as the `SuccNum`
      // parameter when populating the `CycleEquivalenceResult` mapping
      bool Substituted = false;
      for (size_t Idx = 0, NumSuccessors = Terminator->getNumSuccessors();
           Idx < NumSuccessors;
           ++Idx) {
        if (Terminator->getSuccessor(Idx) == Successor) {

          // Confirm that we are performing a single substitution
          revng_assert(not Substituted);
          Substituted = true;

          Terminator->setSuccessor(Idx, DispatcherSuccessor);
          CycleEquivalenceResult.insert(EdgeDescriptor(BB,
                                                       DispatcherSuccessor,
                                                       Idx),
                                        SuccessorEdgeID);
        }
      }
    }
  }

  // Show the CFG after the changes have been applied
  F.viewCFG();

  // This pass modifies the module, adding predecessors and successors to the
  // nodes
  return true;
}

void BundleEdgesByCyclePass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {

  // This pass depends on the `Cycle Equivalence` analysis
  AU.addRequired<CycleEquivalencePass>();
}
