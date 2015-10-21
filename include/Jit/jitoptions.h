//===----------------- include/Jit/options.h -------------------*- C++ -*-===//
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
/// \brief Declaration of the Options class that encapsulates JIT options
///        extracted from CoreCLR config values.
///
//===----------------------------------------------------------------------===//

#ifndef JITOPTIONS_H
#define JITOPTIONS_H

#include "options.h"

/// \brief The JIT options implementation.
///
/// This class queries the CoreCLR interface to compute the Options flags
/// consumed by the JIT.  The query logic here should only be usable from the
/// base JIT at initialization time of the context.
///
/// This derived class is responsible for filling out the base fields
/// as well as it's own.  The base Options class contains options used in
/// all configurations but includes to query logic to fill them out as that
/// will be client based (JIT or AOT)
///
class JitOptions : public Options {
public:
  /// Construct an JitOptions object based on the passed JitContext.
  JitOptions(LLILCJitContext &JitContext);

  /// Destruct Options object.
  ~JitOptions();

private:
  /// Set current JIT invocation as "AltJit".  This sets up
  /// the JIT to filter based on the AltJit flag contents.
  /// \returns true if runing as the alternate JIT
  static bool queryIsAltJit(LLILCJitContext &JitContext);

  /// \brief Compute dump level for the JIT
  ///
  /// Dump level requested via CLR config for the JIT
  /// \returns Computed DumpLevel
  static ::DumpLevel queryDumpLevel(LLILCJitContext &JitContext);

  /// \brief Set optimization level for the JIT.
  ///
  /// Opt Level based on CLR provided flags and environment.
  /// \returns Computed OptLevel
  static ::OptLevel queryOptLevel(LLILCJitContext &JitContext);

  /// \brief Set UseConservativeGC based on environment variable.
  ///
  /// \returns true if COMPLUS_GCCONSERVATIVE is set in the environment.
  static bool queryUseConservativeGC(LLILCJitContext &JitContext);

  /// \brief Set DoInsertStatepoints
  ///
  /// \returns true if insert statepoints is indicated in the
  ///  environment to model precise GC. (COMPLUS_INSERTSTATEPOINTS)
  static bool queryDoInsertStatepoints(LLILCJitContext &JitContext);

  /// \brief Set DoTailCallOpt based on environment variable.
  ///
  /// \returns true if COMPLUS_TAILCALLOPT is set in the environment.
  static bool queryDoTailCallOpt(LLILCJitContext &JitContext);

  /// \brief Set LogGcInfo based on environment variable.
  ///
  /// \returns true if COMPLUS_JitGCInfoLogging is set in the environment.
  static bool queryLogGcInfo(LLILCJitContext &JitContext);

  /// \brief Set ExecuteHandlers based on envirionment variable.
  ///
  /// \returns true if COMPlus_ExecuteHandlers is set in the environment.
  static bool queryExecuteHandlers(LLILCJitContext &JitContext);

  /// \brief Define set of methods to exclude from LLILC compilation
  ///
  /// \returns true if current method is in that set.
  static bool queryIsExcludeMethod(LLILCJitContext &JitContext);

  /// \brief Define set of methods on which to break.
  ///
  /// \returns true if current method is in that set.
  static bool queryIsBreakMethod(LLILCJitContext &JitContext);

  /// \brief Define set of methods for which to dump MSIL
  ///
  /// \returns true if current method is in that set.
  static bool queryIsMSILDumpMethod(LLILCJitContext &JitContext);

  /// \brief Define set of methods for which to dump LLVM IR.
  ///
  /// \returns true if current method is in that set.
  static bool queryIsLLVMDumpMethod(LLILCJitContext &JitContext);

  /// \brief Define set of methods for which to print code address range.
  ///
  /// \returns true if current method is in that set.
  static bool queryIsCodeRangeMethod(LLILCJitContext &JitContext);

  static bool queryMethodSet(LLILCJitContext &JitContext, MethodSet &TheSet,
                             const char16_t *Name);

  /// \brief Check for non-null non-empty configuration variable.
  ///
  /// \param Name The name of the configuration variable
  /// \returns true if configuration variable is non-null and non-empty.
  static bool queryNonNullNonEmpty(LLILCJitContext &JitContext,
                                   const char16_t *Name);

  /// \brief Set SIMD intrinsics using.
  ///
  /// \returns true if SIMD_INTRINSIC is set in the environment set.
  static bool queryDoSIMDIntrinsic(LLILCJitContext &JitContext);

public:
  bool IsAltJit;        ///< True if running as the alternative JIT.
  bool IsExcludeMethod; ///< True if method is to be excluded.
                        ///< Jit if IsAltJit && !IsExcludeMethod
  bool IsBreakMethod;   ///< True if break requested when compiling this method.
  bool IsMSILDumpMethod;  ///< True if dump of MSIL requested.
  bool IsLLVMDumpMethod;  ///< True if dump of LLVM requested.
  bool IsCodeRangeMethod; ///< True if desired to dump entry address and size.

private:
  static MethodSet AltJitMethodSet;     ///< Singleton AltJit MethodSet.
  static MethodSet AltJitNgenMethodSet; ///< Singleton AltJitNgen MethodSet.
  static MethodSet ExcludeMethodSet;    ///< Methods to exclude from jitting.
  static MethodSet BreakMethodSet;      ///< Methods to break.
  static MethodSet MSILMethodSet;       ///< Methods to dump MSIL.
  static MethodSet LLVMMethodSet;       ///< Methods to dump LLVM IR.
  static MethodSet CodeRangeMethodSet;  ///< Methods to dump code range
};

#endif // JITOPTIONS_H
