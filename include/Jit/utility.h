//===----------------- include/Jit/utility.h -------------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Declarations of utility classes.
///
//===----------------------------------------------------------------------===//

#ifndef UTILITY_H
#define UTILITY_H

#include <list>
#include <cassert>
#include <memory>

#include "llvm/Support/Atomic.h"
#include "llvm/Config/llvm-config.h"

/// \brief MethodName struct representing a particular method on a type.
///
/// MethodNames are the elements of MethodSet and are used to do filtering of
/// methods when running as an "alt" JIT.
struct MethodName {
  /// Name
  std::unique_ptr<std::string> Name;
  /// Class name
  std::unique_ptr<std::string> ClassName;
  ///  Number of method arguments.
  int NumArgs;

  /// Default constructor.
  MethodName() : Name(nullptr), ClassName(nullptr), NumArgs(0) {}

  /// Copy constructor for move semantics.
  MethodName(MethodName &&M) {
    Name = std::move(M.Name);
    ClassName = std::move(M.ClassName);
    NumArgs = M.NumArgs;
  }
};

/// \brief MethodSet comprising a set of MethodName objects
///
/// Method set containing methods to filter for if running as the "alt" JIT.
class MethodSet {
public:
  /// Test if the MethodSet is empty.
  /// \returns true if MethodSet is empty.
  bool isEmpty() {
    assert(this->isInitialized());

    return (this->MethodList->size() == 0);
  }

  /// Test of MethodSet contains Method.
  /// \param MethodName        Name of the method
  /// \param ClassName         Encapsulating class of the method.
  /// \param sig               Pointer to method signature.
  /// \return true if Method is contained in Set.
  bool contains(const char *MethodName, const char *ClassName,
                PCCOR_SIGNATURE sig);

  void init(std::unique_ptr<std::string> ConfigValue) {
    if (this->isInitialized()) {
      return;
    }

    // ISSUE-TODO:1/16/2015 - Parse config value returned by the CLR
    // into methods if any.

    MethodName M;

    M.Name = std::move(ConfigValue);

    std::list<MethodName> *ML = new std::list<MethodName>;

    ML->push_front(std::move(M));

    // This write should be atomic, delete if we're not the first.

    llvm::sys::cas_flag Value = llvm::sys::CompareAndSwap(
        (llvm::sys::cas_flag *)&(this->Initialized), 0x1, 0x0);

    if (Value != 0x0) {
      delete ML;
    } else {
      this->MethodList = ML;
    }
  }

  bool isInitialized() { return MethodList != nullptr; }

private:
  /// Internal initialized flag.  This should only be accessed via
  /// interlocked exchange operations.
  uint32_t Initialized = 0;
  /// MethodList implementing the set.
  std::list<MethodName> *MethodList = nullptr;
};

/// \brief Class implementing miscellaneous conversion functions.
class Convert {
public:
  /// Convert a UTF-16 string to a UTF-8 string.
  static std::unique_ptr<std::string> utf16ToUtf8(const char16_t *wideStr);
};

#endif // UTILITY_H
