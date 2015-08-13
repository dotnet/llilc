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
  /// \param Allocator The allocator to be used by GcInfo encoder
  /// \param OffsetCorrection FunctionStart - CodeBlockStart difference
  GCInfo(LLILCJitContext *JitCtx, uint8_t *StackMapData,
         GcInfoAllocator *Allocator, size_t OffsetCorrection);

  /// Emit GC Info to the EE using GcInfoEncoder.
  void emitGCInfo();

  /// Destructor -- delete allocated memory
  ~GCInfo();

private:
  void emitGCInfo(const llvm::Function &F);
  void encodeHeader(const llvm::Function &F);
  void encodeLiveness(const llvm::Function &F);
  void emitEncoding();

  bool shouldEmitGCInfo(const llvm::Function &F);
  bool isStackBaseFramePointer(const llvm::Function &F);

  const LLILCJitContext *JitContext;
  const uint8_t *LLVMStackMapData;
  GcInfoEncoder Encoder;

  // The InstructionOffsets reported at Call-sites are with respect to:
  // (1) FunctionEntry in LLVM's StackMap
  // (2) CodeBlockStart in CoreCLR's GcTable
  // OffsetCorrection accounts for the difference:
  // FunctionStart - CodeBlockStart
  //
  // There is typically a difference between the two even in the JIT case
  // (where we emit one function per module) because of some additional
  // code like the gc.statepoint_poll() method.
  size_t OffsetCorrection;

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
