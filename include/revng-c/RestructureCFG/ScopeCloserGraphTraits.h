#pragma once

//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/GraphTraits.h"
#include "llvm/IR/BasicBlock.h"

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

/// Specializes `GraphTraits<llvm::Dashed<T *>>
template<SpecializationOfBasicBlock T>
struct llvm::GraphTraits<llvm::Dashed<T *>> {
public:
  using NodeRef = T *;

public:
  static auto child_begin(NodeRef N) {

    // TODO: here we need to implement the concatenation of the standard
    ///      successors with the successor represented in the `ScopeCloser`
    ///      marker, if present
    // return successors(N).begin();
    // return successors(N)
    // return successors(N).begin()
    /*
    SCMBuilder
    NodeRef ScopeCloserTarget =
    */

    // ScopeCloserMarkerBuilder SCMBuilder(N->getParent());
    // NodeRef ScopeCloserTarget = SCMBuilder.getScopeCloserTarget(N);
    // return llvm::concat<NodeRef>(successors(N), { ScopeCloserTarget
    // }).begin();
    // return llvm::concat<NodeRef>(successors(N), { N }).begin();
    return successors(N).begin();
  }

  static auto child_end(NodeRef N) {

    // TODO: here we need to implement the concatenation of the standard
    //       successors with the successor represented in the `ScopeCloser`
    //       marker, if present
    // return successors(N).end();

    // ScopeCloserMarkerBuilder SCMBuilder(N->getParent());
    // NodeRef ScopeCloserTarget = SCMBuilder.getScopeCloserTarget(N);
    // return llvm::concat<NodeRef>(successors(N), { ScopeCloserTarget }).end();
    // return llvm::concat<NodeRef>(successors(N), { N }).end();
    return successors(N).end();
  }

  // In the implementation for `llvm::BasicBlock *` trait we simply return
  // `this`
  static NodeRef getEntryNode(llvm::Dashed<NodeRef> N) { return N.Graph; }

public:
  // We infer the `ChildIteratorType` directly from the `child_begin` method
  using ChildIteratorType = decltype(child_begin(NodeRef{ nullptr }));
};
