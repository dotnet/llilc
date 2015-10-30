//===------------------- include/Reader/abi.h -------------------*- C++ -*-===//
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
/// \brief Declares the ABI abstraction used when lowering functions to LLVM IR.
///
//===----------------------------------------------------------------------===//

#ifndef _READER_ABI_H_
#define _READER_ABI_H_

/// \brief Information about how a particular argument is passed to a function.
///
/// This class encapsulates information such as whether a parameter is passed
/// by value or by implicit reference, whether it must be coerced to a different
/// type, etc. This information is used when generating call sequences or
/// accessing method parameters/results.
class ABIArgInfo {
public:
  /// \brief Describes how a particular argument is passed to a function.
  enum Kind {
    Direct, ///< Pass the argument directly, optionally coercing it to a
            ///< different type.

    Expand, ///< Pass the argument directly after expanding its contents
            ///< according to the expansion records.

    ZeroExtend, ///< Pass the argument directly with zero-extension.

    SignExtend, ///< Pass the argument directly with sign-extension.

    Indirect, ///< Pass the argument indirectly via a hidden pointer
  };

  /// \brief Describes how a subfield of a value is expanded into an argument.
  struct Expansion {
    llvm::Type *TheType; ///< The type of the expanded subfield.
    unsigned Offset;     ///< The offset of the subfield in the value.
  };

private:
  Kind TheKind; ///< How this argument is to be passed
  union {
    llvm::Type *TheType; ///< The type this argument is to be passed as.
                         ///< Especially relevant for Kind::Direct.

    llvm::SmallVector<Expansion, 2> *Expansions; ///< The expansions used to
                                                 ///< pass this arg.
                                                 ///< Relevant only for
                                                 ///< Kind::Expand.
  };
  uint32_t Index; ///< Index of this argument in the argument list of
                  ///< its containing \p Function. Currently only used by
                  ///< \p ABIMethodSignature.

  ABIArgInfo(const ABIArgInfo &other) = delete;
  ABIArgInfo &operator=(const ABIArgInfo &other) = delete;

  ABIArgInfo(Kind TheKind, llvm::Type *TheType);
  ABIArgInfo(Kind TheKind, llvm::ArrayRef<Expansion> Expansions);

public:
  ABIArgInfo(ABIArgInfo &&other);
  ~ABIArgInfo();

  ABIArgInfo &operator=(ABIArgInfo &&other);

  /// \brief Create an \p ABIIArgInfo value for an argument that is to be
  ///        passed by value with a particular type.
  ///
  /// \param TheType  The type that this argument is passed as.
  ///
  /// \returns An \p ABIArgInfo value describing the argument.
  static ABIArgInfo getDirect(llvm::Type *TheType);

  /// \brief Create an \p ABIIArgInfo value for an argument that is to be
  ///        passed by expansion.
  ///
  /// \param Expansions  The expansion records for this argument.
  ///
  /// \returns An \p ABIArgInfo value describing the argument.
  static ABIArgInfo getExpand(llvm::ArrayRef<Expansion> Expansions);

  /// \brief Create an \p ABIIArgInfo value for an argument that is to be
  ///        passed by value with zero extension.
  ///
  /// \param TheType  The type that this argument is passed as.
  ///
  /// \returns An \p ABIArgInfo value describing the argument.
  static ABIArgInfo getZeroExtend(llvm::Type *TheType);

  /// \brief Create an \p ABIIArgInfo value for an argument that is to be
  ///        passed by value with sign extension.
  ///
  /// \param TheType  The type that this argument is passed as.
  ///
  /// \returns An \p ABIArgInfo value describing the argument.
  static ABIArgInfo getSignExtend(llvm::Type *TheType);

  /// \brief Create an \p ABIIArgInfo value for an argument that is to be
  ///        passed by an implicit reference to a particular type.
  ///
  /// \param TheType  The referent type for this argument.
  ///
  /// \returns An \p ABIArgInfo value describing the argument.
  static ABIArgInfo getIndirect(llvm::Type *TheType);

  /// \brief Empty constructor to allow vectors, data-dependent construction,
  ///        etc.
  ///
  /// Actual values should be created using \p getDirect and \p getIndirect.
  ABIArgInfo() {}

  /// \brief Get the \p Kind that describes how this argument is passed.
  ///
  /// \returns The \p Kind that describes how this argument is passed.
  Kind getKind() const;

  /// \brief Get the type of this argument.
  ///
  /// \returns The type of the argument for direct args or the referent type
  ///          of the argument for indirect args.
  llvm::Type *getType() const;

  /// \brief Get the expansions for this argument.
  ///
  /// \returns The expansion records for this argument. Only valid for expanded
  ///          arguments.
  llvm::ArrayRef<Expansion> getExpansions() const;

  /// \brief Set the index of this argument in its containing argument list.
  ///
  /// \param Index  The index of this argument in its containing agument list.
  void setIndex(uint32_t Index);

  /// \brief Get the index of this argument in its containing argument list.
  ///
  /// \returns The index of this argument in its containing argument list.
  uint32_t getIndex() const;
};

/// \brief Encapsulates an \p llvm::Type* and its signedness.
class ABIType {
private:
  llvm::Type *TheType;        ///< The type of this argument.
  CORINFO_CLASS_HANDLE Class; ///< The class handle for this argument.
  bool IsSigned;              ///< True if the type is a signed integral type.

public:
  /// \brief Creates an \p ABIType with the given type and signedness.
  ///
  /// \param TheType   The \p llvm::Type* for this \p ABIType.
  /// \param Class     The \p CORINFO_CLASS_HANDLE for this \p ABIType.
  /// \param IsSigned  True if this \p ABIType is a signed integral type.
  ABIType(llvm::Type *TheType, CORINFO_CLASS_HANDLE Class, bool IsSigned);

  /// \brief Empty constructor to allow vectors, data-dependent construction,
  ///        etc.
  ABIType() {}

  /// \brief Get the \p llvm::Type* for this \p ABIType.
  ///
  /// \returns The \p llvm::Type* for this \p ABIType.
  llvm::Type *getType() const;

  /// \brief Get the \p CORINFO_CLASS_HANDLE for this \p ABIType.
  ///
  /// \returns the \p CORINFO_CLASS_HANDLE for this \p ABIType.
  CORINFO_CLASS_HANDLE getClass() const;

  /// \brief Get the signedness of this \p ABIType.
  ///
  /// \returns True if this \p ABIType is a signed integral type.
  bool isSigned() const;
};

/// \brief Encapsulautes ABI-specific functionality.
///
/// The \p ABIInfo class provides ABI-specific services. Currently, this is
/// limited to computing the details of how arguments are passed to functions
/// for a given platform.
class ABIInfo {
public:
  /// \brief Gets an \p ABIInfo that corresponds to the target of the given
  ///        \p Module.
  ///
  /// \returns An \p ABIInfo instance. This instance belongs to the caller and
  ///          should be deleted when it is no longer needed.
  static ABIInfo *get(llvm::Module &M);

  /// \brief Computes argument passing information for the target ABI.
  ///
  /// This function is used to determine how the parameters to and result of a
  /// function are passed for the given calling convention and types for the
  /// target ABI.
  ///
  /// \param Context               The \p LLILCJitContext instance that will be
  ///                              used to retrieve extended type information.
  /// \param CC                    The calling convention for the call target.
  /// \param IsManagedCallingConv  True if the callling convention targets
  ///                              managed code.
  /// \param ResultType            The type of the target's result.
  /// \param ArgTypes              The types of the target's arguments.
  /// \param ResultInfo [out]      Argument passing information for the target's
  ///                              result.
  /// \param ArgInfos [out]        Argument passing information for the target's
  ///                              arguments.
  virtual void
  computeSignatureInfo(LLILCJitContext &Context, llvm::CallingConv::ID CC,
                       bool IsManagedCallingConv, ABIType ResultType,
                       llvm::ArrayRef<ABIType> ArgTypes, ABIArgInfo &ResultInfo,
                       std::vector<ABIArgInfo> &ArgInfos) const = 0;

  /// \brief Virtual Destructor
  virtual ~ABIInfo() = default;
};

#endif
