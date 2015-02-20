//===----------------- include/Jit/LLILCJit.h -------------------*- C++ -*-===//
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
/// \brief Declaration of the main Jit data structures.
///
//===----------------------------------------------------------------------===//

#ifndef LLILC_JIT_H
#define LLILC_JIT_H

#include "Pal/LLILCPal.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ThreadLocal.h"

struct LLILCJitPerThreadState;

/// \brief This struct holds per-jit request state.
///
/// A Jit context object is allocated (on the stack) each
/// time compileMethod is invoked. Note that the Jit
/// can be re-entered on this thread if while in the middle
/// of jitting a method, other methods must be run.
class LLILCJitContext {

public:
  LLILCJitContext(LLILCJitPerThreadState *State);
  ~LLILCJitContext();
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
  LLILCJitContext *Next;
  LLILCJitPerThreadState *State;
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
struct LLILCJitPerThreadState {
public:
  LLILCJitPerThreadState()
      : LLVMContext(), ClassTypeMap(), ArrayTypeMap(), FieldIndexMap(),
        JitContext(NULL) {}

  llvm::LLVMContext LLVMContext;
  LLILCJitContext *JitContext;
  std::map<CORINFO_CLASS_HANDLE, llvm::Type *> ClassTypeMap;
  std::map<std::tuple<CorInfoType, CORINFO_CLASS_HANDLE, uint32_t>,
           llvm::Type *> ArrayTypeMap;
  std::map<CORINFO_FIELD_HANDLE, uint32_t> FieldIndexMap;
};

/// \brief The Jit interface to the EE.
///
/// This class implements the Jit interface to the EE.
class LLILCJit : public ICorJitCompiler {
public:
  LLILCJit();

  CorJitResult __stdcall compileMethod(ICorJitInfo *JitInfo,
                                       CORINFO_METHOD_INFO *MethodInfo,
                                       UINT Flags, BYTE **NativeEntry,
                                       ULONG *NativeSizeOfCode) override;

  void clearCache() override;
  BOOL isCacheCleanupRequired() override;
  void getVersionIdentifier(GUID *VersionIdentifier) override;
  static LLILCJitContext *getLLILCJitContext() {
    return TheJit->State.get()->JitContext;
  }
  static void __cdecl fatal(int Errnum, ...);
  static void signalHandler(void *Cookie);

private:
  bool readMethod(LLILCJitContext *JitContext);
  bool outputGCInfo(LLILCJitContext *JitContext);

public:
  static LLILCJit *TheJit;

private:
  llvm::sys::ThreadLocal<LLILCJitPerThreadState> State;
};

#endif // LLILC_JIT_H
