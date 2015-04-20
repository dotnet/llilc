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
  /// \returns true if COMPLUS_TAILCALLOPT is set in the environment set.
  static bool queryDoTailCallOpt(LLILCJitContext &JitContext);

public:
  bool IsAltJit; ///< True if running as the alternative JIT.

private:
  static MethodSet AltJitMethodSet;     ///< Singleton AltJit MethodSet.
  static MethodSet AltJitNgenMethodSet; ///< Singleton AltJitNgen MethodSet.
};

#endif // JITOPTIONS_H
