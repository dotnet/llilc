//===--------------- include/Jit/EEMemoryManager.h --------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
//
// Declaration of the memory manager interface to the EE.
//
//===----------------------------------------------------------------------===//

#ifndef EE_MEMORYMANAGER_H
#define EE_MEMORYMANAGER_H

#include "llvm/ExecutionEngine/RuntimeDyld.h"

class LLILCJitContext;

namespace llvm {

// Override MemoryManager to implement interface to EE allocator.
// It is through the EE allocated memory that we will return the encoded
// method for execution.
class EEMemoryManager : public RTDyldMemoryManager {

public:
  EEMemoryManager(LLILCJitContext *C)
      : Context(C), HotCodeBlock(nullptr), ColdCodeBlock(nullptr),
        ReadOnlyDataBlock(nullptr) {}
  ~EEMemoryManager() override;

  /// \brief Allocates a memory block of (at least) the given size suitable
  /// for executable code.
  ///
  /// The value of \p Alignment must be a power of two.  If \p Alignment is
  /// zero a default alignment of 16 will be used.
  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override;

  /// \brief Allocates a memory block of (at least) the given size suitable
  /// for executable code.
  ///
  /// The value of \p Alignment must be a power of two.  If \p Alignment is
  /// zero a default alignment of 16 will be used.
  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool IsReadOnly) override;

  /// \brief Update section-specific memory permissions and other attributes.
  ///
  /// This method is called when object loading is complete and section page
  /// permissions can be applied.  It is up to the memory manager
  /// implementation to decide whether or not to act on this method.
  /// The memory manager will typically allocate all sections as read-write
  /// and then apply specific permissions when this method is called.  Code
  /// sections cannot be executed until this function has been called.
  /// In addition, any cache coherency operations needed to reliably use the
  /// memory are also performed.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool finalizeMemory(std::string *ErrMsg = nullptr) override;

  /// Inform the memory manager about the total amount of memory required to
  /// allocate all sections to be loaded:
  /// \p CodeSize - the total size of all code sections
  /// \p DataSizeRO - the total size of all read-only data sections
  /// \p DataSizeRW - the total size of all read-write data sections
  ///
  /// Note that by default the callback is disabled. To enable it
  /// redefine the method needsToReserveAllocationSpace to return true.
  void reserveAllocationSpace(uintptr_t CodeSize, uintptr_t DataSizeRO,
                              uintptr_t DataSizeRW) override;

  /// Overriding to return true to enable the reserveAllocationSpace callback.
  bool needsToReserveAllocationSpace() override { return true; }

  /// \brief Callback to handle processing unwind data.
  ///
  /// This is currently invoked once per .xdata section.
  void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) override;

private:
  LLILCJitContext *Context;

  uint8_t *HotCodeBlock;
  uint8_t *ColdCodeBlock;
  uint8_t *ReadOnlyDataBlock;
  uint8_t *ReadOnlyDataUnallocated;
};
} // namespace llvm

#endif // EEMEMORYMANAGER_H
