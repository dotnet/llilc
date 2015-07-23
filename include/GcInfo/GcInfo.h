//===---- include/gcinfo/gcinfo.h -------------------------------*- C++ -*-===//
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
/// \brief GCInfo Generator for LLILC
///
//===----------------------------------------------------------------------===//

#ifndef GCINFO_H
#define GCINFO_H

#include "gcinfoencoder.h"
#include "jitpch.h"
#include "LLILCJit.h"

class GcInfoAllocator;
class GcInfoEncoder;

/// \brief This is the translator from LLVM's GC StackMaps
///  to CoreCLR's GcInfo encoding.
class GCInfo {
public:
  /// Construct a GCInfo object
  /// \param JitCtx Context record for the method's jit request.
  /// \param StackMapData A pointer to the .llvm_stackmaps section
  ///        loaded in memory
  /// \param CodeBlkStart Start address of the Code section block
  GCInfo(LLILCJitContext *JitCtx, uint8_t *StackMapData, uint8_t *CodeBlkStart,
         GcInfoAllocator *Allocator);

  /// Emit GC Info to the EE using GcInfoEncoder.
  void emitGCInfo();

  /// Destructor -- delete allocated memory
  ~GCInfo();

private:
  void encodeHeader();
  void encodeLiveness();
  void emitEncoding();

  const LLILCJitContext *JitContext;
  const uint8_t *CodeBlockStart;
  const uint8_t *LLVMStackMapData;
  GcInfoEncoder Encoder;

#if !defined(NDEBUG)
  bool EmitLogs;
#endif // !NDEBUG

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  size_t NumCallSites;
  unsigned *CallSites;
  BYTE *CallSiteSizes;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
};

#endif // GCINFO_H
