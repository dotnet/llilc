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
#include "options.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/ThreadLocal.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"

class ABIInfo;
struct LLILCJitPerThreadState;
namespace llvm {
class EEMemoryManager;
}

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

  /// Write an informational message about this jit request to LLVM's dbgs().
  void outputDebugMethodName();

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
  uint32_t HotCodeSize = 0;      ///< Size of hot code section in bytes.
  uint32_t ColdCodeSize = 0;     ///< Size of cold code section in bytes.
  uint32_t ReadOnlyDataSize = 0; ///< Size of readonly data ref'd from code.
  //@}
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
      : LLVMContext(), ClassTypeMap(), ReverseClassTypeMap(), BoxedTypeMap(),
        ArrayTypeMap(), FieldIndexMap(), JitContext(nullptr) {}

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
           llvm::Type *> ArrayTypeMap;

  /// \brief Map from a field handle to the index of that field in the overall
  /// layout of the enclosing class.
  ///
  /// Used to build struct GEP instructions in LLVM IR for field accesses.
  std::map<CORINFO_FIELD_HANDLE, uint32_t> FieldIndexMap;
};

/// \brief Stub \p SymbolResolver expecting no resolution requests
///
/// The ObjectLinkingLayer takes a SymbolResolver ctor parameter.
/// The CLR EE resolves tokens to addresses for the Jit during IL reading,
/// so no symbol resolution is actually needed at ObjLinking time.
class NullResolver : public llvm::RuntimeDyld::SymbolResolver {
public:
  llvm::RuntimeDyld::SymbolInfo findSymbol(const std::string &Name) final {
    llvm_unreachable("Reader resolves tokens directly to addresses");
  }

  llvm::RuntimeDyld::SymbolInfo
  findSymbolInLogicalDylib(const std::string &Name) final {
    llvm_unreachable("Reader resolves tokens directly to addresses");
  }
};

// Object layer that notifies the memory manager to reserve unwind space for
// each object and otherwise passes processing on to the underlying base
// object layer
template <typename BaseLayerT> class ReserveUnwindSpaceLayer {
public:
  typedef llvm::orc::ObjectLinkingLayerBase::ObjSetHandleT ObjSetHandleT;

  ReserveUnwindSpaceLayer(BaseLayerT *BaseLayer, llvm::EEMemoryManager *MM) {
    this->BaseLayer = BaseLayer;
    this->MM = MM;
  }

  template <typename ObjSetT, typename MemoryManagerPtrT,
            typename SymbolResolverPtrT>
  ObjSetHandleT addObjectSet(const ObjSetT &Objects, MemoryManagerPtrT MemMgr,
                             SymbolResolverPtrT Resolver) {
    for (const auto &Obj : Objects) {
      MM->reserveUnwindSpace(*Obj);
    }
    return BaseLayer->addObjectSet(Objects, MemMgr, Resolver);
  }

  void removeObjectSet(ObjSetHandleT H) {
    BaseLayer->removeObjectSet(std::move(H));
  }

  llvm::orc::JITSymbol findSymbol(llvm::StringRef Name,
                                  bool ExportedSymbolsOnly) {
    return BaseLayer->findSymbol(Name, ExportedSymbolsOnly);
  }

  template <typename OwningMBSet>
  void takeOwnershipOfBuffers(ObjSetHandleT H, OwningMBSet MBs) {
    BaseLayer->takeOwnershipOfBuffers(std::move(H), std::move(MBs));
  }

private:
  BaseLayerT *BaseLayer;
  llvm::EEMemoryManager *MM;
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
  typedef llvm::orc::ObjectLinkingLayer<> LoadLayerT;
  typedef ReserveUnwindSpaceLayer<LoadLayerT> ReserveUnwindSpaceLayerT;
  typedef llvm::orc::IRCompileLayer<ReserveUnwindSpaceLayerT> CompileLayerT;

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

private:
  /// Convert a method into LLVM IR.
  /// \param JitContext Context record for the method's jit request.
  /// \returns \p true if the conversion was successful.
  bool readMethod(LLILCJitContext *JitContext);

  /// Output GC info to the EE.
  /// \param JitContext Context record for the method's jit request.
  void outputGCInfo(LLILCJitContext *JitContext);

public:
  /// A pointer to the singleton jit instance.
  static LLILCJit *TheJit;

private:
  /// Thread local storage for the jit's per-thread state.
  llvm::sys::ThreadLocal<LLILCJitPerThreadState> State;
};

#endif // LLILC_JIT_H
