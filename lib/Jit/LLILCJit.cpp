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
#include "options.h"
#include "readerir.h"
#include "abi.h"
#include "EEMemoryManager.h"
#include "llvm/CodeGen/GCs.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LegacyPassManager.h"
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
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include <string>

using namespace llvm;

// Get the LLVM IR dump level. For now this is done by directly
// accessing environment variable. When CLR config support is
// included, update it here.
LLVMDumpLevel dumpLevel() {
  const char *LevelCStr = getenv("DUMPLLVMIR");
  if (LevelCStr) {
    std::string Level = LevelCStr;
    std::transform(Level.begin(), Level.end(), Level.begin(), ::toupper);
    if (Level.compare("VERBOSE") == 0) {
      return VERBOSE;
    }
    if (Level.compare("SUMMARY") == 0) {
      return SUMMARY;
    }
  }
  return NODUMP;
}

// Get the GC-Scheme used by the runtime -- conservative/precise
// For now this is done by directly accessing environment variable.
// When CLR config support is included, update it here.
bool shouldUseConservativeGC() {
  const char *LevelCStr = getenv("COMPLUS_GCCONSERVATIVE");
  return (LevelCStr != nullptr) && (strcmp(LevelCStr, "1") == 0);
}

// Determine if GC statepoints should be inserted.
bool shouldInsertStatepoints() {
  const char *LevelCStr = getenv("COMPLUS_INSERTSTATEPOINTS");
  return (LevelCStr != nullptr) && (strcmp(LevelCStr, "1") == 0);
}

// The one and only Jit Object.
LLILCJit *LLILCJit::TheJit = nullptr;

// This is guaranteed to be called by the EE
// in single-threaded mode.
ICorJitCompiler *__stdcall getJit() {

  if (LLILCJit::TheJit == nullptr) {
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
  llvm::linkStatepointExampleGC();

  ShouldInsertStatepoints = shouldInsertStatepoints();
  assert(ShouldInsertStatepoints ||
         shouldUseConservativeGC() && "Statepoints required for precise-GC");
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

extern "C" void __stdcall sxsJitStartup(void *CcCallbacks) {
  // nothing to do
}

void LLILCJitContext::outputDebugMethodName() {
  const size_t SizeOfBuffer = 512;
  char TempBuffer[SizeOfBuffer];
  const char *DebugClassName = nullptr;
  const char *DebugMethodName = nullptr;

  DebugMethodName = JitInfo->getMethodName(MethodInfo->ftn, &DebugClassName);
  dbgs() << format("INFO:  jitting method %s::%s using LLILCJit\n",
                   DebugClassName, DebugMethodName);
}

LLILCJitContext::LLILCJitContext(LLILCJitPerThreadState *PerThreadState)
    : State(PerThreadState), HasLoadedBitCode(false), Options(this) {
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
  if (nullptr == JitInfo || nullptr == MethodInfo || nullptr == NativeEntry ||
      nullptr == NativeSizeOfCode) {
    return CORJIT_INTERNALERROR;
  }

  // Prep main outputs
  *NativeEntry = nullptr;
  *NativeSizeOfCode = 0;

  // Set up state for this thread (if necessary)
  LLILCJitPerThreadState *PerThreadState = State.get();
  if (PerThreadState == nullptr) {
    PerThreadState = new LLILCJitPerThreadState();
    State.set(PerThreadState);
  }

  // Set up context for this Jit request
  LLILCJitContext Context = LLILCJitContext(PerThreadState);

  // Fill in context information from the CLR
  Context.JitInfo = JitInfo;
  Context.MethodInfo = MethodInfo;
  Context.Flags = Flags;
  JitInfo->getEEInfo(&Context.EEInfo);

  // Fill in context information from LLVM
  Context.LLVMContext = &PerThreadState->LLVMContext;
  std::unique_ptr<Module> M = Context.getModuleForMethod(MethodInfo);
  Context.CurrentModule = M.get();
  Context.CurrentModule->setTargetTriple(LLVM_DEFAULT_TARGET_TRIPLE);
  Context.TheABIInfo = ABIInfo::get(*Context.CurrentModule);

  // Initialize per invocation JIT options. This should be done after the
  // rest of the Context is filled out as it has dependencies on Flags and
  // MethodInfo
  Context.Options.initialize();

  EngineBuilder Builder(std::move(M));
  std::string ErrStr;
  Builder.setErrorStr(&ErrStr);

  std::unique_ptr<RTDyldMemoryManager> MM(new EEMemoryManager(&Context));
  Builder.setMCJITMemoryManager(std::move(MM));

  TargetOptions Options;

  // Statepoint GC does not support FastIsel yet.
  Options.EnableFastISel = false;

  if (Context.Options.OptLevel != OptLevel::DEBUG_CODE) {
    Builder.setOptLevel(CodeGenOpt::Level::Default);
  } else {
    Builder.setOptLevel(CodeGenOpt::Level::None);
    Options.NoFramePointerElim = true;
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
  if (Context.Options.DumpLevel == VERBOSE) {
    Context.outputDebugMethodName();
  }
  bool HasMethod = this->readMethod(&Context);

  if (HasMethod) {

    if (ShouldInsertStatepoints) {
      // If using Precise GC, run the GC-Safepoint insertion
      // and lowering passes before generating code.
      legacy::PassManager Passes;
      Passes.add(createPlaceSafepointsPass());

      PassManagerBuilder PMBuilder;
      PMBuilder.OptLevel = 0;  // Set optimization level to -O0
      PMBuilder.SizeLevel = 0; // so that no additional phases are run.
      PMBuilder.populateModulePassManager(Passes);

      Passes.add(createRewriteStatepointsForGCPass(false));
      Passes.run(*Context.CurrentModule);
    }

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

  // Clean up a bit
  delete Context.EE;
  Context.EE = nullptr;
  delete Context.TheABIInfo;
  Context.TheABIInfo = nullptr;

  return Result;
}

std::unique_ptr<Module>
LLILCJitContext::getModuleForMethod(CORINFO_METHOD_INFO *MethodInfo) {
  // Grab name info from the EE.
  const char *DebugClassName = nullptr;
  const char *DebugMethodName = nullptr;
  DebugMethodName = JitInfo->getMethodName(MethodInfo->ftn, &DebugClassName);

  // Stop gap name.  The full naming will likely require some more info.
  std::string ModName(DebugClassName);
  ModName.append(1, '.');
  ModName.append(DebugMethodName);

  std::unique_ptr<Module> M;
  char *BitcodePath = getenv("BITCODE_PATH");

  if (BitcodePath != nullptr) {
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

  LLVMDumpLevel DumpLevel = JitContext->Options.DumpLevel;

  LLILCJitPerThreadState *PerThreadState = State.get();
  GenIR Reader(JitContext, &PerThreadState->ClassTypeMap,
               &PerThreadState->ReverseClassTypeMap,
               &PerThreadState->BoxedTypeMap, &PerThreadState->ArrayTypeMap,
               &PerThreadState->FieldIndexMap);

  std::string FuncName = JitContext->CurrentModule->getModuleIdentifier();

  try {
    Reader.msilToIR();
  } catch (NotYetImplementedException &Nyi) {
    if (DumpLevel >= SUMMARY) {
      errs() << "Failed to read " << FuncName << '[' << Nyi.reason() << "]\n";
    }
    return false;
  }

  Function *Func = JitContext->CurrentModule->getFunction(FuncName);
  bool IsOk = !verifyFunction(*Func, &dbgs());

  if (IsOk) {
    if (DumpLevel >= SUMMARY) {
      errs() << "Successfully read " << FuncName << '\n';
    }
  } else {
    if (DumpLevel >= SUMMARY) {
      errs() << "Read " << FuncName << " but failed verification\n";
    }
  }

  if (DumpLevel == VERBOSE) {
    Func->dump();
  }

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
