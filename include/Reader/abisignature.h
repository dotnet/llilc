//===------------------- include/Reader/abisignature.h ----------*- C++ -*-===//
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
/// \brief Declares ABI signature abstractions used when lowering functions to
///        LLVM IR.
///
//===----------------------------------------------------------------------===//

#ifndef _READER_ABISIGNATURE_H_
#define _READER_ABISIGNATURE_H_

/// \brief Encapsulates ABI-specific argument and result passing information for
///        a particular function signature.
class ABISignature {
protected:
  llvm::Type *FuncResultType; ///< The return type of this function signature.
  ABIArgInfo Result; ///< Describes how the result of the function is passed.
  std::vector<ABIArgInfo> Args; ///< Describes how each argument to the function
                                ///< is passed.

  ABISignature() {}

  /// \brief Fills in argument and result passing information for the given
  ///        function signature.
  ///
  /// \param Signature   The function signature.
  /// \param Reader      The \p GenIR instance that will be used to emit IR.
  /// \param TheABIInfo  The target \p ABIInfo.
  ABISignature(const ReaderCallSignature &Signature, GenIR &Reader,
               const ABIInfo &TheABIInfo);

  /// \brief Returns the number of arguments to the ABI signature.
  ///
  /// \returns The number of arguments to the ABI signature.
  uint32_t getNumABIArgs() const;

public:
  /// \brief Expand a value according to a specific list of expansions.
  ///
  /// \param Reader        The \p GenIR instance that will be used to emit IR.
  /// \param Expansions    The list of expansions to be applied.
  /// \param Source        The value to expand.
  /// \param Values [in]   A slice that will hold the values that result from
  ///                      the expansion.
  /// \param Values [in]   A slice that will hold the types of the values that
  ///                      result from the expansion.
  /// \param IsResult      True if the value being expanded is the result value
  ///                      for a function.
  static void expand(GenIR &Reader,
                     llvm::ArrayRef<ABIArgInfo::Expansion> Expansions,
                     llvm::Value *Source,
                     llvm::MutableArrayRef<llvm::Value *> Values,
                     llvm::MutableArrayRef<llvm::Type *> Types, bool IsResult);

  /// \brief Store a single value from an expanded argument into its
  /// destination.
  ///
  /// \param Reader  The \p GenIR instance that will be used to emit IR.
  /// \param Exp     The expansion information for the given value.
  /// \param Val     The value to be collapsed.
  /// \param Base    The base address of the target value as an i8*.
  static void collapse(GenIR &Reader, const ABIArgInfo::Expansion &Exp,
                       llvm::Value *Val, llvm::Value *Base);

  /// \brief Coerces a value to a particular target type, casting or
  ///        reinterpreting as necessary.
  ///
  /// \param Reader    The \p GenIR instance that will be used to emit IR.
  /// \param TheType   The target type of the coercion.
  /// \param TheValue  The value to coerce.
  ///
  /// \returns A \p Value that represents the result of the coercion.
  static llvm::Value *coerce(GenIR &Reader, llvm::Type *TheType,
                             llvm::Value *TheValue);
};

/// \brief Encapsulates ABI-specific argument and result passing information for
///        a particular call target signature and provides facilities to emit
///        a call to a target with that signature.
class ABICallSignature : public ABISignature {
private:
  const ReaderCallSignature &Signature; ///< The target function signature.

  /// \brief Emits a call to an unmanaged function.
  ///
  /// This method is called by \p emitCall when emitting a call that targets an
  /// unmanaged function. It is responsible for emitting the IR required to
  /// perform any necessary bookkeeping for the GC as well as the call itself.
  /// The arguments must already have been arranged as per the calling
  /// convention and target ABI.
  ///
  /// \param Reader        The \p GenIR instance that will be used to emit IR.
  /// \param Target        The call target.
  /// \oaram MayThrow      True iff the callee may raise an exception.
  /// \param Args          The arguments to the call, arranged as per the
  ///                      calling convention and target ABI.
  /// \param Result [out]  The result of the call, if any.
  ///
  /// \returns The call site corresponding to the unmanaged call.
  llvm::CallSite emitUnmanagedCall(GenIR &Reader, llvm::Value *Target,
                                   bool MayThrow,
                                   llvm::ArrayRef<llvm::Value *> Args,
                                   llvm::Value *&Result) const;

public:
  ABICallSignature(const ReaderCallSignature &Signature, GenIR &Reader,
                   const ABIInfo &TheABIInfo);

  /// \brief Emits a call to a function using the argument and result passing
  ///        information for the signature provided when this value was created.
  ///
  /// \param Reader           The \p GenIR instance that will be used to emit
  ///                         IR.
  /// \param Target           The call target.
  /// \param MayThrow         True iff the callee may raise an exception
  /// \param Args             The arguments to the call.
  /// \param IndirectionCell  The indirection cell argument for the call, if
  ///                         any.
  /// \param IsJmp            True iff this is a call for a jmp instruction.
  /// \param CallNode [out]   The call instruction.
  ///
  /// \returns The result of the call to the target.
  llvm::Value *emitCall(GenIR &Reader, llvm::Value *Target, bool MayThrow,
                        llvm::ArrayRef<llvm::Value *> Args,
                        llvm::Value *IndirectionCell, bool IsJmp,
                        llvm::Value **CallNode) const;

  /// \brief Check for an indirect result or indirect argument.
  ///
  /// Determines if expansion of this call might result in references to temps
  /// that live on the caller's stack.
  ///
  /// \returns True if there is an indirect result or indirect argument.
  bool hasIndirectResultOrArg() const;
};

/// \brief Encapsulates ABI-specific argument and result passing information for
///        a particular method signature and provides facilites to create an
//         appropriately-typed function symbol.
class ABIMethodSignature : public ABISignature {
private:
  const ReaderMethodSignature *Signature; ///< The target method signature.

public:
  ABIMethodSignature() {}
  ABIMethodSignature(const ReaderMethodSignature &Signature, GenIR &Reader,
                     const ABIInfo &TheABIInfo);

  /// \brief Creates a function symbol for the method signature provided when
  ///        this vaule was created.
  ///
  /// \param Reader  The \p GenIR instance that will be used to emit IR.
  /// \param M       The module in which this function is to be created.
  ///
  /// \returns The newly-created function symbol.
  llvm::Function *createFunction(GenIR &Reader, llvm::Module &M);

  /// \brief Gets result passing information for this signature.
  ///
  /// \returns Result passing information for this signature.
  const ABIArgInfo &getResultInfo() const;

  /// \brief Gets argument passing information for the runtime argument at the
  ///        given index into its parent \p ReaderMethodSignature.
  ///
  /// \param I  The index of the runtime argument into its parent
  ///           \p ReaderMethodSignature.
  ///
  /// \returns Argument passing information for the argument.
  const ABIArgInfo &getArgumentInfo(uint32_t I) const;
};

#endif
