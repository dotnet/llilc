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
#include "jitpch.h"
#include "LLILCJit.h"
#include "GcInfo.h"
#include "jitoptions.h"
#include "compiler.h"
#include "readerir.h"
#include "abi.h"
#include "EEMemoryManager.h"
#include "llvm/CodeGen/GCs.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errno.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include <string>

using namespace llvm;
using namespace llvm::object;

class ObjectLoadListener {
public:
  ObjectLoadListener(LLILCJitContext *Context, EEMemoryManager *MM) {
    this->Context = Context;
    this->MM = MM;
  }

  template <typename ObjSetT, typename LoadResult>
  void operator()(llvm::orc::ObjectLinkingLayerBase::ObjSetHandleT ObjHandles,
                  const ObjSetT &Objs, const LoadResult &LoadedObjInfos) {
    int I = 0;

    for (const auto &PObj : Objs) {

      const object::ObjectFile &Obj = *PObj;
      const RuntimeDyld::LoadedObjectInfo &L = *LoadedObjInfos[I];

      getDebugInfoForObject(Obj, L);

      recordRelocations(Obj);

      ++I;
    }
  }

private:
  void getDebugInfoForObject(const ObjectFile &Obj,
                             const RuntimeDyld::LoadedObjectInfo &L);

  /// \brief Record relocations for external symbols via Jit interface.
  ///
  /// \param Obj Object file to record relocations for.
  void recordRelocations(const ObjectFile &Obj);

  /// \brief Translate from LLVM relocation type to EE relocation type.
  ///
  /// \param LLVMRelocationType LLVM relocation type to translate from.
  /// \returns EE relocation type.
  uint64_t TranslateRelocationType(uint64_t LLVMRelocationType);

private:
  LLILCJitContext *Context;

  EEMemoryManager *MM;
};

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
  const char *DebugClassName = nullptr;
  const char *DebugMethodName = nullptr;

  DebugMethodName = JitInfo->getMethodName(MethodInfo->ftn, &DebugClassName);
  dbgs() << format("INFO:  jitting method %s::%s using LLILCJit\n",
                   DebugClassName, DebugMethodName);
}

void LLILCJitContext::outputSkippingMethodName() {
  const char *DebugClassName = nullptr;
  const char *DebugMethodName = nullptr;

  DebugMethodName = JitInfo->getMethodName(MethodInfo->ftn, &DebugClassName);
  dbgs() << format("INFO:  skipping jitting method %s::%s using LLILCJit\n",
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
  LLILCJitContext Context(PerThreadState);

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

  CorJitResult Result = CORJIT_INTERNALERROR;
  if (JitOptions.IsAltJit) {
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
    if (Context.Options->OptLevel != ::OptLevel::DEBUG_CODE) {
      OptLevel = CodeGenOpt::Level::Default;
    } else {
      OptLevel = CodeGenOpt::Level::None;
      // Options.NoFramePointerElim = true;
    }
    TargetMachine *TM = TheTarget->createTargetMachine(
        LLILC_TARGET_TRIPLE, "", "", Options, Reloc::Default,
        CodeModel::JITDefault, OptLevel);
    Context.TM = TM;

    // Construct the jitting layers.
    EEMemoryManager MM(&Context);
    ObjectLoadListener Listener(&Context, &MM);
    orc::ObjectLinkingLayer<decltype(Listener)> Loader(Listener);
    orc::IRCompileLayer<decltype(Loader)> Compiler(Loader,
                                                   orc::LLILCCompiler(*TM));

    // Now jit the method.
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

      // Use a custom resolver that will tell the dynamic linker to skip
      // relocation processing for external symbols that we create. We will
      // report relocations for those symbols via Jit interface's
      // recordRelocation method.
      EESymbolResolver Resolver(&Context.NameToHandleMap);
      auto HandleSet =
          Compiler.addModuleSet<ArrayRef<Module *>>(M.get(), &MM, &Resolver);

      *NativeEntry =
          (BYTE *)Compiler.findSymbol(Context.MethodName, false).getAddress();

      // TODO: ColdCodeSize, or separated code, is not enabled or included.
      *NativeSizeOfCode = Context.HotCodeSize + Context.ReadOnlyDataSize;

      GcInfoAllocator GcInfoAllocator;
      GCInfo GcInfo(&Context, MM.getStackMapSection(), MM.getHotCodeBlock(),
                    &GcInfoAllocator);
      GcInfo.emitGCInfo();

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
  } else {
    // This method was not selected for jitting by LLILC.
    if (JitOptions.DumpLevel == DumpLevel::SUMMARY) {
      Context.outputSkippingMethodName();
    }
  }

  // Clean up a bit more
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

void ObjectLoadListener::getDebugInfoForObject(
    const ObjectFile &Obj, const RuntimeDyld::LoadedObjectInfo &L) {
  OwningBinary<ObjectFile> DebugObjOwner = L.getObjectForDebug(Obj);
  const ObjectFile &DebugObj = *DebugObjOwner.getBinary();

  // TODO: This extracts DWARF information from the object file, but we will
  // want to also be able to eventually extract WinCodeView information as well
  DWARFContextInMemory DwarfContext(DebugObj);

  // Use symbol info to iterate functions in the object.
  // TODO: This may have to change when we have funclets

  std::vector<std::pair<SymbolRef, uint64_t>> SymbolSizes =
      object::computeSymbolSizes(DebugObj);

  for (const auto &Pair : SymbolSizes) {
    object::SymbolRef Symbol = Pair.first;
    SymbolRef::Type SymType = Symbol.getType();
    if (SymType != SymbolRef::ST_Function)
      continue;

    // Function info
    ErrorOr<StringRef> NameOrError = Symbol.getName();
    assert(NameOrError);
    StringRef Name = NameOrError.get();
    ErrorOr<uint64_t> AddrOrError = Symbol.getAddress();
    if (!AddrOrError) {
      continue; // Error.
    }
    uint64_t Addr = AddrOrError.get();
    uint64_t Size = Pair.second;

    unsigned LastDebugOffset = -1;
    unsigned NumDebugRanges = 0;
    ICorDebugInfo::OffsetMapping *OM;

    DILineInfoTable Lines = DwarfContext.getLineInfoForAddressRange(Addr, Size);

    DILineInfoTable::iterator Begin = Lines.begin();
    DILineInfoTable::iterator End = Lines.end();

    // Count offset entries. Will skip an entry if the current IL offset
    // matches the previous offset.
    for (DILineInfoTable::iterator It = Begin; It != End; ++It) {
      int LineNumber = (It->second).Line;

      if (LineNumber != LastDebugOffset) {
        NumDebugRanges++;
        LastDebugOffset = LineNumber;
      }
    }

    // Reset offset
    LastDebugOffset = -1;

    if (NumDebugRanges > 0) {
      // Allocate OffsetMapping array
      unsigned SizeOfArray =
          (NumDebugRanges) * sizeof(ICorDebugInfo::OffsetMapping);
      OM = (ICorDebugInfo::OffsetMapping *)Context->JitInfo->allocateArray(
          SizeOfArray);

      unsigned CurrentDebugEntry = 0;

      // Iterate through the debug entries and save IL offset, native
      // offset, and source reason
      for (DILineInfoTable::iterator It = Begin; It != End; ++It) {
        int Offset = It->first;
        int LineNumber = (It->second).Line;

        // We store info about if the instruction is being recorded because
        // it is a call in the column field
        bool IsCall = (It->second).Column == 1;

        if (LineNumber != LastDebugOffset) {
          LastDebugOffset = LineNumber;
          OM[CurrentDebugEntry].nativeOffset = Offset;
          OM[CurrentDebugEntry].ilOffset = LineNumber;
          OM[CurrentDebugEntry].source = IsCall
                                             ? ICorDebugInfo::CALL_INSTRUCTION
                                             : ICorDebugInfo::STACK_EMPTY;
          CurrentDebugEntry++;
        }
      }

      // Send array of OffsetMappings to CLR EE
      CORINFO_METHOD_INFO *MethodInfo = Context->MethodInfo;
      CORINFO_METHOD_HANDLE MethodHandle = MethodInfo->ftn;

      Context->JitInfo->setBoundaries(MethodHandle, NumDebugRanges, OM);
    }
  }
}

void ObjectLoadListener::recordRelocations(const ObjectFile &Obj) {
  for (section_iterator SI = Obj.section_begin(), SE = Obj.section_end();
       SI != SE; ++SI) {
    if (SI->isText()) {
      section_iterator RelocatedSection = SI->getRelocatedSection();

      if (RelocatedSection == SE) {
        continue;
      }

      relocation_iterator I = SI->relocation_begin();
      relocation_iterator E = SI->relocation_end();

      for (; I != E;) {
        symbol_iterator Symbol = I->getSymbol();
        assert(Symbol != Obj.symbol_end());
        section_iterator SecI(Obj.section_end());
        Symbol->getSection(SecI);
        const bool IsExtern = SecI == Obj.section_end();
        if (IsExtern) {
          // This is an external symbol. Verify that it's one we created for
          // a global variable and report the relocation via Jit interface.
          uint64_t RelType = I->getType();
          uint64_t Offset = I->getOffset();
          ErrorOr<StringRef> NameOrError = Symbol->getName();
          assert(NameOrError);
          StringRef TargetName = NameOrError.get();
          assert(Context->NameToHandleMap.count(TargetName) == 1);
          uint64_t Target = Context->NameToHandleMap[TargetName];
          Context->JitInfo->recordRelocation(MM->getCodeSection() + Offset,
                                             (void *)Target,
                                             TranslateRelocationType(RelType));
        }
        ++I;
      }
    }
  }
}

uint64_t
ObjectLoadListener::TranslateRelocationType(uint64_t LLVMRelocationType) {
  switch (LLVMRelocationType) {
  case IMAGE_REL_AMD64_ABSOLUTE:
    return IMAGE_REL_BASED_ABSOLUTE;
  case IMAGE_REL_AMD64_ADDR64:
    return IMAGE_REL_BASED_DIR64;
  case IMAGE_REL_AMD64_REL32:
    return IMAGE_REL_BASED_REL32;

  default:
    llvm_unreachable("Unknown reloc type.");
  }
}
