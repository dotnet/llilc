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
enum class OptLevel {
  INVALID,
  DEBUG_CODE,   ///< No/Low optimization to preserve debug semantics.
  BLENDED_CODE, ///< Fast code that remains sensitive to code size.
  SMALL_CODE,   ///< Optimized for small size.
  FAST_CODE     ///< Optimized for speed.
};

/// \brief Enum for LLVM IR Dump Level
enum class DumpLevel {
  NODUMP,  ///< Do not dump any LLVM IR or summary.
  SUMMARY, ///< Only dump one line summary per method.
  VERBOSE  ///< Dump full LLVM IR and method summary.
};

// Macro to determine the default behavior of automatically
// detecting tail calls (without the "tail." opcode in MSIL).
#define DEFAULT_TAIL_CALL_OPT 1

/// \brief The JIT options provided via CoreCLR configurations.
///
/// This class exposes the JIT options flags. This interface is passed
/// around to Options consumers to give them access to the results of
/// the ConfigValue queries but no query logic is exposed here.
///
struct Options {
  ::DumpLevel DumpLevel;    ///< Dump level for this JIT invocation.
  ::OptLevel OptLevel;      ///< Optimization level for this JIT invocation.
  bool EnableOptimization;  ///< True iff OptLevel is not debug
  bool UseConservativeGC;   ///< True if the environment is set to use CGC.
  bool DoInsertStatepoints; ///< True if the environment calls for statepoints.
  bool DoTailCallOpt;       ///< Tail call optimization.
  bool LogGcInfo;           ///< Generate GCInfo Translation logs
  bool ExecuteHandlers;     ///< Squelch handler suppression.
  bool DoSIMDIntrinsic;     ///< True if SIMD intrinsic is on.
  unsigned PreferredIntrinsicSIMDVectorLength; ///< Prefer Intrinsic SIMD Vector
  /// Length in bytes.
};
#endif // OPTIONS_H
