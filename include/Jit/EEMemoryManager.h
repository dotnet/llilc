//===--------------- include/Jit/EEMemoryManager.h --------------*- C++ -*-===//
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
/// \brief Declaration of the memory manager interface to the EE.
///
//===----------------------------------------------------------------------===//

#ifndef EE_MEMORYMANAGER_H
#define EE_MEMORYMANAGER_H

#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"

struct LLILCJitContext;

namespace llvm {

/// \brief Memory manager for LLILC
///
/// This class extends \p RTDyldMemoryManager to obtain memory from the
/// CoreCLR's EE for the persistent jit outputs (code, data, and unwind
/// information). Each jit request instantiates its own memory manager.
class EEMemoryManager : public RTDyldMemoryManager {

public:
  /// Construct a new \p EEMemoryManager
  /// \param C Jit context for the method being jitted.
  EEMemoryManager(LLILCJitContext *C)
      : Context(C), HotCodeBlock(nullptr), ColdCodeBlock(nullptr),
        ReadOnlyDataBlock(nullptr), StackMapBlock(nullptr) {}

  /// Destroy an \p EEMemoryManager
  ~EEMemoryManager() override;

  /// \brief Allocates a memory block of (at least) the given size suitable
  /// for executable code.
  ///
  /// The value of \p Alignment must be a power of two.  If \p Alignment is
  /// zero a default alignment of 16 will be used.
  ///
  /// \param Size           Size of allocation request in bytes
  /// \param Alignment      Alignment demand for the allocation
  /// \param SectionID      SectionID for this particular section of code
  /// \param SectionName    Name of the section
  /// \returns Pointer to the newly allocated memory region
  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override;

  // \brief Returns code section address.
  // \returns Code section address.
  uint8_t *getCodeSection();

  /// \brief Allocates a memory block of (at least) the given size suitable
  /// for executable code.
  ///
  /// The value of \p Alignment must be a power of two.  If \p Alignment is
  /// zero a default alignment of 16 will be used.
  ///
  /// \param Size           Size of allocation request in bytes
  /// \param Alignment      Alignment demand for the allocation
  /// \param SectionID      SectionID for this particular section of code
  /// \param SectionName    Name of the section
  /// \param IsReadOnly     True if this is intended for read-only data
  /// \returns Pointer to the newly allocated memory region
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
  /// \param ErrMsg [out] Additional information about finalization errors.
  /// \returns false if an error occurred, true otherwise.
  bool finalizeMemory(std::string *ErrMsg = nullptr) override;

  /// Inform the memory manager about the total amount of memory required to
  /// allocate all sections to be loaded.
  ///
  /// \param CodeSize - the total size of all code sections.
  /// \param CodeAlign - alignment required for the code sections.
  /// \param RODataSize - the total size of all read-only data sections.
  /// \param RODataAlign - alignment required for read-only data sections.
  /// \param RWDataSize - the total size of all read-write data sections.
  /// \param RWDataAlign - alignment required for read-write data sections.
  ///
  /// Note that by default the callback is disabled. To enable it
  /// redefine the method needsToReserveAllocationSpace to return true.
  void reserveAllocationSpace(uintptr_t CodeSize, uint32_t CodeAlign,
                              uintptr_t RODataSize, uint32_t RODataAlign,
                              uintptr_t RWDataSize,
                              uint32_t RWDataAlign) override;

  /// Inform the memory manager about the amount of memory required to hold
  /// unwind codes for the function and funclets being loaded.
  ///
  /// \param Obj - the Object being loaded
  void reserveUnwindSpace(const object::ObjectFile &Obj);

  /// \brief Override to enable the reserveAllocationSpace callback.
  ///
  /// The CoreCLR's EE requires an up-front resevation of the total allocation
  /// demands from the jit. This callback enables that to happen.
  /// \returns true to enable the callback.
  bool needsToReserveAllocationSpace() override { return true; }

  /// \brief Callback to handle processing unwind data.
  ///
  /// This is currently invoked once per .xdata section. The EE uses this info
  /// to build and register the appropriate .pdata with the OS.
  ///
  /// \param Addr      The address of the data in the pre-loaded image.
  /// \param LoadAddr  The address the data will have once loaded.
  /// \param Size      Size of the unwind data in bytes.
  ///
  /// \note Because we're not relocating data during loading, \p Addr and
  /// \p LoadAddr are currently identical.
  void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) override;

  /// \brief Callback to handle unregistering unwind data.
  ///
  /// This is currently a no-op.
  ///
  /// \param Addr      The address of the data in the image.
  /// \param LoadAddr  The address the data has after loading.
  /// \param Size      Size of the unwind data in bytes.
  void deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr,
                          size_t Size) override;

  /// \brief Get the LLVM Stackmap section if allocated.
  ///
  /// Returns a pointer to the .llvm_stackmaps section
  /// if it is already loaded into memory.

  uint8_t *getStackMapSection() { return StackMapBlock; }

  /// \brief Get the HotCode section if allocated.
  ///
  /// Returns a pointer to the HotCode section
  /// if it is already loaded into memory.

  uint8_t *getHotCodeBlock() { return HotCodeBlock; }

private:
  LLILCJitContext *Context;         ///< LLVM context for types, etc.
  uint8_t *HotCodeBlock;            ///< Memory to hold the hot method code.
  uint8_t *ColdCodeBlock;           ///< Memory to hold the cold method code.
  uint8_t *ReadOnlyDataBlock;       ///< Memory to hold the readonly data.
  uint8_t *StackMapBlock;           ///< Memory to hold the readonly StackMap
  uint8_t *ReadOnlyDataUnallocated; ///< Address of unallocated part of RO data.
};
} // namespace llvm

#endif // EEMEMORYMANAGER_H
