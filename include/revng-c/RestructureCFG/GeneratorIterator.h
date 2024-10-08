#pragma once

//
// Copyright rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/IR/BasicBlock.h"

#include "revng/Support/Debug.h"
#include "revng/Support/Generator.h"
#include "revng/Support/IRHelpers.h"

#include "revng-c/RestructureCFG/ScopeCloserUtils.h"

inline cppcoro::generator<llvm::BasicBlock *> increment(llvm::BasicBlock *BB) {

  //  First of all, we return all the standard successors of `BB`
  for (auto *Successor : successors(BB)) {
    co_yield Successor;
  }

  // We then move to returning the additional successor represented by the
  // `ScopeCloser` edge
  // ScopeCloserMarkerBuilder SCMBuilder(BB->getParent());
  // llvm::BasicBlock *ScopeCloserTarget = SCMBuilder.getScopeCloserTarget(BB);
  // co_yield ScopeCloserTarget;
}

template<typename T>
class GeneratorIterator {
public:
  using inner_iterator = cppcoro::generator<T>::iterator;

public:
  bool IsEnd = false;
  mutable bool IsDead = false;
  mutable cppcoro::generator<T> Coroutine;
  inner_iterator Begin;
  inner_iterator End;

  // We assume that `T` is copy constructible and cheap to copy. These
  // assumptions are inherited by the assumptions made on `NodeRef`, since the
  // only use we envision is on those objects.
  T Content;

public:
  GeneratorIterator() : IsEnd(true), IsDead(false) {}

  GeneratorIterator(cppcoro::generator<T> &&Coroutine) :
    IsEnd(false),
    IsDead(false),
    Coroutine(std::move(Coroutine)),
    Begin(this->Coroutine.begin()),
    End(this->Coroutine.end()) {
    updateContent();
  }

  GeneratorIterator(const GeneratorIterator &Other) { *this = Other; }

  GeneratorIterator &operator=(GeneratorIterator &&Other) {
    IsEnd = Other.IsEnd;
    IsDead = Other.IsDead;
    Coroutine = std::move(Other.Coroutine);
    Begin = std::move(Other.Begin);
    End = std::move(Other.End);
    Content = std::move(Other.Content);

    // After invoking the move assignment, we mark the `Other` object as dead
    Other.IsDead = true;

    return *this;
  }

  GeneratorIterator &operator=(const GeneratorIterator &Other) {
    IsEnd = Other.IsEnd;
    IsDead = Other.IsDead;

    // This is a special copy constructor, it is used in the post increment
    // `operator`, in order to create a copy of the `GeneratorIterator` object,
    // which we leave in the `IsDead` state, this object will be just
    // dereferenced (returning the value that we pre-computed into `Content`).
    // Therefore it should not be incremented.

    Begin = Other.Begin;
    End = Other.End;
    Content = Other.Content;

    // TODO: in theory, this copy constructor, should behave like a move
    //       constructor, since the ownership of the Coroutine must be unique.
    //       But if we enable either of the following statements, we assert
    //       during an increment.
    // Other.IsDead = true;
    // this->IsDead = true;
    return *this;
  }

public:
  bool operator==(const GeneratorIterator &Other) const {
    if (IsEnd)
      return Other.Begin == Other.End;
    if (Other.IsEnd)
      return Begin == End;
    else
      return Begin == Other.Begin;
  }

  GeneratorIterator &operator++() {

    // Verify that we are not in the `Dead` state
    revng_assert(not IsDead);

    ++Begin;
    updateContent();
    return *this;
  }

  GeneratorIterator operator++(int) {

    // Verify that we are not in the `Dead` state
    revng_assert(not IsDead);

    GeneratorIterator Ret = *this;
    ++*this;
    return Ret;
  }

  T operator*() const {

    // It is correct that we do not perform a check on the `IsDead` state before
    // returning `Content`, since it is expected that a `GeneratorIterator`
    // object left in a `IsDead` state by, e.g., the copy constructor, can be
    // dereferenced (returning the value in `Content`).
    return Content;
  }

private:
  // Private method to update the `Content` field by dereferencing `Begin`. If
  // we are at the end of the `internal_iterator`, we also set the `IsDead`
  // flag.
  void updateContent() {
    if (Begin != End) {
      Content = *Begin;
    } else {

      // This is probably superfluous, since the `==` operator will already
      // handle the `Begin == End` per se
      IsEnd = true;
    }
  }
};
