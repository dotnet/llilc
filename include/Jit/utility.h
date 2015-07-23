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

#include "cor.h"
#include "utility.h"

#include "llvm/Support/Atomic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Config/llvm-config.h"

// The MethodID.NumArgs field may hold either of 2 values:
//   Empty   => the input string was all-blank, or empty.
//   AnyArgs => the pattern did not specify any constraint on the number of
//              arguments.  Eg: "foo" as opposed to "foo()", "foo(3)", etc

enum MethodIDState { Empty = -7, AnyArgs = -1 };

/// \brief MethodID struct represents a Method Signature.
///
/// MethodIDs are held in a MethodSet and are used to decide which
/// methods should be compiled when running as an "alt" JIT.

class MethodID {
public:
  /// Class name.
  std::unique_ptr<std::string> ClassName;
  /// Method Name
  std::unique_ptr<std::string> MethodName;
  ///  Number of method arguments.  MethodIDState::AnyArgs => not specified
  int NumArgs;

  /// Default constructor.
  MethodID()
      : ClassName(nullptr), MethodName(nullptr),
        NumArgs(MethodIDState::AnyArgs) {}

  /// Copy constructor.
  MethodID(const MethodID &Other) {
    if (Other.ClassName) {
      ClassName = llvm::make_unique<std::string>(*Other.ClassName);
    } else {
      ClassName = nullptr;
    }
    if (Other.MethodName) {
      MethodName = llvm::make_unique<std::string>(*Other.MethodName);
    } else {
      MethodName = nullptr;
    }
    this->NumArgs = Other.NumArgs;
  }

  /// Copy assignment operator.

  MethodID &operator=(const MethodID &Other) {
    if (this == &Other)
      return *this;
    if (Other.ClassName) {
      ClassName = llvm::make_unique<std::string>(*Other.ClassName);
    } else {
      ClassName = nullptr;
    }
    if (Other.MethodName) {
      MethodName = llvm::make_unique<std::string>(*Other.MethodName);
    } else {
      MethodName = nullptr;
    }
    NumArgs = Other.NumArgs;
    return *this;
  }

  /// Parse a string into a MethodID.
  /// Parse string S, starting at index I, into a MethodID.  The string can be
  /// any of the following patterns:
  /// *   => any method.
  /// M   => method M, with any number of arguments.
  /// C:M => method M in class C.
  ///
  /// C can be a simple class name, or a namespace.classname.  For example,
  /// "Sort" or "System.Array:Sort".  (The dots in a namespace are ignored).
  ///
  /// M can be followed by the pattern (number) to specify its number of
  /// arguments.  For example, .ctor(0) or Adjust(2)
  ///
  /// If the parse encounters an invalid pattern, we return nullptr.
  ///
  /// In actual use, S may consist of several such patterns, separated by one
  /// or more spaces.  Therefore, leading spaces are skipped, but subsequent
  /// space is significant.
  ///
  //  Eg: "  foo  Ns.MyClass:bar System.Array.Sort " holds 3 patterns.

  static std::unique_ptr<MethodID> parse(const std::string &S, size_t &I);

  // Scan the string S, starting at index I, for the next lexical token.
  // A token is terminated by any of {space, end-of-string, :, (, )}.
  //
  // Return the token, or empty string if none found.  Update I to index
  // the character just past the end of the token.

  std::string scan(const std::string &S, size_t &I);

  // Parse string S, starting at index I, to determine how many arguments
  // the function specifies.  On entry, S[I] holds the character '('.

  int parseArgs(const std::string &S, size_t &I);
};

/// \brief MethodSet comprises a set of MethodID objects
///
/// MethodSet specifies the methods to compile with the "alt" JIT.

class MethodSet {
public:
  /// Test if the MethodSet is empty.
  /// \returns true if MethodSet is empty.

  bool isEmpty() {
    assert(this->isInitialized());
    return (this->MethodIDList->size() == 0);
  }

  /// Test whether specified method is matched in current MethodSet.
  /// \param MethodName        Name of method
  /// \param ClassName         Encapsulating namespace+class of the method.
  /// \param sig               Pointer to method signature.
  /// \return true if Method is contained in Set.

  bool contains(const char *MethodName, const char *ClassName,
                PCCOR_SIGNATURE Sig);

  /// Initialize a new MethodSet - once only.
  void init(std::unique_ptr<std::string> ConfigValue);

  /// Check whether current MethodSet has been initialized.
  bool isInitialized() { return MethodIDList != nullptr; }

  /// \brief Parse the string S into one or more MethodIDs, and insert
  /// them into current MethodSet.
  void insert(std::unique_ptr<std::string> S);

private:
  /// Internal initialized flag.  This should only be accessed via interlocked
  /// exchange operations, since several methods may be JIT'ing concurrently.
  uint32_t Initialized = 0;

  /// MethodSigList implementing the set.
  std::list<MethodID> *MethodIDList = nullptr;
};

/// \brief Class implementing miscellaneous conversion functions.
class Convert {
public:
  /// Convert a UTF-16 string to a UTF-8 string.
  static std::unique_ptr<std::string> utf16ToUtf8(const char16_t *WideStr);
};

#endif // UTILITY_H
