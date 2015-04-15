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

#ifndef OPTIONS_H
#define OPTIONS_H

#include "utility.h"

struct LLILCJitContext;

/// \brief Enum for JIT optimization level.
enum OptLevel {
  INVALID,
  DEBUG_CODE,   ///< No/Low optimization to preserve debug semantics.
  BLENDED_CODE, ///< Fast code that remains sensitive to code size.
  SMALL_CODE,   ///< Optimized for small size.
  FAST_CODE     ///< Optimized for speed.
};

/// \brief Enum for LLVM IR Dump Level
enum LLVMDumpLevel {
  NODUMP,  ///< Do not dump any LLVM IR or summary.
  SUMMARY, ///< Only dump one line summary per method.
  VERBOSE  ///< Dump full LLVM IR and method summary.
};

/// \brief The JIT options provided via CoreCLR configurations.
///
/// This class implements the JIT options flags.  The CoreCLR interface
/// is queried and results are cached in this object.
///
class Options {
public:
  /// Construct an Options object based on the passed JitContext.
  Options(LLILCJitContext *Context);

  /// Destruct Options object.
  ~Options();

  // Initialize object after full Context is established.
  void initialize();

  /// Set current JIT invocation as "AltJit".  This sets up
  /// the JIT to filter based on the AltJit flag contents.
  void setIsAltJit();

  /// \brief Compute dump level for the JIT
  ///
  /// Dump level requested via CLR config for the JIT
  void setDumpLevel();

  /// \brief Set optimization level for the JIT.
  ///
  /// Opt Level based on CLR provided flags and environment.
  void setOptLevel();

public:
  LLILCJitContext *Context; ///< Invocation CLR Execution Engine flags.
  LLVMDumpLevel DumpLevel;  ///< Dump level for this JIT invocation.
  ::OptLevel OptLevel;      ///< Optimization level for this JIT invocation.
  bool IsAltJit;            ///< True if compiling as the alternative JIT.

private:
  static MethodSet AltJitMethodSet;     ///< Singleton AltJit MethodSet.
  static MethodSet AltJitNgenMethodSet; ///< Singleton AltJitNgen MethodSet.
};

#endif // OPTIONS_H
