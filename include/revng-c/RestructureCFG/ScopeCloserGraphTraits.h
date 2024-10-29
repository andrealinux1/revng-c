#pragma once

//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/GraphTraits.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/DOTGraphTraits.h"

#include "revng-c/RestructureCFG/GeneratorIterator.h"

inline cppcoro::generator<llvm::BasicBlock *> increment(llvm::BasicBlock *BB) {

  //  First of all, we return all the standard successors of `BB`
  for (auto *Successor : successors(BB)) {
    co_yield Successor;
  }

  // We then move to returning the additional successor represented by the
  // `ScopeCloser` edge, if present at all
  ScopeCloserMarkerBuilder SCMBuilder(BB->getParent());
  llvm::BasicBlock *ScopeCloserTarget = SCMBuilder.getScopeCloserTarget(BB);
  if (ScopeCloserTarget) {
    co_yield ScopeCloserTarget;
  }
}

#if 0
inline cppcoro::generator<const llvm::BasicBlock *> increment(const llvm::BasicBlock *BB) {

  //  First of all, we return all the standard successors of `BB`
  for (auto *Successor : successors(BB)) {
    co_yield Successor;
  }

  // We then move to returning the additional successor represented by the
  // `ScopeCloser` edge, if present at all
  const llvm::Function *ParentFunction = BB->getParent();
  ScopeCloserMarkerBuilder SCMBuilder(ParentFunction);
  const llvm::BasicBlock *ScopeCloserTarget = SCMBuilder.getScopeCloserTarget(BB);
  if (ScopeCloserTarget) {
    co_yield ScopeCloserTarget;
  }
}
#endif

namespace llvm {

/// This class is used as a marker class to tell the graph iterator to treat the
/// underlying graph considering also the _dashed_ edges needed for, e.g., IDB
template<class GraphType>
struct Dashed {
  const GraphType &Graph;

  inline Dashed(const GraphType &G) : Graph(G) {}
};

} // namespace llvm

// Define a concept which we can use in order to implement `llvm::GraphTraits`
// for the `llvm::Dashed` wrapper class, on something that is similar to an
// `llvm::BasicBlock` (and must be it at least at the beginning).
template<typename T>
concept SpecializationOfBasicBlock = requires {
  std::same_as<llvm::BasicBlock, T>;

  typename llvm::GraphTraits<T *>;
  typename llvm::Inverse<T *>;
};

template<typename T>
concept SpecializationOfFunction = requires {
  std::same_as<llvm::Function, T>;

  typename llvm::GraphTraits<T *>;
  typename llvm::Inverse<T *>;
};

#if 0

/// Specializes `GraphTraits<llvm::Dashed<T *>>
template<SpecializationOfBasicBlock T>
struct llvm::GraphTraits<llvm::Dashed<T *>> {
public:
  using NodeRef = T *;

public:
  static auto child_begin(NodeRef N) {
    return GeneratorIterator<llvm::BasicBlock *>(increment(N));
  }

  static auto child_end(NodeRef N) {
    return GeneratorIterator<llvm::BasicBlock *>();
  }

  // In the implementation for `llvm::BasicBlock *` trait we simply return
  // `this`
  static NodeRef getEntryNode(llvm::Dashed<NodeRef> N) { return N.Graph; }

public:
  // We infer the `ChildIteratorType` directly from the `child_begin` method
  using ChildIteratorType = decltype(child_begin(NodeRef{ nullptr }));

  // Define the iterator for nodes (BasicBlocks)
  //using nodes_iterator = llvm::Function::iterator;
};

#endif

#if 0
template<SpecializationOfFunction T>
struct llvm::GraphTraits<llvm::Dashed<T *>>
  : public llvm::GraphTraits<llvm::Dashed<typename T::NodeRef>> {
public:
  using NodeRef = T::NodeRef;
  using nodes_iterator = T::iterator;

  static NodeRef getEntryNode(T *G) { return G->getEntryNode(); }

  static nodes_iterator nodes_begin(T *G) { return G->nodes().begin(); }
  static nodes_iterator nodes_end(T *G) { return G->nodes().end(); }

  static size_t size(T *G) { return G->size(); }
};

#endif

#if 1

#if 1

/// Specializes `GraphTraits<llvm::Dashed<T *>>
template<>
struct llvm::GraphTraits<llvm::Dashed<llvm::BasicBlock *>> {
public:
  using NodeRef = llvm::BasicBlock *;
  using ChildIteratorType = GeneratorIterator<llvm::BasicBlock *>;

public:
  static ChildIteratorType child_begin(NodeRef N) {
    return GeneratorIterator<llvm::BasicBlock *>(increment(N));
  }

  static ChildIteratorType child_end(NodeRef N) {
    return GeneratorIterator<llvm::BasicBlock *>();
  }

  // In the implementation for `llvm::BasicBlock *` trait we simply return
  // `this`
  static NodeRef getEntryNode(llvm::Dashed<NodeRef> N) { return N.Graph; }

  // static NodeRef getEntryNode(const NodeRef &N) { return N; }

public:
  // We infer the `ChildIteratorType` directly from the `child_begin` method
  // using ChildIteratorType = decltype(child_begin(std::declval<NodeRef>()));
};

#endif

#if 0
template<>
struct llvm::GraphTraits<llvm::Dashed<const llvm::BasicBlock *>> {
public:
  using NodeRef = const llvm::BasicBlock *;
  using ChildIteratorType = llvm::Function::const_iterator;

public:
  static auto child_begin(NodeRef N) {
    return GeneratorIterator<const llvm::BasicBlock *>(increment(N));
  }

  static auto child_end(NodeRef N) {
    return GeneratorIterator<const llvm::BasicBlock *>();
  }

  static NodeRef getEntryNode(llvm::Dashed<NodeRef> N) { return N.Graph; }
};
#endif

template<>
struct llvm::GraphTraits<llvm::Dashed<llvm::Function *>>
  : public llvm::GraphTraits<llvm::Dashed<typename llvm::BasicBlock *>> {
  using NodeRef = llvm::BasicBlock *;
  using nodes_iterator = pointer_iterator<Function::iterator>;

  static NodeRef getEntryNode(llvm::Dashed<llvm::Function *> G) {
    return &G.Graph->getEntryBlock();
  }

  static nodes_iterator nodes_begin(llvm::Dashed<llvm::Function *> G) {
    return nodes_iterator(G.Graph->begin());
  }

  static nodes_iterator nodes_end(llvm::Dashed<llvm::Function *> G) {
    return nodes_iterator(G.Graph->end());
  }

  static size_t size(llvm::Dashed<llvm::Function *> G) {
    return G.Graph->size();
  }
};

#endif

#if 1
// Explicitly instantiate `DOTGraphTraits` for `llvm::Dashed<llvm::BasicBlock
// *>`
template<>
struct llvm::DOTGraphTraits<llvm::Dashed<llvm::BasicBlock *>>
  : public llvm::DefaultDOTGraphTraits {
  using llvm::DefaultDOTGraphTraits::DefaultDOTGraphTraits;
};

#if 0
template<>
struct llvm::DOTGraphTraits<llvm::Dashed<llvm::Function *>>
  : public llvm::DOTGraphTraits<llvm::Dashed<llvm::BasicBlock *>> {
  using llvm::DefaultDOTGraphTraits::DefaultDOTGraphTraits;
};
#endif

#if 1
template<>
struct llvm::DOTGraphTraits<llvm::Dashed<llvm::Function *>>
  : public llvm::DefaultDOTGraphTraits {
  using llvm::DefaultDOTGraphTraits::DefaultDOTGraphTraits;
};
#endif

#endif
