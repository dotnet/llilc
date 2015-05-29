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
/// \param XdataPtr - Pointer to the (first byte of) the unwind info.
/// \returns The size of the unwind info, in bytes.
static size_t getXdataSize(const uint8_t *XdataPtr) {
  uint8_t NumCodes = XdataPtr[2];
  // There's always a 4-byte header
  size_t Size = 4;
  // Each unwind code is 2 bytes
  Size += (2 * NumCodes);
  if (Size & 2) {
    // The unwind code array is padded to a multiple of 4 bytes
    Size += 2;
  }
  if (XdataPtr[0] != 1) {
    // 4-byte runtime function pointer
    Size += 4;
  }
  if (Size < 8) {
    // Minimum size is 8
    Size = 8;
  }
  return Size;
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
          size_t ByteCount = getXdataSize(DataPtr);
          this->Context->JitInfo->reserveUnwindInfo(IsHandler, FALSE,
                                                    ByteCount);
          IsHandler = TRUE;
          DataPtr += ByteCount;
          if (DataPtr == DataEnd) {
            break;
          }
          // LLILC adds 3 ints between funcs for handler info.  The last int
          // is 0xffffffff if we're done with unwind infos (and about to start
          // EH clauses, which don't need their space specially reserved now)
          // and 0 if there are more unwind infos. Skip past 11 bytes and check
          // the last byte to see if we've reached the end of the unwind infos.
          DataPtr += 11;
        } while (!*(DataPtr++));
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
    uint32_t XdataSize = getXdataSize(XdataPtr);
    DataPtr += (XdataSize / 4);
    bool IsNoEhLeaf = (Size == XdataSize);
    assert(!(IsNoEhLeaf && (FuncKind != CorJitFuncKind::CORJIT_FUNC_ROOT)));
    uint32_t StartOffset = (IsNoEhLeaf ? 0 : *(DataPtr++));
    uint32_t EndOffset = (IsNoEhLeaf ? Context->HotCodeSize : *(DataPtr++));
    this->Context->JitInfo->allocUnwindInfo(this->HotCodeBlock, nullptr,
                                            StartOffset, EndOffset, XdataSize,
                                            (BYTE *)XdataPtr, FuncKind);
    // Any subsequent funclet will be a handler.
    FuncKind = CorJitFuncKind::CORJIT_FUNC_HANDLER;
    // Read token to see if this is the last function;
    IsLast = IsNoEhLeaf || *(DataPtr++);
    // Decrement size remaining.
    Size -= XdataSize;
    if (!IsNoEhLeaf) {
      // also read StartOffset, EndOffset, and IsLast
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
