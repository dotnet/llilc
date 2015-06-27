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
          this->Context->JitInfo->reserveUnwindInfo(IsHandler, FALSE,
                                                    ReportedByteCount);
          IsHandler = TRUE;
          DataPtr += TotalByteCount;
          if (DataPtr == DataEnd) {
            break;
          }

          // We don't support chained unwind infos, so we expect the next data
          // in the xdata to be one of the two sentinels we insert to indicate
          // funclet positions (0xffffffff or 0x00000000). If the compiler did
          // emit chained unwind info, we'd expect the next unwind info to
          // start immediately; bits 0-2 would be 001 to indicate version 1 of
          // the xdata format, and bit 5 would be set to indicate that it is a
          // chained unwind info.  Check for this pattern to distinguish the
          // unsupported chained case from malformed xdata.
          assert(((*DataPtr == 0) || (*DataPtr == 0xff) ||
                  ((*DataPtr & 0x27) == 0x21)) &&
                 "Malformed .xdata");
          assert(((*DataPtr == 0) || (*DataPtr == 0xff)) &&
                 "Chained unwind infos not supported");

          // Advance DataPtr past the sentinel and the two offsets indicating
          // funclet position/size.  Loop depending which sentinel is seen.
          DataPtr += 12;
        } while (!*(DataPtr - 12));
      }
    }
  }
}

void EEMemoryManager::reserveAllocationSpace(uintptr_t CodeSize,
                                             uintptr_t DataSizeRO,
                                             uintptr_t DataSizeRW) {
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

  uint32_t *DataPtr = reinterpret_cast<uint32_t *>(Addr);
  CorJitFuncKind FuncKind = CorJitFuncKind::CORJIT_FUNC_ROOT;
  uint32_t IsLast;
  do {
    uint8_t *XdataPtr = reinterpret_cast<uint8_t *>(DataPtr);
    size_t ReportedXdataSize;
    size_t TotalXdataSize;
    getXdataSize(XdataPtr, &ReportedXdataSize, &TotalXdataSize);
    DataPtr += (TotalXdataSize / 4);
    bool IsNoEhLeaf = (Size == TotalXdataSize);
    assert(!(IsNoEhLeaf && (FuncKind != CorJitFuncKind::CORJIT_FUNC_ROOT)));
    // Read token to see if this is the last function
    IsLast = IsNoEhLeaf || *(DataPtr++);
    uint32_t StartOffset = (IsNoEhLeaf ? 0 : *(DataPtr++));
    uint32_t EndOffset = (IsNoEhLeaf ? Context->HotCodeSize : *(DataPtr++));
    this->Context->JitInfo->allocUnwindInfo(
        this->HotCodeBlock, nullptr, StartOffset, EndOffset, ReportedXdataSize,
        (BYTE *)XdataPtr, FuncKind);
    // Any subsequent funclet will be a handler.
    FuncKind = CorJitFuncKind::CORJIT_FUNC_HANDLER;
    // Decrement size remaining.
    Size -= TotalXdataSize;
    if (!IsNoEhLeaf) {
      // also read IsLast, StartOffset, and EndOffset
      Size -= 12;
    }
  } while (!IsLast);
  if (Size > 0) {
    // We have EH Clauses to report.
    uint32_t NumClauses = *(DataPtr++);
    CORINFO_EH_CLAUSE *Clauses = reinterpret_cast<CORINFO_EH_CLAUSE *>(DataPtr);
    this->Context->JitInfo->setEHcount(NumClauses);
    for (unsigned i = 0; i < NumClauses; ++i) {
      this->Context->JitInfo->setEHinfo(i, &Clauses[i]);
    }
  }
}

void EEMemoryManager::deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                                         size_t Size) {}

EEMemoryManager::~EEMemoryManager() {
  // nothing yet.
}

} // namespace llvm
