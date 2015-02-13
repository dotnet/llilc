//===----------------- include/Jit/MSILCJit.h -------------------*- C++ -*-===//
//
// LLVM-MSILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Declaration of the main Jit data structures.
///
//===----------------------------------------------------------------------===//

#ifndef MSILC_JIT_H
#define MSILC_JIT_H

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ThreadLocal.h"

struct MSILCJitPerThreadState;

/// \brief This struct holds per-jit request state.
///
/// A Jit context object is allocated (on the stack) each
/// time compileMethod is invoked. Note that the Jit
/// can be re-entered on this thread if while in the middle
/// of jitting a method, other methods must be run.
class MSILCJitContext {

public:
  MSILCJitContext(MSILCJitPerThreadState *State);
  ~MSILCJitContext();
  std::unique_ptr<llvm::Module>
  getModuleForMethod(CORINFO_METHOD_INFO *MethodInfo);
  void outputDebugMethodName();
  inline uint8_t getILByte() { return *((uint8_t *&)ILCursor)++; }
  inline uint32_t getILUInt32() { return *((UNALIGNED uint32_t *&)ILCursor)++; }

public:
  uint8_t *ILCursor;
  ICorJitInfo *JitInfo;
  CORINFO_METHOD_INFO *MethodInfo;
  uint32_t Flags;
  llvm::Module *CurrentModule;
  std::string MethodName;
  llvm::ExecutionEngine *EE;
  bool HasLoadedBitCode;
  MSILCJitContext *Next;
  MSILCJitPerThreadState *State;
  CORINFO_EE_INFO EEInfo;
  llvm::LLVMContext *LLVMContext;

  // Allocated memory sizes for method being processed.
  // This is encoded byte size for the complied method
  // and associated data.

  uint32_t HotCodeSize = 0;
  uint32_t ColdCodeSize = 0;
  uint32_t ReadOnlyDataSize = 0;
};

/// \brief This struct holds per-thread Jit state.
///
/// The Jit may be invoked concurrently on more than
/// one thread. To avoid synchronization overhead
/// it maintains per-thread state, mainly to map from
/// EE artifacts to LLVM data structures.
///
/// The per thread state also provides access to the
/// current Jit context in case it's needed.
struct MSILCJitPerThreadState {
public:
  MSILCJitPerThreadState()
      : LLVMContext(), ClassTypeMap(), ArrayTypeMap(), FieldIndexMap(),
        JitContext(NULL) {}

  llvm::LLVMContext LLVMContext;
  MSILCJitContext *JitContext;
  std::map<CORINFO_CLASS_HANDLE, llvm::Type *> ClassTypeMap;
  std::map<std::tuple<CorInfoType, CORINFO_CLASS_HANDLE, uint32_t>,
           llvm::Type *> ArrayTypeMap;
  std::map<CORINFO_FIELD_HANDLE, uint32_t> FieldIndexMap;
};

/// \brief The Jit interface to the EE.
///
/// This class implements the Jit interface to the EE.
class MSILCJit : public ICorJitCompiler {
public:
  MSILCJit();

  CorJitResult __stdcall compileMethod(ICorJitInfo *JitInfo,
                                       CORINFO_METHOD_INFO *MethodInfo,
                                       UINT Flags, BYTE **NativeEntry,
                                       ULONG *NativeSizeOfCode) override;

  void clearCache() override;
  BOOL isCacheCleanupRequired() override;
  void getVersionIdentifier(GUID *VersionIdentifier) override;
  static MSILCJitContext *getMSILCJitContext() {
    return TheJit->State.get()->JitContext;
  }
  static void __cdecl fatal(int Errnum, ...);
  static void signalHandler(void *Cookie);

private:
  bool readMethod(MSILCJitContext *JitContext);
  bool outputGCInfo(MSILCJitContext *JitContext);

public:
  static MSILCJit *TheJit;

private:
  llvm::sys::ThreadLocal<MSILCJitPerThreadState> State;
};

#endif // MSILC_JIT_H
