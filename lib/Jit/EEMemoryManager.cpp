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
  // TODO: ColdCodeBlock is not currently used.
  return this->HotCodeBlock;
}

uint8_t *EEMemoryManager::getCodeSection() {
  // TODO: ColdCodeBlock is not currently used.
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
  unsigned int Offset = 0;
  if ((this->Context->Flags & CORJIT_FLG_PREJIT) != 0) {
    // In ngen scenario ReadOnlyDataBlock will have a 16 bytes alignment in the
    // image but the memory block we get may not have a 16 bytes alignment. We
    // calculate alignment padding based on the image alignment of
    // ReadOnlyDataBlock.
    assert(Alignment <= 16);
    Offset = ((uint64_t)ReadOnlyDataUnallocated - (uint64_t)ReadOnlyDataBlock) %
             Alignment;
  } else {
    Offset = ((uint64_t)ReadOnlyDataUnallocated) % Alignment;
  }
  if (Offset > 0) {
    ReadOnlyDataUnallocated += Alignment - Offset;
  }

  // There are multiple read-only sections, so we need to keep
  // track of the current allocation point in the read-only memory region.
  uint8_t *Result = ReadOnlyDataUnallocated;
  ReadOnlyDataUnallocated += Size;

  // Make sure we are not allocating more than we expected to.
  assert(ReadOnlyDataUnallocated <=
         (ReadOnlyDataBlock + this->Context->ReadOnlyDataSize));

  if (SectionName.equals(".llvm_stackmaps")) {
    assert((this->StackMapBlock == nullptr) &&
           "Unexpected second Stackmap Section");

    this->Context->StackMapSize = Size;
    this->StackMapBlock = Result;
  }

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
  uintptr_t HotCodeSize = CodeSize;
  uintptr_t ColdCodeSize = 0;

  // We still need to allocate space for the RO data here too, because
  // LLVM's dynamic loader does not know where the EE's reservation was made.
  // So this gives the dynamic loader room to copy the RO sections, and later
  // the EE will copy from there to the place it really keeps unwind data.

  uintptr_t ReadOnlyDataSize = DataSizeRO;
  uint32_t ExceptionCount = 0;

  // Remap alignment to the EE notion of alignment.
  // Conservatively request 16 bytes alignment for RODataBlock since one of the
  // .rdata sections may be 16 bytes aligned (e.g., a floating-point constant
  // pool.)
  CorJitAllocMemFlag Flag =
      CorJitAllocMemFlag::CORJIT_ALLOCMEM_DEFAULT_CODE_ALIGN |
      CorJitAllocMemFlag::CORJIT_ALLOCMEM_FLG_RODATA_16BYTE_ALIGN;

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

  // Addr points to an UNWIND_INFO structure:
  // typedef struct _UNWIND_INFO {
  //  UCHAR Version : 3;
  //  UCHAR Flags : 5;
  //  UCHAR SizeOfProlog;
  //  UCHAR CountOfUnwindCodes;
  //  UCHAR FrameRegister : 4;
  //  UCHAR FrameOffset : 4;
  //  UNWIND_CODE UnwindCode[1];
  //} UNWIND_INFO, *PUNWIND_INFO;

  // Size passed to this method includes the size of a padding UNWIND_CODE entry
  // so that the number of elements of the UnwindCode array is even. Also, if
  // CountOfUnwindCodes is 0, Size includes a 4 byte padding.
  // Jit interface expects the size reported to it not to include the padding
  // entries. Calculate the unpadded size.
  const uint32_t OffsetOfCountOfUnwindCodesField = 2;
  UCHAR CountOfUnwindCodes = Addr[OffsetOfCountOfUnwindCodesField];
  const size_t SizeOfUnwindCode = 2;
  size_t SizeWithoutPadding =
      CountOfUnwindCodes == 0
          ? Size - 4
          : Size - (CountOfUnwindCodes & 1) * SizeOfUnwindCode;

#if !defined(NDEBUG)
  // Check that we actually have only one function with unwind info
  // within the module.
  const Module &M = *LLILCJit::TheJit->getLLILCJitContext()->CurrentModule;
  size_t NumFunctionsWithUnwindInfo = 0;
  for (const Function &F : M) {
    if (!F.isDeclaration() && !F.hasFnAttribute(Attribute::NoUnwind)) {
      NumFunctionsWithUnwindInfo++;
    }
  }

  assert(NumFunctionsWithUnwindInfo == 1);
#endif

  // Assume this unwind covers the entire method. Later when
  // we have multiple unwind regions we'll need something more clever.

  uint32_t StartOffset = 0;
  uint32_t EndOffset = this->Context->HotCodeSize;
  this->Context->JitInfo->allocUnwindInfo(
      this->HotCodeBlock, nullptr, StartOffset, EndOffset, SizeWithoutPadding,
      (BYTE *)LoadAddr, CorJitFuncKind::CORJIT_FUNC_ROOT);
}

void EEMemoryManager::deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                                         size_t Size) {}

EEMemoryManager::~EEMemoryManager() {
  // nothing yet.
}

} // namespace llvm
