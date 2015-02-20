//===---- lib/Jit/LLILCJit.cpp --------------------------------*- C++ -*-===//
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
/// \brief Implementation of the main Jit entry points.
///
//===----------------------------------------------------------------------===//

#include "jitpch.h"
#include "LLILCJit.h"
#include "readerir.h"
#include "EEMemoryManager.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/CommandLine.h"
#include <string>

using namespace llvm;

// The one and only Jit Object.
LLILCJit *LLILCJit::TheJit = NULL;

// This is guaranteed to be called by the EE
// in single-threaded mode.
ICorJitCompiler *__stdcall getJit() {

  if (LLILCJit::TheJit == NULL) {
    // These are one-time only operations.
    // Create the singleton jit object.
    LLILCJit::TheJit = new LLILCJit();

    // Register a signal handler, mainly so that the critical
    // section that is entered on windows when LLVM gets a fatal error
    // is properly initialized.
    sys::AddSignalHandler(&LLILCJit::signalHandler, LLILCJit::TheJit);

    // Allow LLVM to pick up options via the environment
    cl::ParseEnvironmentOptions("LLILCJit", "COMplus_altjitOptions");
  }

  return LLILCJit::TheJit;
}

// Construct the JIT instance
LLILCJit::LLILCJit() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
}

#ifdef LLVM_ON_WIN32
// Windows only
BOOL WINAPI DllMain(HANDLE Instance, DWORD Reason, LPVOID Reserved) {
  if (Reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls((HINSTANCE)Instance);
  } else if (Reason == DLL_PROCESS_DETACH) {
    // TBD
  }
  return TRUE;
}
#endif // LLVM_ON_WIN32

extern "C"
void __stdcall sxsJitStartup(void *CcCallbacks) {
  // nothing to do
}

void LLILCJitContext::outputDebugMethodName() {
  const size_t SizeOfBuffer = 512;
  char TempBuffer[SizeOfBuffer];
  const char *DebugClassName = NULL;
  const char *DebugMethodName = NULL;

  DebugMethodName = JitInfo->getMethodName(MethodInfo->ftn, &DebugClassName);
  dbgs() << format("INFO:  jitting method %s::%s using LLILCJit\n",
                   DebugClassName, DebugMethodName);
}

LLILCJitContext::LLILCJitContext(LLILCJitPerThreadState *PerThreadState)
    : State(PerThreadState), HasLoadedBitCode(false) {
  this->Next = State->JitContext;
  State->JitContext = this;
}

LLILCJitContext::~LLILCJitContext() {
  LLILCJitContext *TopContext = State->JitContext;
  assert(this == TopContext && "Unbalanced contexts!");
  State->JitContext = TopContext->Next;
}

// This is the method invoked by the EE to Jit code.
CorJitResult LLILCJit::compileMethod(ICorJitInfo *JitInfo,
                                     CORINFO_METHOD_INFO *MethodInfo,
                                     UINT Flags, BYTE **NativeEntry,
                                     ULONG *NativeSizeOfCode) {

  // Bail if input is malformed
  if (NULL == JitInfo || NULL == MethodInfo || NULL == NativeEntry ||
      NULL == NativeSizeOfCode) {
    return CORJIT_INTERNALERROR;
  }

  // Prep main outputs
  *NativeEntry = NULL;
  *NativeSizeOfCode = NULL;

  // Set up state for this thread (if necessary)
  LLILCJitPerThreadState *PerThreadState = State.get();
  if (PerThreadState == NULL) {
    PerThreadState = new LLILCJitPerThreadState();
    State.set(PerThreadState);
  }

  // Set up context for this Jit request
  LLILCJitContext Context = LLILCJitContext(PerThreadState);

  // Fill in context information from the CLR
  Context.JitInfo = JitInfo;
  Context.MethodInfo = MethodInfo;
  Context.ILCursor = MethodInfo->ILCode;
  Context.Flags = Flags;
  JitInfo->getEEInfo(&Context.EEInfo);

  // Fill in context information from LLVM
  Context.LLVMContext = &PerThreadState->LLVMContext;
  std::unique_ptr<Module> M = Context.getModuleForMethod(MethodInfo);
  Context.CurrentModule = M.get();

  EngineBuilder Builder(std::move(M));
  std::string ErrStr;
  Builder.setErrorStr(&ErrStr);

  std::unique_ptr<RTDyldMemoryManager> MM(new EEMemoryManager(&Context));
  Builder.setMCJITMemoryManager(std::move(MM));

  TargetOptions Options;

  Options.EnableFastISel = true;

  if ((Flags & CORJIT_FLG_DEBUG_CODE) == 0) {
    Builder.setOptLevel(CodeGenOpt::Level::Default);
  } else {
    Builder.setOptLevel(CodeGenOpt::Level::None);
    Options.NoFramePointerElim = 1;
  }

  Builder.setTargetOptions(Options);

  ExecutionEngine *NewEngine = Builder.create();

  if (!NewEngine) {
    errs() << "Could not create ExecutionEngine: " << ErrStr << "\n";
    return CORJIT_INTERNALERROR;
  }

  Context.EE = NewEngine;

  // Now jit the method.
  CorJitResult Result = CORJIT_INTERNALERROR;
  Context.outputDebugMethodName();
  bool HasMethod = this->readMethod(&Context);

  if (HasMethod) {
    Context.EE->generateCodeForModule(Context.CurrentModule);

    // You need to pick up the COFFDyld changes from the MS branch of LLVM
    // or this will fail with an "Incompatible object format!" error
    // from LLVM's dynamic loader.
    uint64_t FunctionAddress =
        Context.EE->getFunctionAddress(Context.MethodName);
    *NativeEntry = (BYTE *)FunctionAddress;

    // TODO: ColdCodeSize, or separated code, is not enabled or included.
    *NativeSizeOfCode = Context.HotCodeSize + Context.ReadOnlyDataSize;

    // This is a stop-gap point to issue a default stub of GC info. This lets
    // the CLR consume our methods cleanly. (and the ETW tracing still works)
    // Down the road this will be superseded by a CLR specific
    // GCMetadataPrinter instance or similar.
    this->outputGCInfo(&Context);

    // Dump out any enabled timing info.
    TimerGroup::printAll(errs());

    // Tell the CLR that we've successfully generated code for this method.
    Result = CORJIT_OK;
  }

  return Result;
}

std::unique_ptr<Module>
LLILCJitContext::getModuleForMethod(CORINFO_METHOD_INFO *MethodInfo) {
  // Grab name info from the EE.
  const char *DebugClassName = NULL;
  const char *DebugMethodName = NULL;
  DebugMethodName = JitInfo->getMethodName(MethodInfo->ftn, &DebugClassName);

  // Stop gap name.  The full naming will likely require some more info.
  std::string ModName(DebugClassName);
  ModName.append(1, '.');
  ModName.append(DebugMethodName);

  std::unique_ptr<Module> M;
  char *BitcodePath = getenv("BITCODE_PATH");

  if (BitcodePath != NULL) {
    SMDiagnostic Err;
    std::string Path = std::string(BitcodePath);

    // If there is a bitcode path, use the debug module name to look for an .bc
    // file. If one is found then load it directly.
    Path.append("\\");
    Path.append(ModName);
    Path.append(".bc");
    M = llvm::parseIRFile(Path, Err, *this->LLVMContext);

    if (!M) {
      // Err.print("IR Parsing failed: ", errs());
      this->HasLoadedBitCode = false;
    } else {
      std::string Message("Loaded bitcode from: ");
      Message.append(Path);
      dbgs() << Message;
      this->HasLoadedBitCode = true;
    }
  }

  if (!this->HasLoadedBitCode) {
    M = std::unique_ptr<Module>(new Module(ModName, *this->LLVMContext));
  }

  return std::move(M);
}

// Read method MSIL and construct LLVM bitcode
bool LLILCJit::readMethod(LLILCJitContext *JitContext) {
  if (JitContext->HasLoadedBitCode) {
    // This is a case where we side loaded a llvm bitcode module.
    // The module is already complete so we avoid reading entirely.
    return true;
  }

  LLILCJitPerThreadState *PerThreadState = State.get();
  GenIR Reader(JitContext, &PerThreadState->ClassTypeMap,
               &PerThreadState->ArrayTypeMap, &PerThreadState->FieldIndexMap);

  std::string FuncName = JitContext->CurrentModule->getModuleIdentifier();

  try {
    Reader.msilToIR();
  } catch (NotYetImplementedException &Nyi) {
    errs() << "Failed to read " << FuncName << '[' << Nyi.reason() << "]\n";
    return false;
  }

  Function *Func = JitContext->CurrentModule->getFunction(FuncName);
  bool IsOk = !verifyFunction(*Func, &dbgs());

  if (IsOk) {
    errs() << "Successfully read " << FuncName << '\n';
  } else {
    errs() << "Read " << FuncName << " but failed verification\n";
  }

  Func->dump();

  JitContext->MethodName = FuncName;

  return IsOk;
}

// Stop gap function to allocate and emit stub GCInfo for the method.
// This avoids a crash in the EE.
// Assuming that this will be replaced by an override of the GCMetaData
// or similar writer in LLVM once we move to the safepoint design.
bool LLILCJit::outputGCInfo(LLILCJitContext *JitContext) {
  size_t Size = 5;
  void *GCInfoBuffer = JitContext->JitInfo->allocGCInfo(Size);

  if (GCInfoBuffer == nullptr) {
    return false;
  }

  // First word of the GCInfoBuffer should be the size of the method.
  *(uint32_t *)GCInfoBuffer = JitContext->HotCodeSize;

  // 0x8 is the end sentinel of the buffer.
  *(((char *)GCInfoBuffer) + 4) = 0x8;

  return true;
}

// Notification from the runtime that any caches should be cleaned up.
void LLILCJit::clearCache() { return; }

// Notify runtime if we have something to clean up
BOOL LLILCJit::isCacheCleanupRequired() { return FALSE; }

// Verify the JIT/EE interface identifier.
void LLILCJit::getVersionIdentifier(GUID *VersionIdentifier) {
  _ASSERTE(VersionIdentifier != nullptr);
  memcpy(VersionIdentifier, &JITEEVersionIdentifier, sizeof(GUID));
}

// Raise a fatal error.
void __cdecl LLILCJit::fatal(int Errnum, ...) {
  _ASSERTE(FAILED(Errnum));
  ULONG_PTR ExceptArg = Errnum;
  RaiseException(CORJIT_INTERNALERROR, EXCEPTION_NONCONTINUABLE, 1, &ExceptArg);
}

//  Handle an abort signal from LLVM. We don't do anything, but registering
//  this handler avoids a crash when LLVM goes down.
void LLILCJit::signalHandler(void *Cookie) {
  // do nothing
}
