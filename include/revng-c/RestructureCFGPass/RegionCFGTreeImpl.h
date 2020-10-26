#ifndef REVNGC_RESTRUCTURE_CFG_REGIONCFGTREEIMPL_H
#define REVNGC_RESTRUCTURE_CFG_REGIONCFGTREEIMPL_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <llvm/ADT/SmallVector.h>
#include <sys/stat.h>

// LLVM includes
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/GenericDomTreeConstruction.h>
#include <llvm/Support/raw_os_ostream.h>

// revng includes
#include <revng/BasicAnalyses/GeneratedCodeBasicInfo.h>
#include <revng/Support/IRHelpers.h>

// Local libraries includes
#include "revng-c/ADT/ReversePostOrderTraversal.h"
#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/BasicBlockNodeBB.h"
#include "revng-c/RestructureCFGPass/MetaRegionBB.h"
#include "revng-c/RestructureCFGPass/RegionCFGTree.h"
#include "revng-c/RestructureCFGPass/Utils.h"

template<typename IterT>
bool intersect(IterT I1, IterT E1, IterT I2, IterT E2) {
  while ((I1 != E1) and (I2 != E2)) {
    if (*I1 < *I2)
      ++I1;
    else if (*I2 < *I1)
      ++I2;
    else
      return true;
  }
  return false;
}

template<typename IterT>
bool disjoint(IterT I1, IterT E1, IterT I2, IterT E2) {
  return not intersect(I1, E1, I2, E2);
}

template<typename RangeT>
bool intersect(const RangeT &R1, const RangeT &R2) {
  return intersect(R1.begin(), R1.end(), R2.begin(), R2.end());
}

template<typename RangeT>
bool disjoint(const RangeT &R1, const RangeT &R2) {
  return not intersect(R1, R2);
}

unsigned const SmallSetSize = 16;

// llvm::SmallPtrSet is a handy way to store set of BasicBlockNode pointers.
template<class NodeT>
using SmallPtrSet = llvm::SmallPtrSet<BasicBlockNode<NodeT> *, SmallSetSize>;

template<class NodeT>
inline void RegionCFG<NodeT>::setFunctionName(std::string Name) {
  FunctionName = Name;
}

template<class NodeT>
inline void RegionCFG<NodeT>::setRegionName(std::string Name) {
  RegionName = Name;
}

template<class NodeT>
inline std::string RegionCFG<NodeT>::getFunctionName() const {
  return FunctionName;
}

template<class NodeT>
inline std::string RegionCFG<NodeT>::getRegionName() const {
  return RegionName;
}

template<class NodeT>
inline BasicBlockNode<NodeT> *
RegionCFG<NodeT>::addNode(NodeT Node, llvm::StringRef Name) {
  using BBNodeT = BasicBlockNodeT;
  BlockNodes.emplace_back(std::make_unique<BBNodeT>(this, Node, Name));
  BasicBlockNodeT *Result = BlockNodes.back().get();
  revng_log(CombLogger,
            "Building " << Name << " at address: " << Result << "\n");
  return Result;
}

template<class NodeT>
inline BasicBlockNode<NodeT> *
RegionCFG<NodeT>::cloneNode(BasicBlockNodeT &OriginalNode) {
  using BBNodeT = BasicBlockNodeT;
  BlockNodes.emplace_back(std::make_unique<BBNodeT>(OriginalNode, this));
  BasicBlockNodeT *New = BlockNodes.back().get();
  New->setName(OriginalNode.getName().str() + " cloned");
  New->setWeaved(OriginalNode.isWeaved());
  return New;
}

template<class NodeT>
inline void RegionCFG<NodeT>::removeNode(BasicBlockNodeT *Node) {

  revng_log(CombLogger, "Removing node named: " << Node->getNameStr() << "\n");

  for (BasicBlockNodeT *Predecessor : Node->predecessors())
    Predecessor->removeSuccessor(Node);

  for (BasicBlockNodeT *Successor : Node->successors())
    Successor->removePredecessor(Node);

  for (auto It = BlockNodes.begin(); It != BlockNodes.end(); It++) {
    if ((*It).get() == Node) {
      BlockNodes.erase(It);
      break;
    }
  }
}

template<class NodeT>
using BBNodeT = typename RegionCFG<NodeT>::BasicBlockNodeT;

template<class NodeT>
inline void copyNeighbors(BBNodeT<NodeT> *Dst, BBNodeT<NodeT> *Src) {
  for (const auto &P : Src->labeled_successors())
    Dst->addLabeledSuccessor(P);
  for (const auto &P : Src->labeled_predecessors())
    Dst->addLabeledPredecessor(P);
}

template<class NodeT>
inline void RegionCFG<NodeT>::insertBulkNodes(BasicBlockNodeTSet &Nodes,
                                              BasicBlockNodeT *Head,
                                              BBNodeMap &SubMap) {
  revng_assert(BlockNodes.empty());

  for (BasicBlockNodeT *Node : Nodes) {
    BlockNodes.emplace_back(std::make_unique<BasicBlockNodeT>(*Node, this));
    BasicBlockNodeT *New = BlockNodes.back().get();
    SubMap[Node] = New;

    // The copy constructor used above does not bring along the successors and
    // the predecessors, neither adjusts the parent.
    // The following lines are a hack to fix this problem, but they momentarily
    // build a broken data structure where the predecessors and the successors
    // of the New BasicBlockNodes in *this still refer to the BasicBlockNodes in
    // the Parent CFGRegion of Nodes. This will be fixed later by updatePointers
    copyNeighbors<NodeT>(New, Node);
  }

  revng_assert(Head != nullptr);
  EntryNode = SubMap[Head];
  revng_assert(EntryNode != nullptr);
  // Fix the hack above
  for (BBNodeTUniquePtr &Node : BlockNodes)
    Node->updatePointers(SubMap);
}

template<class NodeT>
using lk_iterator = typename RegionCFG<NodeT>::links_container::iterator;

template<class NodeT>
inline llvm::iterator_range<lk_iterator<NodeT>>
RegionCFG<NodeT>::copyNodesAndEdgesFrom(RegionCFGT *O, BBNodeMap &SubMap) {
  typename links_container::difference_type NumNewNodes = 0;

  for (BasicBlockNode<NodeT> *Node : *O) {
    BlockNodes.emplace_back(std::make_unique<BasicBlockNodeT>(*Node, this));
    ++NumNewNodes;
    BasicBlockNodeT *New = BlockNodes.back().get();
    SubMap[Node] = New;
    copyNeighbors<NodeT>(New, Node);
  }

  internal_iterator BeginInserted = BlockNodes.end() - NumNewNodes;
  internal_iterator EndInserted = BlockNodes.end();
  using MovedIteratorRange = llvm::iterator_range<internal_iterator>;
  MovedIteratorRange Result = llvm::make_range(BeginInserted, EndInserted);
  for (std::unique_ptr<BasicBlockNode<NodeT>> &NewNode : Result)
    NewNode->updatePointers(SubMap);
  return Result;
}

template<class NodeT>
inline void RegionCFG<NodeT>::connectBreakNode(std::set<EdgeDescriptor> &Out,
                                               const BBNodeMap &SubMap) {
  for (EdgeDescriptor Edge : Out) {
    // Create a new break for each outgoing edge.
    auto *Break = addBreak();
    auto &EdgeInfo = Edge.first->getSuccessorEdge(Edge.second).second;
    addEdge<BBNodeT>({ SubMap.at(Edge.first), Break }, EdgeInfo);
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::connectContinueNode() {
  BasicBlockNodeTVect ContinueNodes;

  // We need to pre-save the edges to avoid breaking the predecessor iterator
  for (BasicBlockNode<NodeT> *Source : EntryNode->predecessors()) {
    ContinueNodes.push_back(Source);
  }

  for (BasicBlockNode<NodeT> *Source : ContinueNodes) {
    // Create a new continue node for each retreating edge.
    BasicBlockNode<NodeT> *Continue = addContinue();
    moveEdgeTarget(EdgeDescriptor(Source, EntryNode), Continue);
  }
}

template<class NodeT>
template<typename StreamT>
inline void
RegionCFG<NodeT>::streamNode(StreamT &S, const BasicBlockNodeT *BB) const {
  unsigned NodeID = BB->getID();
  S << "\"" << NodeID << "\"";
  S << " ["
    << "label=\"ID: " << NodeID << " Name: " << BB->getNameStr() << "\"";
  if (BB == EntryNode)
    S << ",fillcolor=green,style=filled";
  S << "];\n";
}

/// \brief Dump a GraphViz file on stdout representing this function
template<class NodeT>
template<typename StreamT>
inline void RegionCFG<NodeT>::dumpDot(StreamT &S) const {
  S << "digraph CFGFunction {\n";

  for (const std::unique_ptr<BasicBlockNode<NodeT>> &BB : BlockNodes) {
    streamNode(S, BB.get());
    for (const auto &[Successor, EdgeInfo] : BB->labeled_successors()) {
      unsigned PredID = BB->getID();
      unsigned SuccID = Successor->getID();
      S << "\"" << PredID << "\""
        << " -> \"" << SuccID << "\"";
      if (EdgeInfo.Inlined)
        S << " [color=purple];\n";
      else
        S << " [color=green];\n";
    }
  }
  S << "}\n";
}

template<class NodeT>
inline void RegionCFG<NodeT>::dumpDotOnFile(const std::string &FileName) const {
  std::error_code EC;
  llvm::raw_fd_ostream DotFile(FileName, EC);
  revng_check(not EC, "Could not open file for printing RegionCFG dot");
  dumpDot(DotFile);
}

template<class NodeT>
inline void RegionCFG<NodeT>::dumpDotOnFile(const std::string &FolderName,
                                            const std::string &FuncName,
                                            const std::string &FileName) const {
  std::error_code EC = llvm::sys::fs::create_directory(FolderName);
  revng_check(not EC, "Could not create directory to print RegionCFG dot");
  const std::string PathName = FolderName + "/" + FuncName;
  EC = llvm::sys::fs::create_directory(PathName);
  revng_check(not EC, "Could not create directory to print RegionCFG dot");
  dumpDotOnFile(PathName + "/" + FileName);
}

template<class NodeT>
inline bool RegionCFG<NodeT>::purgeIfTrivialDummy(BBNodeT *Dummy) {
  RegionCFG<NodeT> &Graph = *this;

  revng_assert(not Dummy->isEmpty() or Dummy->predecessor_size() != 0);

  if ((Dummy->isEmpty()) and (Dummy->predecessor_size() == 1)
      and (Dummy->successor_size() == 1)) {

    revng_log(CombLogger, "Purging dummy node " << Dummy->getNameStr());

    BasicBlockNode<NodeT> *Predecessor = *Dummy->predecessors().begin();
    BasicBlockNode<NodeT> *Successor = *Dummy->successors().begin();

    // Connect directly predecessor and successor, and remove the dummy node
    // under analysis
    moveEdgeTarget({ Predecessor, Dummy }, Successor);
    Graph.removeNode(Dummy);
    return true;
  }

  return false;
}

template<class NodeT>
inline bool RegionCFG<NodeT>::purgeTrivialDummies() {
  RegionCFG<NodeT> &Graph = *this;
  bool RemovedNow = true;
  bool Removed = false;

  while (RemovedNow) {
    RemovedNow = false;
    for (auto *Node : Graph) {
      RemovedNow = purgeIfTrivialDummy(Node);
      if (RemovedNow) {
        Removed = true;
        break;
      }
    }
  }

  return Removed;
}

template<class NodeT>
inline void RegionCFG<NodeT>::purgeVirtualSink(BasicBlockNode<NodeT> *Sink) {

  RegionCFG<NodeT> &Graph = *this;

  BasicBlockNodeTVect WorkList;
  BasicBlockNodeTVect PurgeList;

  WorkList.push_back(Sink);

  while (!WorkList.empty()) {
    BasicBlockNode<NodeT> *CurrentNode = WorkList.back();
    WorkList.pop_back();

    if (CurrentNode->isEmpty()) {
      PurgeList.push_back(CurrentNode);

      for (BasicBlockNode<NodeT> *Predecessor : CurrentNode->predecessors()) {
        WorkList.push_back(Predecessor);
      }
    }
  }

  for (BasicBlockNode<NodeT> *Purge : PurgeList) {
    Graph.removeNode(Purge);
  }
}

inline bool isGreater(unsigned Op1, unsigned Op2) {
  unsigned MultiplicativeFactor = 1;
  if (Op1 > (MultiplicativeFactor * Op2)) {
    return true;
  } else {
    return false;
  }
}

template<class NodeT>
inline BasicBlockNode<NodeT> *
RegionCFG<NodeT>::cloneUntilExit(BasicBlockNode<NodeT> *Node,
                                 BasicBlockNode<NodeT> *Sink) {

  // Clone the postdominator node.
  BBNodeMap CloneMap;
  BasicBlockNode<NodeT> *Clone = cloneNode(*Node);

  // Insert the postdominator clone in the map.
  CloneMap[Node] = Clone;

  BasicBlockNodeTVect WorkList;
  WorkList.push_back(Node);

  // Set of nodes which have been already processed.
  BasicBlockNodeTSet AlreadyProcessed;

  while (!WorkList.empty()) {
    BasicBlockNode<NodeT> *CurrentNode = WorkList.back();
    WorkList.pop_back();

    // Ensure that we are not processing the sink node.
    revng_assert(CurrentNode != Sink);

    if (AlreadyProcessed.count(CurrentNode) == 0) {
      AlreadyProcessed.insert(CurrentNode);
    } else {
      continue;
    }

    // Get the clone of the `CurrentNode`.
    BasicBlockNode<NodeT> *CurrentClone = CloneMap.at(CurrentNode);

    for (const auto &[Succ, Labels] : CurrentNode->labeled_successors()) {
      // If the successor is not the sink, create and edge that directly
      // connects it.
      if (Succ != Sink) {
        BasicBlockNode<NodeT> *SuccessorClone = nullptr;

        // The clone of the successor node already exists.
        auto CloneIt = CloneMap.find(Succ);
        if (CloneIt != CloneMap.end()) {
          SuccessorClone = CloneIt->second;
        } else {
          // The clone of the successor does not exist, create it in place.
          SuccessorClone = cloneNode(*Succ);
          CloneMap[Succ] = SuccessorClone;
        }

        // Create the edge to the clone of the successor.
        revng_assert(SuccessorClone != nullptr);
        addEdge(EdgeDescriptor(CurrentClone, SuccessorClone), Labels);

        // Add the successor to the worklist.
        WorkList.push_back(Succ);
      }
    }
  }

  return Clone;
}

template<class NodeT>
inline void RegionCFG<NodeT>::untangle() {
  // TODO: Here we handle only conditional nodes with two successors. We should
  //       consider extending the untangle procedure also to conditional nodes
  //       with more than two successors (switch nodes).

  revng_assert(isDAG());

  RegionCFG<NodeT> &Graph = *this;

  // Add a new virtual sink node to computer the postdominator.
  BasicBlockNode<NodeT> *Sink = Graph.addArtificialNode("Sink");
  for (auto *Node : Graph)
    if (Node != Sink and Node->successor_size() == 0)
      addPlainEdge(EdgeDescriptor(Node, Sink));

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("untangle",
                        FunctionName,
                        "Region-" + RegionName + "-initial-state");
  }

  // Map which contains the precomputed wheight for each node in the graph. In
  // case of a code node the weight will be equal to the number of instruction
  // in the original basic block; in case of a collapsed node the weight will be
  // the sum of the weights of all the nodes contained in the collapsed graph.
  std::map<BasicBlockNode<NodeT> *, size_t> WeightMap;
  for (BasicBlockNode<NodeT> *Node : Graph.nodes()) {
    WeightMap[Node] = Node->getWeight();
  }

  // Collect all the conditional nodes in the graph into a vector sorted in
  // Reverse Post-Order.
  BasicBlockNodeTVect ConditionalNodes;
  {
    BasicBlockNodeTSet ConditionalNodesSet;
    for (auto *Node : Graph)
      if (Node->successor_size() == 2)
        ConditionalNodesSet.insert(Node);

    llvm::ReversePostOrderTraversal<BasicBlockNode<NodeT> *> RPOT(EntryNode);

    for (BasicBlockNode<NodeT> *RPOTBB : RPOT) {
      if (ConditionalNodesSet.count(RPOTBB) != 0) {
        ConditionalNodes.push_back(RPOTBB);
      }
    }
  }

  while (not ConditionalNodes.empty()) {

    if (CombLogger.isEnabled()) {
      Graph.dumpDotOnFile("untangle",
                          FunctionName,
                          "Region-" + RegionName + "-debug");
    }

    BasicBlockNode<NodeT> *Conditional = ConditionalNodes.back();
    ConditionalNodes.pop_back();

    // Update the information of the dominator and postdominator trees.
    DT.recalculate(Graph);
    IFPDT.recalculate(Graph);

    // Update the postdominator
    BasicBlockNodeT *PostDominator = IFPDT[Conditional]->getIDom()->getBlock();

    // Ensure that we have both the successors.
    revng_assert(Conditional->successor_size() == 2);

    // Get the first node of the then and else branches respectively.
    // TODO: Check that this is the right way to do this. At this point we
    //       cannot assume that we have the `getThen()` and `getFalse()`
    //       methods.
    BasicBlockNode<NodeT> *ThenChild = Conditional->getSuccessorI(0);
    BasicBlockNode<NodeT> *ElseChild = Conditional->getSuccessorI(1);

    // Collect all the nodes laying between the branches
    BasicBlockNodeTSet ThenNodes = findReachableNodes(ThenChild, PostDominator);
    BasicBlockNodeTSet ElseNodes = findReachableNodes(ElseChild, PostDominator);

    // Remove the postdominator from both the sets.
    ThenNodes.erase(PostDominator);
    ElseNodes.erase(PostDominator);

    const auto EdgeDominates = [DT = &DT](const EdgeDescriptor &E,
                                          BasicBlockNodeT *N) {
      const auto &[Src, Dst] = E;

      if (not DT->dominates(Dst, N))
        return false;

      if (Dst->predecessor_size() < 2)
        return true;

      bool DuplicateEdge = false;
      for (BasicBlockNodeT *Pred : Dst->predecessors()) {
        if (Pred == Src) {
          if (DuplicateEdge)
            return false;
          DuplicateEdge = true;
          continue;
        }

        if (not DT->dominates(Dst, Pred))
          return false;
      }
      return true;
    };

    // New implementation of the dominance criterion which uses the then and
    // else edges to compute the dominance.
    if (EdgeDominates({ Conditional, ElseChild }, ElseChild)) {
      const auto DominatedByElse = [DT = &DT, ElseChild](auto *Node) {
        return DT->dominates(ElseChild, Node);
      };
      // TODO: substitute the following loop with std::set::erase_if when it
      // becomes available.
      for (auto I = ElseNodes.begin(), E = ElseNodes.end(); I != E;) {
        if (DominatedByElse(*I))
          I = ElseNodes.erase(I);
        else
          ++I;
      }
    }

    if (EdgeDominates({ Conditional, ThenChild }, ThenChild)) {
      const auto DominatedByThen = [DT = &DT, ThenChild](auto *Node) {
        return DT->dominates(ThenChild, Node);
      };
      // TODO: substitute the following loop with std::set::erase_if when it
      // becomes available.
      for (auto I = ThenNodes.begin(), E = ThenNodes.end(); I != E;) {
        if (DominatedByThen(*I))
          I = ThenNodes.erase(I);
        else
          ++I;
      }
    }

    // Compute the weight of the `then` and `else` branches.
    unsigned ThenWeight = 0;
    unsigned ElseWeight = 0;

    for (BasicBlockNode<NodeT> *Node : ThenNodes) {
      ThenWeight += WeightMap[Node];
    }

    for (BasicBlockNode<NodeT> *Node : ElseNodes) {
      ElseWeight += WeightMap[Node];
    }

    // The weight of the nodes placed after the immediate postdominator is the
    // sum of all the weights of the nodes which are reachable starting from the
    // immediate post dominator and the sink node (to which all the exits have
    // been connected).
    // If the post dominator is `nullptr` (meaning that it is the `VirtualRoot`
    // node on the filtered post dominator tree), we can skip the computation of
    // this weight.
    unsigned PostDominatorWeight = 0;
    if (PostDominator != nullptr) {
      BasicBlockNodeTSet PostDominatorToExit = findReachableNodes(PostDominator,
                                                                  Sink);
      for (BasicBlockNode<NodeT> *Node : PostDominatorToExit) {
        PostDominatorWeight += WeightMap[Node];
      }
    }

    // Criterion which decides if we can apply the untangle optimization to the
    // conditional under analysis.
    // We define 3 weights:
    // - 1) weight(then) + weight(else)
    // - 2) weight(then) + weight(postdom)
    // - 3) weight(else) + weight(postdom)
    //
    // We need to operate the split if:
    // 2 >> 3
    // 1 >> 3
    // and specifically we need to split the `else` branch.
    //
    // We need to operate the split if:
    // 3 >> 2
    // 1 >> 2
    // and specifically we need to split the `then` branch.
    //
    // We can also define in a dynamic way the >> operator, so we can change the
    // threshold that triggers the split.

    unsigned CombingCost = ThenWeight + ElseWeight;
    unsigned UntangleThenCost = ThenWeight + PostDominatorWeight;
    unsigned UntangleElseCost = ElseWeight + PostDominatorWeight;
    unsigned UntanglingCost = std::min(UntangleThenCost, UntangleElseCost);

    if (isGreater(CombingCost, UntanglingCost)) {
      revng_log(CombLogger, FunctionName << ":");
      revng_log(CombLogger, RegionName << ":");
      revng_log(CombLogger,
                "Found untangle candidate " << Conditional->getNameStr());
      revng_log(CombLogger, "CombingCost:" << CombingCost);
      revng_log(CombLogger, "UntangleThenCost:" << UntangleThenCost);
      revng_log(CombLogger, "UntangleElseCost:" << UntangleElseCost);

      // Register a tentative untangle in the dedicated counter.
      UntangleTentativeCounter++;

      // Register an actual untangle in the dedicated counter.
      UntanglePerformedCounter++;
      revng_log(CombLogger, "Actually splitting node");

      auto *ToUntangle = (UntangleThenCost > UntangleElseCost) ? ElseChild :
                                                                 ThenChild;
      // Perform the split from the first node of the then/else branches.
      // We fully inline all the nodes belonging to the branch we are untangling
      // till the exit node.
      BasicBlockNode<NodeT> *UntangledChild = cloneUntilExit(ToUntangle, Sink);

      // Move the edge coming out of the conditional node to the new clone of
      // the node.
      moveEdgeTarget(EdgeDescriptor(Conditional, ToUntangle), UntangledChild);

      // We mark the edge going into the `UntangleChild` as an inlined edge.
      // In this way, in all the next phases, these edges will be ignored by the
      // dominator and postdominator trees.
      markEdgeInlined(EdgeDescriptor(Conditional, UntangledChild));

      // Remove nodes that have no predecessors (nodes that are the result of
      // node cloning and that remains dandling around).
      // While doing this, update InlineEdges.
      bool Removed = true;
      while (Removed) {
        Removed = false;
        BasicBlockNode<NodeT> *Entry = &getEntryNode();
        for (auto It = begin(); It != end(); ++It) {
          if ((Entry != *It and (*It)->predecessor_size() == 0)) {
            removeNode(*It);
            Removed = true;
            break;
          }
        }
      }
    }
  }

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("untangle",
                        FunctionName,
                        "Region-" + RegionName + "-after-processing");
  }

  // Remove the sink node.
  purgeVirtualSink(Sink);

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("untangle",
                        FunctionName,
                        "Region-" + RegionName + "-after-sink-removal");
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::inflate() {

  // Call the untangle preprocessing.
  untangle();

  revng_assert(isDAG());

  // Apply the comb to a RegionCFG object.
  // TODO: handle all the collapsed regions.
  RegionCFG<NodeT> &Graph = *this;

  BasicBlockNode<NodeT> *Entry = &Graph.getEntryNode();

  if (CombLogger.isEnabled()) {
    revng_log(CombLogger, "Entry node is: " << Entry->getNameStr());
    Graph.dumpDotOnFile("inflates",
                        FunctionName,
                        "Region-" + RegionName + "-before-combing");
  }

  // Add a new virtual sink node to which all the reachable exit nodes are
  // connected. Also collect the sets of reachable exits from each node that is
  // a successor of a node that induces duplication.
  std::map<BasicBlockNode<NodeT> *, BasicBlockNodeTSet> ReachableExits;
  for (auto *Exit : Graph) {

    bool HasOnlyInlinedSuccessor = true;
    for (auto &Succ : Exit->labeled_successors())
      if (not Succ.second.Inlined)
        HasOnlyInlinedSuccessor = false;

    if (HasOnlyInlinedSuccessor) {
      revng_log(CombLogger, "From exit node: " << Exit->getNameStr());
      revng_log(CombLogger, "We can reach:");
      for (BasicBlockNode<NodeT> *Node :
           llvm::inverse_depth_first(EFGT<BBNodeT *>(Exit))) {
        revng_log(CombLogger, Node->getNameStr());
        ReachableExits[Node].insert(Exit);
      }
    }
  }

  // Refresh information of dominator and postdominator trees.
  DT.recalculate(Graph);
  IFPDT.recalculate(Graph);

  // Map to hold, for each conditional node that initiates combing, the node
  // that will be used to detect the point where combing needs to stop
  // duplicating node. This is the immediate post dominator for most nodes, but
  // we have a special case for the case nodes of switches.
  BBNodeMap ConditionalToCombEnd;

  // Collect all the conditional nodes in the graph.
  // This is the working list of conditional nodes on which we will operate and
  // will contain only the filtered conditionals.
  BasicBlockNodeTSet ConditionalNodesSet;

  std::vector<BasicBlockNode<NodeT> *> Switches;

  for (BBNodeT *Node : Graph) {
    switch (Node->successor_size()) {

    case 0:
    case 1:
      // We don't need to add it to the conditional nodes vector.
      break;

    case 2: {
      BasicBlockNodeTSet ThenExits = ReachableExits.at(Node->getSuccessorI(0));
      BasicBlockNodeTSet ElseExits = ReachableExits.at(Node->getSuccessorI(1));

      // Add the conditional node to the set of nodes processed by the inflate.
      ConditionalNodesSet.insert(Node);
      BasicBlockNode<NodeT> *PostDom = IFPDT[Node]->getIDom()->getBlock();
      bool New = ConditionalToCombEnd.insert({ Node, PostDom }).second;
      revng_assert(New);

      // Check that we do not dominate at maximum on of the two sets of
      // reachable exits.
      bool ThenIsDominated = true;
      bool ElseIsDominated = true;
      for (BasicBlockNode<NodeT> *Exit : ThenExits) {
        if (not DT.dominates(Node, Exit)) {
          ThenIsDominated = false;
          break;
        }
      }
      for (BasicBlockNode<NodeT> *Exit : ElseExits) {
        if (not DT.dominates(Node, Exit)) {
          ElseIsDominated = false;
          break;
        }
      }

      // If there is one set of exits that Node entirely dominates, we can
      // blacklist it because it will never cause duplication.
      // The reason is that the set of exits that we dominate can be completely
      // inlined and absorbed either into the then or into the else.
      if (ThenIsDominated or ElseIsDominated) {
        revng_log(CombLogger,
                  "Blacklisted conditional: " << Node->getNameStr());

        // Mark then or else edges as inlined during the conditional
        // blacklisting. In case both the `then` and `else` branches are
        // completely dominated, we mark both as inlineable.
        BasicBlockNodeT *Then = Node->getSuccessorI(0);
        BasicBlockNodeT *Else = Node->getSuccessorI(1);

        // TODO: Verify that, for nodes where the inling is applied to both the
        //       branches, the immediate post-dominator on the filtered postdom
        //       is the conditional node itself. Verify also that, if this is
        //       the situation, the inflating procedure does not go in a loop
        //       (it should stop immediately if conditional == postdom).
        if (ThenIsDominated and ElseIsDominated) {
          markEdgeInlined(EdgeDescriptor(Node, Then));
          markEdgeInlined(EdgeDescriptor(Node, Else));
        } else if (ThenIsDominated) {
          markEdgeInlined(EdgeDescriptor(Node, Then));
        } else if (ElseIsDominated) {
          markEdgeInlined(EdgeDescriptor(Node, Else));
        } else {
          revng_abort();
        }
      }
    } break;

    default: {
      Switches.push_back(Node);
    } break;
    }
  }

  for (auto *Switch : Switches) {
    llvm::SmallPtrSet<BasicBlockNode<NodeT> *, 8> CaseNodes;

    for (auto *SwitchCase : Switch->successors())
      CaseNodes.insert(SwitchCase);

    for (auto *Case : CaseNodes) {
      auto *DummyCase = addArtificialNode();
      moveEdgeTarget(EdgeDescriptor(Switch, Case), DummyCase);
      addPlainEdge(EdgeDescriptor(DummyCase, Case));

      ConditionalNodesSet.insert(DummyCase);
      BasicBlockNode<NodeT> *PostDom = IFPDT[Switch]->getIDom()->getBlock();
      // Combing of switch cases continues until the post dominator of the
      // switch, not until the post dominator of the case.
      bool New = ConditionalToCombEnd.insert({ DummyCase, PostDom }).second;
      revng_assert(New);
    }
  }

  if (CombLogger.isEnabled()) {
    revng_log(CombLogger, "Conditional nodes present in the graph are:");
    for (BasicBlockNode<NodeT> *Node : ConditionalNodesSet)
      revng_log(CombLogger, Node->getNameStr());
  }

  // Equivalence-class like set to keep track of all the cloned nodes created
  // starting from an original node.
  std::map<BasicBlockNode<NodeT> *, SmallPtrSet<NodeT>> NodesEquivalenceClass;

  // Map to keep track of the cloning relationship.
  BBNodeMap CloneToOriginalMap;

  // Initialize a list containing the reverse post order of the nodes of the
  // graph.
  std::list<BasicBlockNode<NodeT> *> RevPostOrderList;

  // Vector of conditional nodes, to be filled in reverse post order.
  BasicBlockNodeTVect ConditionalNodes;

  llvm::ReversePostOrderTraversal<BasicBlockNode<NodeT> *> RPOT(Entry);
  for (BasicBlockNode<NodeT> *RPOTBB : RPOT) {
    RevPostOrderList.push_back(RPOTBB);
    NodesEquivalenceClass[RPOTBB].insert(RPOTBB);
    CloneToOriginalMap[RPOTBB] = RPOTBB;
    if (ConditionalNodesSet.count(RPOTBB))
      ConditionalNodes.push_back(RPOTBB);
  }
  NodesEquivalenceClass[nullptr] = {};

  // Iterate on ConditionalNodes from the back. Given that they are inserted
  // into ConditionalNodes in RPOT, this iteration is in post-order.
  while (not ConditionalNodes.empty()) {

    // Process each conditional node after ordering it.
    BasicBlockNode<NodeT> *Conditional = ConditionalNodes.back();
    ConditionalNodes.pop_back();

    // Retrieve a reference to the set of postdominators.
    auto CombEndIt = ConditionalToCombEnd.find(Conditional);
    revng_assert(CombEndIt != ConditionalToCombEnd.end());
    auto CombEndSetIt = NodesEquivalenceClass.find(CombEndIt->second);
    revng_assert(CombEndSetIt != NodesEquivalenceClass.end());

    if (CombLogger.isEnabled()) {
      revng_log(CombLogger,
                "Analyzing conditional node " << Conditional->getNameStr());
      Graph.dumpDotOnFile("inflates",
                          FunctionName,
                          "Region-" + RegionName + "-conditional-"
                            + Conditional->getNameStr() + "-begin");
    }

    // List to keep track of the nodes that we still need to analyze.
    SmallPtrSet<NodeT> WorkList;
    // Enqueue in the worklist the successors of the contional node.
    for (auto &[Successor, EdgeLabel] : Conditional->labeled_successors())
      if (not EdgeLabel.Inlined)
        WorkList.insert(Successor);

    // Keep a set of the visited nodes for the current conditional node.
    SmallPtrSet<NodeT> Visited = { Conditional };

    // Get an iterator from the reverse post order list in the position of the
    // conditional node.
    auto ListIt = std::find(RevPostOrderList.begin(),
                            RevPostOrderList.end(),
                            Conditional);
    revng_assert(ListIt != RevPostOrderList.end());

    int Iteration = 0;
    while (++ListIt != RevPostOrderList.end() and not WorkList.empty()) {
      if (not WorkList.count(*ListIt))
        continue; // Go to the next node in reverse postorder.

      // Otherwise this node is in the worklist, and we have to analyze it.
      BasicBlockNode<NodeT> *Candidate = *ListIt;
      revng_assert(Candidate != nullptr);

      revng_log(CombLogger, "Analyzing candidate " << Candidate->getNameStr());

      bool AllPredAreVisited = std::all_of(Candidate->predecessors().begin(),
                                           Candidate->predecessors().end(),
                                           [&Visited](auto *Pred) {
                                             return Visited.count(Pred);
                                           });
      WorkList.erase(Candidate);
      Visited.insert(Candidate);

      // Comb end flag, which is useful to understand if the dummies we will
      // insert will need to substitute the current postdominator.
      bool IsCombEnd = CombEndSetIt->second.count(Candidate);

      if (not IsCombEnd) {
        for (auto &[Successor, EdgeLabel] : Candidate->labeled_successors()) {
          WorkList.insert(Successor);
        }
      } else {
        revng_log(CombLogger,
                  Candidate->getNameStr()
                    << " is Post-Dominator of " << Conditional->getNameStr());
      }

      if (AllPredAreVisited)
        continue; // Go to the next node in reverse postorder.

      if (IsCombEnd) {
        revng_assert(Candidate->predecessor_size() > 1);

        llvm::SmallVector<BasicBlockNode<NodeT> *, 8> NewDummyPredecessors;
        revng_log(CombLogger, "Current predecessors are:");
        for (BasicBlockNode<NodeT> *Predecessor : Candidate->predecessors()) {
          revng_log(CombLogger, Predecessor->getNameStr());
          if (Visited.count(Predecessor))
            NewDummyPredecessors.push_back(Predecessor);
        }

        // We don't insert the dummy, because it would be a dummy with a
        // single predecessor and a single successor, which is pointless.
        if (NewDummyPredecessors.size() < 2)
          continue;

        revng_log(CombLogger,
                  "Inserting a dummy node for " << Candidate->getNameStr());

        // Insert a dummy node. Notice, this is guaranteed not to be trivial
        // because it will have more than one predecessor.
        BasicBlockNode<NodeT> *Dummy = Graph.addArtificialNode();

        for (BasicBlockNode<NodeT> *Predecessor : NewDummyPredecessors) {
          revng_log(CombLogger,
                    "Moving edge from predecessor " << Predecessor->getNameStr()
                                                    << " to dummy");
          moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate), Dummy);
        }

        addPlainEdge(EdgeDescriptor(Dummy, Candidate));

        // Remove from the visited set the node which triggered the creation
        // of the dummy nodes, because we're not really analyzing it now,
        // since we're just inserting the dummy. For the same reason we
        // re-insert it in the WorkList, otherwise it will be skipped at the
        // next iteration.
        Visited.erase(Candidate);

        // The new dummy node does not lead back to any original node, for
        // this reason we need to insert a new entry in the
        // `CloneToOriginalMap`.
        CloneToOriginalMap[Dummy] = Dummy;

        revng_log(CombLogger,
                  "Update conditional post-dominator. Old: "
                    << CombEndIt->second->getNameStr()
                    << " New: " << Dummy->getNameStr());

        // The dummy is now the node that ends the combing for Conditional.
        CombEndIt->second = Dummy;
        CombEndSetIt = NodesEquivalenceClass.insert({ Dummy, { Dummy } }).first;

        // Mark the dummy to explore.
        WorkList.insert(Dummy);

        // Insert the dummy nodes in the reverse post order list. The
        // insertion order is particularly relevant, because we have added a
        // dummy that now post-dominates the region starting from Conditional,
        // while Candidate (which is the post-dominator of Conditional here),
        // is a successor of Dummy. Hence Dummy must come first in reverse
        // post order, otherwise future RPOT visits based on RevPostOrderList
        // might be disrupted.
        auto PrevListIt = std::prev(ListIt);
        RevPostOrderList.insert(ListIt, Dummy);
        ListIt = PrevListIt;

      } else {

        // Duplicate node.
        DuplicationCounter++;
        revng_log(CombLogger, "Duplicating node " << Candidate->getNameStr());

        BasicBlockNode<NodeT> *Duplicated = Graph.cloneNode(*Candidate);
        revng_assert(Duplicated != nullptr);

        // Initialize the successors of the Duplicated node with the same
        // successors of Candidate node
        for (const auto &[Succ, Label] : Candidate->labeled_successors())
          addEdge(EdgeDescriptor(Duplicated, Succ), Label);

        bool Same = Candidate->successor_size() == Duplicated->successor_size();
        revng_assert(Same);

        // Move Candidate's predecessors that have not been visited yet, so that
        // they become predecessors of Duplicated
        BasicBlockNodeTVect NotVisitedPredecessors;
        for (BasicBlockNode<NodeT> *Predecessor : Candidate->predecessors())
          if (not Visited.count(Predecessor))
            NotVisitedPredecessors.push_back(Predecessor);

        for (BasicBlockNode<NodeT> *Predecessor : NotVisitedPredecessors) {
          moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate), Duplicated);
          revng_log(CombLogger,
                    "Moving edge from predecessor "
                      << Predecessor->getNameStr() << " to "
                      << Duplicated->getNameStr());
        }

        if (CombLogger.isEnabled()) {
          Graph.dumpDotOnFile("inflates",
                              FunctionName,
                              "Region-" + RegionName + "-before-purge-dummies-"
                                + Conditional->getNameStr() + "-"
                                + std::to_string(Iteration));
        }

        BasicBlockNode<NodeT> *OriginalNode = CloneToOriginalMap.at(Candidate);

        bool AreDummies = Candidate->isEmpty();
        revng_assert(AreDummies == Duplicated->isEmpty());
        if (AreDummies) {
          revng_log(CombLogger, "Duplicated is dummy");
          unsigned CandidateSuccSize = Candidate->successor_size();
          unsigned DuplicatedSuccSize = Duplicated->successor_size();

          revng_assert(CandidateSuccSize < 2 and DuplicatedSuccSize < 2);
          revng_assert(CandidateSuccSize == DuplicatedSuccSize);

          bool CInl = Candidate->labeled_successors().begin()->second.Inlined;
          bool DInl = Duplicated->labeled_successors().begin()->second.Inlined;

          revng_assert(CandidateSuccSize == 0 or CInl == false);
          revng_assert(DuplicatedSuccSize == 0 or DInl == false);

          // Notice: after this call Duplicated is invalid if the call returns
          // true, meaning that dereferncing it is bad. You can still use it as
          // a key or value into maps though.
          if (not purgeIfTrivialDummy(Duplicated)) {
            // Add the cloned node in the equivalence class of the original
            // node.
            CloneToOriginalMap[Duplicated] = OriginalNode;
            NodesEquivalenceClass.at(OriginalNode).insert(Duplicated);

            // If it wasn't purged, insert the cloned node in the reverse post
            // order list. Here the order is not strictly relevant, because
            // there is no strict relationship between Candidate and Duplicated.
            RevPostOrderList.insert(ListIt, Duplicated);
          } else {
            revng_log(CombLogger, "Duplicated is trivial");
          }

          // The duplication process divides the edges incoming to Candidate,
          // and it moves some of them to Duplicated. If Candidate is a dummy
          // node, this process may make it trivial. In that case we want to
          // remove it.

          // Notice: after this call Candidate is invalid if the call returns
          // true, meaning that dereferncing it is bad. You can still use it as
          // a key or value into maps though.
          if (purgeIfTrivialDummy(Candidate)) {
            revng_log(CombLogger, "Candidate is now trivial");
            CloneToOriginalMap.erase(Candidate);
            NodesEquivalenceClass.at(OriginalNode).erase(Candidate);
            Visited.erase(Candidate);
            // Erase Candidate from the post order list, but update ListIt so
            // that after the removal it points to the element that was
            // previously before Candidate. In this way, at the next iteration
            // of the loop on RevPostOrderList we go on with the element that
            // was right after Candidate before its removal.
            auto PrevListIt = std::prev(ListIt);
            RevPostOrderList.erase(ListIt);
            ListIt = PrevListIt;
          }

        } else {
          revng_log(CombLogger, "Duplicated is not dummy");

          // Add the cloned node in the equivalence class of the original node.
          CloneToOriginalMap[Duplicated] = OriginalNode;
          NodesEquivalenceClass.at(OriginalNode).insert(Duplicated);

          // If the node we are duplicating is a conditional node, add it to the
          // vector of ConditionalNodes. Remember, that the ConditionalNodes
          // vector is ordered in reverse-post-order.
          // Pushing back into ConditionalNodes always preserves the property
          // that ConditionalNodes is sorted in reverse post-order.
          if (auto It = ConditionalToCombEnd.find(Candidate);
              It != ConditionalToCombEnd.end()) {
            ConditionalToCombEnd[Duplicated] = It->second;
            ConditionalNodes.push_back(Duplicated);
          }

          // Insert the cloned node in the reverse post order list, right before
          // the Candidate. This is not important right now, because we don't
          // add it to the WorkList. It will become important if whenever
          // Duplicated is reached with a traversal based on RevPostOrderList
          // starting from a different Conditional.
          // In this sense, it's not really important to insert Duplicated
          // before or after Candidate, since they have no strict relationship
          // in the reverse post order.
          RevPostOrderList.insert(ListIt, Duplicated);
        }
      }

      if (CombLogger.isEnabled()) {
        Graph.dumpDotOnFile("inflates",
                            FunctionName,
                            "Region-" + RegionName + "-conditional-"
                              + Conditional->getNameStr() + "-"
                              + std::to_string(Iteration));
      }
      Iteration++;
    }

    revng_log(CombLogger,
              "Finished looking at conditional: " << Conditional->getNameStr());
  }

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("inflates",
                        FunctionName,
                        "Region-" + RegionName
                          + "-before-final-inflate-cleanup");
  }

  // Purge extra dummy nodes introduced.
  purgeTrivialDummies();

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("inflates",
                        FunctionName,
                        "Region-" + RegionName + "-after-combing");
  }
}

template<class NodeT>
inline void RegionCFG<NodeT>::removeNotReachables() {

  // Remove nodes that have no predecessors (nodes that are the result of node
  // cloning and that remains dandling around).
  bool Difference = true;
  while (Difference) {
    Difference = false;
    BasicBlockNode<NodeT> *Entry = &getEntryNode();
    for (auto It = begin(); It != end(); It++) {
      if ((Entry != *It and (*It)->predecessor_size() == 0)) {

        removeNode(*It);
        Difference = true;
        break;
      }
    }
  }
}

template<class NodeT>
inline void
RegionCFG<NodeT>::removeNotReachables(std::vector<MetaRegion<NodeT> *> &MS) {

  // Remove nodes that have no predecessors (nodes that are the result of node
  // cloning and that remains dandling around).
  bool Difference = true;
  while (Difference) {
    Difference = false;
    BasicBlockNode<NodeT> *Entry = &getEntryNode();
    for (auto It = begin(); It != end(); It++) {
      if ((Entry != *It and (*It)->predecessor_size() == 0)) {
        for (MetaRegion<NodeT> *M : MS) {
          M->removeNode(*It);
        }
        removeNode(*It);
        Difference = true;
        break;
      }
    }
  }
}

template<class NodeT>
inline bool RegionCFG<NodeT>::isDAG() {
  for (llvm::scc_iterator<RegionCFG<NodeT> *> I = llvm::scc_begin(this),
                                              IE = llvm::scc_end(this);
       I != IE;
       ++I) {
    const std::vector<BasicBlockNode<NodeT> *> &SCC = *I;
    if (SCC.size() != 1) {
      return false;
    } else {
      BasicBlockNode<NodeT> *Node = SCC[0];
      for (BasicBlockNode<NodeT> *Successor : Node->successors()) {
        if (Successor == Node) {
          return false;
        }
      }
    }
  }

  return true;
}

template<class NodeT>
inline bool
RegionCFG<NodeT>::isTopologicallyEquivalent(RegionCFG &Other) const {

  // The algorithm inspects in a depth first fashion the two graphs, and check
  // that they are topologically equivalent. Take care that this function may
  // return true if there are nodes not reachable from the entry node.

  // Early failure if the number of nodes composing the two CFG is different.
  if (size() != Other.size()) {
    return false;
  }

  // Retrieve the entry nodes of the two `RegionCFG` under analysis.
  BasicBlockNode<NodeT> &Entry = getEntryNode();
  BasicBlockNode<NodeT> &OtherEntry = Other.getEntryNode();

  // Check that the only node without predecessors is the entry node.
  for (const BasicBlockNode<NodeT> *Node : nodes()) {
    if (Node != &Entry and Node->predecessor_size() == 0) {
      return false;
    }
  }

  // Check that the only node without predecessors is the entry node.
  for (const BasicBlockNode<NodeT> *Node : Other.nodes()) {
    if (Node != &OtherEntry and Node->predecessor_size() == 0) {
      return false;
    }
  }

  // Call to a `BasicBlockNode` method which does a deep and recursive
  // comparison of a node and its successors.
  return Entry.isEquivalentTo(&OtherEntry);
}

template<class NodeT>
inline void RegionCFG<NodeT>::weave() {

  // Check that we are in a valid state of the graph.
  revng_assert(isDAG());

  // Collect useful objects.
  RegionCFG<NodeT> &Graph = *this;

  IFPDT.recalculate(Graph);

  // Iterate over all the nodes in post order.
  BBNodeT *Entry = &getEntryNode();
  for (BBNodeT *Switch : post_order(Entry)) {

    if (not Switch->isDispatcher() and not isASwitch(Switch))
      continue;

    // If we find a switch node we can start the weaving analysis.
    if (Switch->successor_size() > 2) {

      if (CombLogger.isEnabled()) {
        CombLogger << "Looking at switch node: " << Switch->getName() << "\n";
        dumpDotOnFile("weaves",
                      FunctionName,
                      "Weaving-region-" + RegionName + "-debug");
      }

      // Collect the case nodes of the switch.
      BasicBlockNodeTSet CaseSet;
      for (BBNodeT *Successor : Switch->successors())
        CaseSet.insert(Successor);

      // Find the postdominator of the switch.
      BBNodeT *PostDom = IFPDT[Switch]->getIDom()->getBlock();

      // Iterate over all the nodes "in the body" of the switch in reverse post
      // order.
      llvm::SmallPtrSet<BBNodeT *, 1> PostDomSet;
      if (nullptr != PostDom)
        PostDomSet.insert(PostDom);
      ReversePostOrderTraversalExt RPOT(EFGT<BBNodeT *>(Switch), PostDomSet);

      revng_log(CombLogger,
                "Dumping the candidates that may initiate weaving:");

      for (BBNodeT *RPOTBB : RPOT) {
        // We expect to never reach the PostDom
        revng_assert(RPOTBB != PostDom);

        // Skip the switch
        if (RPOTBB == Switch)
          continue;

        revng_log(CombLogger, RPOTBB->getName());

        BasicBlockNodeTVect PostDominatedCases;
        for (BBNodeT *Case : CaseSet)
          if (IFPDT.dominates(RPOTBB, Case))
            PostDominatedCases.push_back(Case);

        // Criterion to check if we need to perform the weaving. Specifically,
        // we need to perform a weaving if we find a node (between the switch
        // and its postdominator) that postdominates more than 1 of the cases.
        // Note: it cannot postdominate not all of them, otherwise it would be
        // the immediate postdominator of the switch, that we have explicitly
        // excluded.
        auto NumPostDominatedCases = PostDominatedCases.size();
        revng_assert(NumPostDominatedCases != CaseSet.size());
        if (NumPostDominatedCases > 1U) {

          // Create the new sub-switch node.
          BasicBlockNodeT *NewSwitch = nullptr;
          if (Switch->isDispatcher()) {
            NewSwitch = addDispatcher(Switch->getNameStr() + " weaved");
          } else if (Switch->isCode()) {
            NewSwitch = addNode(Switch->getOriginalNode(),
                                Switch->getNameStr() + " weaved");
          } else {
            revng_unreachable("unexpected switch");
          }
          revng_assert(nullptr != NewSwitch);
          revng_assert(not NewSwitch->successor_size());
          NewSwitch->setWeaved(true);

          using edge_label_t = typename BasicBlockNodeT::edge_label_t;
          edge_label_t Labels;
          bool WeavingDefault = false;

          // Iterate over all the case nodes that we found, moving all the
          // necessary edges and updating the IFPDT.
          // Also, collect all the case labels of the cases we're weaving.
          for (BasicBlockNodeT *Case : PostDominatedCases) {

            auto LabeledEdge = extractLabeledEdge(EdgeDescriptor(Switch, Case));
            IFPDT.deleteEdge(Switch, Case);

            auto &EdgeInfo = LabeledEdge.second;
            // If we find an edge without case labels, that's the default.
            if (EdgeInfo.Labels.empty()) {
              revng_assert(WeavingDefault == false);
              WeavingDefault = true;
              Labels = {};
            }

            // If we're weaving the default, we don't care about the exact case
            // labels, because the weaved switch will become the default of the
            // original switch.
            if (not WeavingDefault)
              Labels.insert(EdgeInfo.Labels.begin(), EdgeInfo.Labels.end());

            addEdge(EdgeDescriptor(NewSwitch, Case), EdgeInfo);
            IFPDT.insertEdge(NewSwitch, Case);

            CaseSet.erase(Case);
          }

          CaseSet.insert(NewSwitch);

          // Connect the old switch to the new one and update the IFPDT.
          // Use the collected labels to mark the new edge from the original
          // switch to the weaved switch.
          using EdgeInfo = typename BasicBlockNodeT::EdgeInfo;
          EdgeInfo EI = { Labels, false };

          addEdge(EdgeDescriptor(Switch, NewSwitch), EI);
          IFPDT.insertEdge(Switch, NewSwitch);
        }
      }
    }
  }

  DT.recalculate(Graph);
}

template<class NodeT>
inline void RegionCFG<NodeT>::markUnexpectedAndAnyPCAsInlined() {

  llvm::SmallPtrSet<BBNodeT *, 8> UnexpectedPCS;

  for (BBNodeT *UnexpectedPC : *this) {
    if (UnexpectedPC->isCode()) {
      llvm::BasicBlock *BB = UnexpectedPC->getOriginalNode();
      BlockType::Values BBType = GeneratedCodeBasicInfo::getType(BB);
      if (BBType == BlockType::UnexpectedPCBlock
          or BBType == BlockType::AnyPCBlock) {
        UnexpectedPCS.insert(UnexpectedPC);
      }
    }
  }

  for (BBNodeT *UnexpectedPC : UnexpectedPCS) {

    BasicBlockNodeTVect Predecessors;
    for (BBNodeT *Pred : UnexpectedPC->predecessors()) {
      Predecessors.push_back(Pred);
      markEdgeInlined(EdgeDescriptor(Pred, UnexpectedPC));
    }

    if (Predecessors.size() > 1) {
      for (BBNodeT *Pred : llvm::drop_begin(Predecessors, 1)) {
        BBNodeT *NewUnexpectedPC = cloneNode(*UnexpectedPC);
        moveEdgeTarget({ Pred, UnexpectedPC }, NewUnexpectedPC);
      }
    }
  }
}

#endif // REVNGC_RESTRUCTURE_CFG_REGIONCFGTREEIMPL_H
