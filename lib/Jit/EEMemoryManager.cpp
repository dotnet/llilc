//===---- lib/Jit/EEMemoryManager.cpp ---------------------------*- C++ -*-===//
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
/// \brief Implementation of the memory manager interface to the EE.
///
//===----------------------------------------------------------------------===//

#include "EEMemoryManager.h"
#include "jitpch.h"
#include "LLILCJit.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/Debug.h"
#include <string>

namespace llvm {

uint8_t *EEMemoryManager::allocateCodeSection(uintptr_t Size,
                                              unsigned int Alignment,
                                              unsigned int SectionID,
                                              StringRef SectionName) {
  // ColdCodeBlock is not currently used.
  return this->HotCodeBlock;
}

uint8_t *EEMemoryManager::allocateDataSection(uintptr_t Size,
                                              unsigned int Alignment,
                                              unsigned int SectionID,
                                              StringRef SectionName,
                                              bool IsReadOnly) {
  // We don't expect to see RW data requests.
  assert(IsReadOnly);

  // Pad for alignment needs.
  unsigned int Offset = ((uint64_t)ReadOnlyDataUnallocated) % Alignment;
  if (Offset > 0) {
    ReadOnlyDataUnallocated += Alignment - Offset;
  }
  assert((((uint64_t)ReadOnlyDataUnallocated) % Alignment) == 0);

  // There are multiple read-only sections, so we need to keep
  // track of the current allocation point in the read-only memory region.
  uint8_t *Result = ReadOnlyDataUnallocated;
  ReadOnlyDataUnallocated += Size;

  // Make sure we are not allocating more than we expected to.
  assert(ReadOnlyDataUnallocated <=
         (ReadOnlyDataBlock + this->Context->ReadOnlyDataSize));

  return Result;
}

bool EEMemoryManager::finalizeMemory(std::string *ErrMsg) {
  // Do nothing
  return true;
}

void EEMemoryManager::reserveAllocationSpace(uintptr_t CodeSize,
                                             uintptr_t DataSizeRO,
                                             uintptr_t DataSizeRW) {
  // Assume for now all RO data is unwind related. We really only
  // need to reserve space for .xdata here but without altering
  // the pattern of callbacks we can't easily separate .xdata out from
  // other read-only things like .rdata and .pdata.
  this->Context->JitInfo->reserveUnwindInfo(FALSE, FALSE, DataSizeRO);

  // Treat all code for now as "hot section"
  uint32_t HotCodeSize = CodeSize;
  uint32_t ColdCodeSize = 0;

  // We still need to allocate space for the RO data here too, because
  // LLVM's dynamic loader does not know where the EE's reservation was made.
  // So this gives the dyamic loader room to copy the RO sections, and later
  // the EE will copy from there to the place it really keeps unwind data.
  uint32_t ReadOnlyDataSize = DataSizeRO;
  uint32_t ExceptionCount = 0;

  // Remap alignment to the EE notion of alignment
  CorJitAllocMemFlag Flag =
      CorJitAllocMemFlag::CORJIT_ALLOCMEM_DEFAULT_CODE_ALIGN;

  // We allow the amount of RW data to be nonzero to work around the fact that
  // MCJIT will not report a size of zero for any section, even if that
  // section does not, in fact, contain any data. allocateDataSection will
  // catch any RW sections that are actually allocated.

  uint8_t *HotBlock = nullptr;
  uint8_t *ColdBlock = nullptr;
  uint8_t *RODataBlock = nullptr;

  this->Context->JitInfo->allocMem(HotCodeSize, ColdCodeSize, ReadOnlyDataSize,
                                   ExceptionCount, Flag, (void **)&HotBlock,
                                   (void **)&ColdBlock, (void **)&RODataBlock);

  assert(ColdBlock == nullptr);

  this->HotCodeBlock = HotBlock;
  this->ColdCodeBlock = ColdBlock;
  this->ReadOnlyDataBlock = RODataBlock;
  this->ReadOnlyDataUnallocated = RODataBlock;

  this->Context->HotCodeSize = HotCodeSize;
  this->Context->ColdCodeSize = ColdCodeSize;
  this->Context->ReadOnlyDataSize = ReadOnlyDataSize;
}

// This is a callback from the dynamic loading code to actually
// register the .pdata with the runtime.
void EEMemoryManager::registerEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                                       size_t Size) {
  // ColdCodeBlock, i.e. separated code is not supported.
  assert(this->ColdCodeBlock == 0 && "ColdCodeBlock must be zero");

  // Assume this unwind covers the entire method. Later when
  // we have multiple unwind regions we'll need something more clever.
  uint32_t StartOffset = 0;
  uint32_t EndOffset = this->Context->HotCodeSize;
  this->Context->JitInfo->allocUnwindInfo(
      this->HotCodeBlock, nullptr, StartOffset, EndOffset, Size,
      (BYTE *)LoadAddr, CorJitFuncKind::CORJIT_FUNC_ROOT);
}

void EEMemoryManager::deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                                         size_t Size) {}

EEMemoryManager::~EEMemoryManager() {
  // nothing yet.
}

} // namespace llvm
