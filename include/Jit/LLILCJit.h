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
#include "Reader/options.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ThreadLocal.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/NullResolver.h"
#include "llvm/Config/config.h"

class ABIInfo;
class GcInfo;
struct LLILCJitPerThreadState;
namespace llvm {
class EEMemoryManager;
} // namespace llvm

/// \brief This struct holds per-jit request state.
///
/// LLILC is invoked to jit one method at a time. An \p LLILCJitContext
/// represents all of the information about a particular jit request.
///
/// Note the Jit can be re-entered on the same thread while in the
/// middle of jitting a method, if other methods must be run and also require
/// jitting. To handle this, the contexts form a stack, so that the jit can
/// keep the state from nested jit requests distinct from parent requests.
struct LLILCJitContext {

  /// Construct a context and push it onto the context stack.
  /// \param State the per-thread state for this thread.
  LLILCJitContext(LLILCJitPerThreadState *State);

  /// Destruct this context and pop it from the context stack.
  ~LLILCJitContext();

  /// \brief Create or sideload the module for this method.
  ///
  /// In typical usage this creates a new empty \p Module to hold the method
  /// that is about to be jitted. The reader then creates the IR for the
  /// method.
  ///
  /// Alternatively, if BITCODE_PATH is set to a directory name and there is a
  /// suitably named bitcode file in that directory, the file is loaded and
  /// parsed and the resulting module is returned, and the code from that
  /// parsing is used for the method instead. This provides a way to experiment
  /// with alternative codegen. Note however jit codegen may embed handle
  /// values in the IR and these values can vary from run to run, so a bitcode
  /// file saved from a previous run may not work as expected.
  ///
  /// \param MethodInfo  The CoreCLR method info for the method being jitted.
  std::unique_ptr<llvm::Module>
  getModuleForMethod(CORINFO_METHOD_INFO *MethodInfo);

public:
  /// \name CoreCLR EE information
  //@{
  ICorJitInfo *JitInfo;            ///< EE callback interface.
  CORINFO_METHOD_INFO *MethodInfo; ///< Description of method to jit.
  uint32_t Flags;                  ///< Flags controlling jit behavior.
  CORINFO_EE_INFO EEInfo;          ///< Information about internal EE data.
  std::string MethodName;          ///< Name of the method (for diagnostics).
  //@}

  /// \name LLVM information
  //@{
  llvm::LLVMContext *LLVMContext; ///< LLVM context for types and similar.
  llvm::Module *CurrentModule;    ///< Module holding LLVM IR.
  llvm::TargetMachine *TM;        ///< Target characteristics
  bool HasLoadedBitCode;          ///< Flag for side-loaded LLVM IR.
  llvm::StringMap<uint64_t> NameToHandleMap; ///< Map from global object names
                                             ///< to the corresponding CLR
                                             ///< handles.
  //@}

  /// \name ABI information
  //@{
  ABIInfo *TheABIInfo; ///< Target ABI information.
  //@}

  /// \name Context management
  //@{
  LLILCJitContext *Next;         ///< Parent jit context, if any.
  LLILCJitPerThreadState *State; ///< Per thread state for the jit.
  //@}

  /// \name Per invocation JIT Options
  //@{
  ::Options *Options;
  //@}

  /// \name Jit output sizes
  //@{
  uintptr_t HotCodeSize = 0;      ///< Size of hot code section in bytes.
  uintptr_t ColdCodeSize = 0;     ///< Size of cold code section in bytes.
  uintptr_t ReadOnlyDataSize = 0; ///< Size of readonly data ref'd from code.
  uintptr_t StackMapSize = 0;     ///< Size of readonly Stackmap section.
  //@}

  /// \name GC Information
  ::GcInfo *GcInfo; ///< GcInfo for functions in CurrentModule
};

/// \brief This struct holds per-thread Jit state.
///
/// The Jit may be invoked concurrently on more than one thread. To avoid
/// synchronization overhead it maintains per-thread state, mainly to map from
/// CoreCLR EE artifacts to LLVM data structures.
///
/// The per thread state also provides access to the current Jit context in
/// case it is ever needed.
struct LLILCJitPerThreadState {
public:
  /// Construct a new state.
  LLILCJitPerThreadState()
      : LLVMContext(), JitContext(nullptr), ClassTypeMap(),
        ReverseClassTypeMap(), BoxedTypeMap(), ArrayTypeMap(), FieldIndexMap() {
  }

  /// Each thread maintains its own \p LLVMContext. This is where
  /// LLVM keeps definitions of types and similar constructs.
  llvm::LLVMContext LLVMContext;

  /// Pointer to the current jit context.
  LLILCJitContext *JitContext;

  /// Map from class handles to the LLVM types that represent them.
  std::map<CORINFO_CLASS_HANDLE, llvm::Type *> ClassTypeMap;

  /// Map from LLVM types to the corresponding class handles.
  std::map<llvm::Type *, CORINFO_CLASS_HANDLE> ReverseClassTypeMap;

  /// Map from class handles for value types to the LLVM types that represent
  /// their boxed versions.
  std::map<CORINFO_CLASS_HANDLE, llvm::Type *> BoxedTypeMap;

  /// \brief Map from class handles for arrays to the LLVM types that represent
  /// them.
  ///
  /// \note Arrays can't be looked up via the \p ClassTypeMap. Instead they
  /// are looked up via element type, element handle, array rank, and whether
  /// this array is a vector (single-dimensional array with zero lower bound).
  std::map<std::tuple<CorInfoType, CORINFO_CLASS_HANDLE, uint32_t, bool>,
           llvm::Type *>
      ArrayTypeMap;

  /// \brief Map from a field handle to the index of that field in the overall
  /// layout of the enclosing class.
  ///
  /// Used to build struct GEP instructions in LLVM IR for field accesses.
  std::map<CORINFO_FIELD_HANDLE, uint32_t> FieldIndexMap;
};

/// \brief Stub \p SymbolResolver that tells dynamic linker not to apply
/// relocations for external symbols we know about.
///
/// The ObjectLinkingLayer takes a SymbolResolver ctor parameter.
class EESymbolResolver : public llvm::RuntimeDyld::SymbolResolver {
public:
  EESymbolResolver(llvm::StringMap<uint64_t> *NameToHandleMap) {
    this->NameToHandleMap = NameToHandleMap;
  }

  llvm::RuntimeDyld::SymbolInfo findSymbol(const std::string &Name) final {
    // Address UINT64_MAX means that we will resolve relocations for this symbol
    // manually and the dynamic linker will skip relocation resolution for this
    // symbol.
    assert(NameToHandleMap->count(Name) == 1);
    return llvm::RuntimeDyld::SymbolInfo(UINT64_MAX,
                                         llvm::JITSymbolFlags::None);
  }

  llvm::RuntimeDyld::SymbolInfo
  findSymbolInLogicalDylib(const std::string &Name) final {
    llvm_unreachable("Unexpected request to resolve a common symbol.");
  }

private:
  llvm::StringMap<uint64_t> *NameToHandleMap;
};

/// \brief The Jit interface to the CoreCLR EE.
///
/// This class implements the Jit interface to the CoreCLR EE. The EE uses this
/// to direct the jit to jit methods. There is a single instance of this
/// object in the process, created by the \p getJit() function exported from
/// the jit library or DLL.
///
/// Because the jit can be invoked re-entrantly and on multiple threads,
/// this class itself has no mutable state. Any state kept live between
/// top-level invocations of the jit is held in thread local storage.
class LLILCJit : public ICorJitCompiler {
public:
  /// \brief Construct a new jit instance.
  ///
  /// There is only one LLILC jit instance per process, so this
  /// constructor also invokes a number of LLVM initialization methods.
  LLILCJit();

  /// \brief Jit a method.
  ///
  /// Main entry point into the jit. Invoked once per method to be jitted.
  /// May be invoked re-entrantly and/or concurrently on multiple threads.
  ///
  /// \param JitInfo                Interface the jit can use for callbacks.
  /// \param MethodInfo             Data structure describing the method to jit.
  /// \param Flags                  CorJitFlags controlling jit behavior.
  /// \param NativeEntry [out]      Address of the jitted code.
  /// \param NativeSizeOfCode [out] Length of the jitted code.
  ///
  /// \returns Code indicating success or failure of the jit request.
  CorJitResult __stdcall compileMethod(ICorJitInfo *JitInfo,
                                       CORINFO_METHOD_INFO *MethodInfo,
                                       UINT Flags, BYTE **NativeEntry,
                                       ULONG *NativeSizeOfCode) override;

  /// Clear any caches kept by the jit.
  void clearCache() override;

  /// Check if cache cleanup is required.
  /// \returns \p true if the jit is caching information.
  BOOL isCacheCleanupRequired() override;

  /// \brief Get the Jit's version identifier.
  ///
  /// To avoid version skew between the Jit and the EE, the EE will query
  /// the jit for a version identifier GUID, and verify that it matches the
  /// expectations of the EE.
  ///
  /// \param VersionIdentifier [out] Buffer where the jit's GUID can be stored.
  void getVersionIdentifier(GUID *VersionIdentifier) override;

  /// \brief Get access to the current jit context.
  ///
  /// This method can be called from anywhere to retrieve the current jit
  /// context.
  ///
  /// \returns Context for the current jit request.
  static LLILCJitContext *getLLILCJitContext() {
    return TheJit->State.get()->JitContext;
  }

  /// Report a fatal error in the Jit.
  /// \param Errnum Error number to report in exception context.
  static void __cdecl fatal(int Errnum, ...);

  /// Signal handler for LLVM abort signals.
  /// \param Cookie additional context to help identify which handler
  ///        instances this is.
  static void signalHandler(void *Cookie);

  /// Return SIMD generic vector length if LLILC is primary JIT.
  unsigned getMaxIntrinsicSIMDVectorLength(DWORD CpuCompileFlags) override;

private:
  /// Convert a method into LLVM IR.
  /// \param JitContext Context record for the method's jit request.
  /// \returns \p true if the conversion was successful.
  bool readMethod(LLILCJitContext *JitContext);

public:
  /// A pointer to the singleton jit instance.
  static LLILCJit *TheJit;

private:
  /// Thread local storage for the jit's per-thread state.
  llvm::sys::ThreadLocal<LLILCJitPerThreadState> State;
};

#endif // LLILC_JIT_H
