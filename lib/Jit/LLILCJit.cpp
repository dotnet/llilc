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

#include "earlyincludes.h"
#include "GcInfo.h"
#include "jitpch.h"
#include "LLILCJit.h"
#include "jitoptions.h"
#include "readerir.h"
#include "abi.h"
#include "EEMemoryManager.h"
#include "llvm/CodeGen/GCs.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include <string>

using namespace llvm;

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

    // Statepoint GC does not support Fast ISel yet.
    auto &Opts = cl::getRegisteredOptions();
    if (Opts["fast-isel"]->getNumOccurrences() == 0) {
      Opts["fast-isel"]->addOccurrence(0, "fast-isel", "false");
    }
  }

  return LLILCJit::TheJit;
}

// Construct the JIT instance
LLILCJit::LLILCJit() {
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeScalarOpts(Registry);

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  llvm::linkCoreCLRGC();
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
  Context.CurrentModule->setTargetTriple(LLILC_TARGET_TRIPLE);
  Context.MethodName = Context.CurrentModule->getModuleIdentifier();
  Context.TheABIInfo = ABIInfo::get(*Context.CurrentModule);

  // Initialize per invocation JIT options. This should be done after the
  // rest of the Context is filled out as it has dependencies on JitInfo,
  // Flags and MethodInfo.
  JitOptions JitOptions(Context);

  Context.Options = &JitOptions;

  // Construct the TargetMachine that we will emit code for
  std::string ErrStr;
  const llvm::Target *TheTarget =
      TargetRegistry::lookupTarget(LLILC_TARGET_TRIPLE, ErrStr);
  if (!TheTarget) {
    errs() << "Could not create Target: " << ErrStr << "\n";
    return CORJIT_INTERNALERROR;
  }
  TargetOptions Options;
  CodeGenOpt::Level OptLevel;
  if (Context.Options->OptLevel != OptLevel::DEBUG_CODE) {
    OptLevel = CodeGenOpt::Level::Default;
  } else {
    OptLevel = CodeGenOpt::Level::None;
    // Options.NoFramePointerElim = true;
  }
  TargetMachine *TM = TheTarget->createTargetMachine(
      LLILC_TARGET_TRIPLE, "", "", Options, Reloc::Default, CodeModel::Default,
      OptLevel);
  Context.TM = TM;

  // Construct the jitting layers.
  EEMemoryManager MM(&Context);
  LoadLayerT Loader;
  CompileLayerT Compiler(Loader, orc::SimpleCompiler(*TM));

  // Now jit the method.
  CorJitResult Result = CORJIT_INTERNALERROR;
  if (Context.Options->DumpLevel == DumpLevel::VERBOSE) {
    Context.outputDebugMethodName();
  }
  bool HasMethod = this->readMethod(&Context);

  if (HasMethod) {

    if (Context.Options->DoInsertStatepoints) {
      // If using Precise GC, run the GC-Safepoint insertion
      // and lowering passes before generating code.
      legacy::PassManager Passes;
      Passes.add(createPlaceSafepointsPass());

      PassManagerBuilder PMBuilder;
      PMBuilder.OptLevel = 0;  // Set optimization level to -O0
      PMBuilder.SizeLevel = 0; // so that no additional phases are run.
      PMBuilder.populateModulePassManager(Passes);

      Passes.add(createRewriteStatepointsForGCPass(false));
      Passes.run(*M);
    }

    // Don't allow the LoadLayer to search for external symbols, by supplying
    // it a NullResolver.
    NullResolver Resolver;
    auto HandleSet =
        Compiler.addModuleSet<ArrayRef<Module *>>(M.get(), &MM, &Resolver);

    *NativeEntry =
        (BYTE *)Compiler.findSymbol(Context.MethodName, false).getAddress();

    // TODO: ColdCodeSize, or separated code, is not enabled or included.
    *NativeSizeOfCode = Context.HotCodeSize + Context.ReadOnlyDataSize;

    // This is a stop-gap point to issue a default stub of GC info. This lets
    // the CLR consume our methods cleanly. (and the ETW tracing still works)
    // Down the road this will be superseded by a CLR specific
    // GCMetadataPrinter instance or similar.
    this->outputGCInfo(&Context);

    // Dump out any enabled timing info.
    TimerGroup::printAll(errs());

    // Give the jit layers a chance to free resources.
    Compiler.removeModuleSet(HandleSet);

    // Tell the CLR that we've successfully generated code for this method.
    Result = CORJIT_OK;
  }

  // Clean up a bit
  delete Context.TM;
  Context.TM = nullptr;
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

  DumpLevel DumpLevel = JitContext->Options->DumpLevel;

  LLILCJitPerThreadState *PerThreadState = State.get();
  GenIR Reader(JitContext, &PerThreadState->ClassTypeMap,
               &PerThreadState->ReverseClassTypeMap,
               &PerThreadState->BoxedTypeMap, &PerThreadState->ArrayTypeMap,
               &PerThreadState->FieldIndexMap);

  std::string FuncName = JitContext->MethodName;

  try {
    Reader.msilToIR();
  } catch (NotYetImplementedException &Nyi) {
    if (DumpLevel >= ::DumpLevel::SUMMARY) {
      errs() << "Failed to read " << FuncName << '[' << Nyi.reason() << "]\n";
    }
    return false;
  }

  Function *Func = JitContext->CurrentModule->getFunction(FuncName);
  bool IsOk = !verifyFunction(*Func, &dbgs());
  assert(IsOk && "verification failed");

  if (IsOk) {
    if (DumpLevel >= ::DumpLevel::SUMMARY) {
      errs() << "Successfully read " << FuncName << '\n';
    }
  } else {
    if (DumpLevel >= ::DumpLevel::SUMMARY) {
      errs() << "Failed to read " << FuncName << "[verification error]\n";
    }
    return false;
  }

  if (DumpLevel == ::DumpLevel::VERBOSE) {
    Func->dump();
  }

  return IsOk;
}

void LLILCJit::outputGCInfo(LLILCJitContext *JitContext) {
  GcInfoAllocator Allocator;
  GcInfoEncoder gcInfoEncoder(JitContext->JitInfo, JitContext->MethodInfo,
                              &Allocator);

  // The Encoder currently only encodes the CodeSize
  // TODO: Encode pointer liveness information for GC-safepoints in the method

  gcInfoEncoder.SetCodeLength(JitContext->HotCodeSize);

  gcInfoEncoder.Build();
  gcInfoEncoder.Emit();
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
