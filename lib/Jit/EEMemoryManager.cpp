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
#include "llvm/Object/ObjectFile.h"
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

/// Compute the number of bytes of the unwind info at \p XdataPtr.
///
/// \param XdataPtr             Pointer to the (first byte of) the unwind info.
/// \param ReportedSize [out]   The size of the unwind info as it should be
///                             reported to the EE (excludes padding and
///                             runtime function pointer), in bytes.
/// \param TotalSize [out]      The size of the entire unwind info, in bytes.
static void getXdataSize(const uint8_t *XdataPtr, size_t *ReportedSize,
                         size_t *TotalSize) {
  // XdataPtr points to an UNWIND_INFO structure:
  // typedef struct _UNWIND_INFO {
  //  UCHAR Version : 3;
  //  UCHAR Flags : 5;
  //  UCHAR SizeOfProlog;
  //  UCHAR CountOfUnwindCodes;
  //  UCHAR FrameRegister : 4;
  //  UCHAR FrameOffset : 4;
  //  UNWIND_CODE UnwindCode[1];
  //} UNWIND_INFO, *PUNWIND_INFO;

  size_t UnreportedBytes = 0;
  uint8_t NumCodes = XdataPtr[2];
  // There's always a 4-byte header
  size_t ReportedBytes = 4;
  // Each unwind code is 2 bytes
  ReportedBytes += (2 * NumCodes);
  if (ReportedBytes & 2) {
    // The unwind code array is padded to a multiple of 4 bytes
    UnreportedBytes += 2;
  }
  if (XdataPtr[0] != 1) {
    // 4-byte runtime function pointer
    UnreportedBytes += 4;
  }

  size_t TotalBytes = ReportedBytes + UnreportedBytes;
  if (TotalBytes < 8) {
    // Minimum size is 8
    TotalBytes = 8;
  }

  if (ReportedSize != nullptr) {
    *ReportedSize = ReportedBytes;
  }
  if (TotalSize != nullptr) {
    *TotalSize = TotalBytes;
  }
}

void EEMemoryManager::reserveUnwindSpace(const object::ObjectFile &Obj) {
  // The EE needs to be informed for each funclet (and the main function)
  // what the size of its unwind codes will be.  Parse the header info in
  // the xdata section to determine this.
  BOOL IsHandler = FALSE;
  for (const object::SectionRef &Section : Obj.sections()) {
    StringRef SectionName;
    if (!Section.getName(SectionName) && (SectionName == ".xdata")) {
      StringRef Contents;
      if (!Section.getContents(Contents)) {
        const uint8_t *DataPtr =
            reinterpret_cast<const uint8_t *>(Contents.data());
        const uint8_t *DataEnd =
            reinterpret_cast<const uint8_t *>(Contents.end());
        do {
          size_t ReportedByteCount;
          size_t TotalByteCount;
          getXdataSize(DataPtr, &ReportedByteCount, &TotalByteCount);
          // Bit 5 indicates whether this is chained unwind info.  If we saw
          // that here, we'd have wanted to include it with the previous
          // reservation (or we'd be separating cold code).  Since it's not
          // currently emitted, just verify that we don't see it.
          assert((*DataPtr & 0x20) != 0x10 &&
                 "chained unwind info not supported");
          this->Context->JitInfo->reserveUnwindInfo(IsHandler, FALSE,
                                                    ReportedByteCount);
          IsHandler = TRUE;
          DataPtr += TotalByteCount;
          if (DataPtr == DataEnd) {
            break;
          }

          // The next thing should either be the next xdata entry or the
          // sentinel we insert between that and the clause descriptors.
          // If it is the next xdata entry, bits 0-2 will be the version
          // number (currently only version 1 exists).
        } while ((*DataPtr & 0x7) == 1);
        // If we didn't reach the end of the xdata, the next thing should be
        // our sentinel.
        assert(DataPtr == DataEnd || *DataPtr == 0xff && "Malformed .xdata");
      }
    }
  }
}

void EEMemoryManager::reserveAllocationSpace(
    uintptr_t CodeSize, uint32_t CodeAlign, uintptr_t RODataSize,
    uint32_t RODataAlign, uintptr_t RWDataSize, uint32_t RWDataAlign) {
  // Treat all code for now as "hot section"
  uintptr_t HotCodeSize = CodeSize;
  uintptr_t ColdCodeSize = 0;

  // We still need to allocate space for the RO data here too, because
  // LLVM's dynamic loader does not know where the EE's reservation was made.
  // So this gives the dynamic loader room to copy the RO sections, and later
  // the EE will copy from there to the place it really keeps unwind data.

  uintptr_t ReadOnlyDataSize = RODataSize;
  assert(RWDataSize == 0);
  uint32_t ExceptionCount = 0;

  // Remap alignment to the EE notion of alignment.
  assert(CodeAlign <= 16);
  assert(RODataAlign <= 16);
  CorJitAllocMemFlag AlignmentFlag =
      (CodeAlign == 16
           ? CorJitAllocMemFlag::CORJIT_ALLOCMEM_FLG_16BYTE_ALIGN
           : CorJitAllocMemFlag::CORJIT_ALLOCMEM_DEFAULT_CODE_ALIGN);
  if (RODataAlign == 16) {
    AlignmentFlag = AlignmentFlag |
                    CorJitAllocMemFlag::CORJIT_ALLOCMEM_FLG_RODATA_16BYTE_ALIGN;
  }

  // We allow the amount of RW data to be nonzero to work around the fact that
  // MCJIT will not report a size of zero for any section, even if that
  // section does not, in fact, contain any data. allocateDataSection will
  // catch any RW sections that are actually allocated.

  uint8_t *HotBlock = nullptr;
  uint8_t *ColdBlock = nullptr;
  uint8_t *RODataBlock = nullptr;

  this->Context->JitInfo->allocMem(HotCodeSize, ColdCodeSize, ReadOnlyDataSize,
                                   ExceptionCount, AlignmentFlag,
                                   (void **)&HotBlock, (void **)&ColdBlock,
                                   (void **)&RODataBlock);

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

  // The xdata section always starts with the standard xdata entry for the main
  // function.  If there are no funclets, that's the entire section.  If
  // there are funclets, it is followed by the standard xdata entries for each
  // funclet, then:
  // - a sentinel (four bytes, all bits set)
  // - the number of funclets
  // - the offset to the start of each funclet (in lexical order)
  // - the offset to the end of the last funclet
  // - the number of EH clauses
  // - the EH clauses themselves, laid out the same way that the
  //   CORINFO_EH_CLAUSE structure is laid out in memory.

  // The first thing we need to do is report for each function/funclet the
  // size and location of the function/funclet as well as the size and
  // location of its xdata entry.
  // First, walk the standard xdata entries, collecting their locations and
  // sizes into the UnwindBlocks vector.
  SmallVector<std::pair<BYTE *, ULONG>, 4> UnwindBlocks;
  uint8_t *XdataPtr = reinterpret_cast<uint8_t *>(Addr);
  uint8_t *PtrEnd = XdataPtr + Size;
  CorJitFuncKind FuncKind = CorJitFuncKind::CORJIT_FUNC_ROOT;
  do {
    size_t ReportedXdataSize;
    size_t TotalXdataSize;
    getXdataSize(XdataPtr, &ReportedXdataSize, &TotalXdataSize);
    UnwindBlocks.emplace_back(reinterpret_cast<BYTE *>(XdataPtr),
                              ReportedXdataSize);
    XdataPtr += TotalXdataSize;
    if (XdataPtr == PtrEnd) {
      // No trailing funclet info
      assert(FuncKind == CorJitFuncKind::CORJIT_FUNC_ROOT);
      break;
    }
    // Any subsequent funclet will be a handler.
    FuncKind = CorJitFuncKind::CORJIT_FUNC_HANDLER;
    // The next thing should either be the next xdata entry or the
    // sentinel we insert between that and the clause descriptors.
    // If it is the next xdata entry, bits 0-2 will be the version
    // number (currently only version 1 exists).
  } while ((*XdataPtr & 0x7) == 1);

  if (FuncKind == CorJitFuncKind::CORJIT_FUNC_ROOT) {
    // There are no funclets, so the xdata describes the whole function.
    BYTE *UnwindBlock;
    ULONG UnwindSize;
    std::tie(UnwindBlock, UnwindSize) = UnwindBlocks.pop_back_val();
    assert(UnwindBlocks.empty());
    this->Context->JitInfo->allocUnwindInfo(
        this->HotCodeBlock, nullptr, 0, this->Context->HotCodeSize, UnwindSize,
        UnwindBlock, CorJitFuncKind::CORJIT_FUNC_ROOT);
    return;
  }

  uint32_t *DataPtr = reinterpret_cast<uint32_t *>(XdataPtr);
  // Eat the sentinel that separates the xdata proper from the CLR additional
  // info.
  assert(*DataPtr == 0xffffffff);
  ++DataPtr;
  // Read the number of funclets.
  uint32_t NumFunclets = *DataPtr++;
  // Allocate unwind info space for the function and each funclet
  uint32_t StartOffset = 0;
  FuncKind = CorJitFuncKind::CORJIT_FUNC_ROOT;
  for (uint32_t I = 0; I <= NumFunclets; ++I) {
    // Read the function/funclet end offset
    uint32_t EndOffset = *DataPtr++;
    BYTE *UnwindBlock;
    ULONG UnwindSize;
    std::tie(UnwindBlock, UnwindSize) = UnwindBlocks[I];
    this->Context->JitInfo->allocUnwindInfo(this->HotCodeBlock, nullptr,
                                            StartOffset, EndOffset, UnwindSize,
                                            UnwindBlock, FuncKind);
    FuncKind = CorJitFuncKind::CORJIT_FUNC_HANDLER;
    StartOffset = EndOffset;
  }
  // Read the number of clauses
  uint32_t NumClauses = *DataPtr++;
  // Report that we have clauses
  this->Context->JitInfo->setEHcount(NumClauses);
  // Report the individual clauses
  CORINFO_EH_CLAUSE *Clauses = reinterpret_cast<CORINFO_EH_CLAUSE *>(DataPtr);
  for (uint32_t I = 0; I < NumClauses; ++I) {
    this->Context->JitInfo->setEHinfo(I, &Clauses[I]);
  }
}

void EEMemoryManager::deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                                         size_t Size) {}

EEMemoryManager::~EEMemoryManager() {
  // nothing yet.
}

} // namespace llvm
