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
#include "EEObjectLinkingLayer.h"
#include "llvm/CodeGen/GCs.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
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
  ObjectLoadListener(LLILCJitContext *Context) { this->Context = Context; }

  template <typename ObjSetT, typename LoadResult>
  void operator()(llvm::orc::ObjectLinkingLayerBase::ObjSetHandleT ObjHandles,
                  const ObjSetT &Objs, const LoadResult &LoadedObjInfos) {
    int I = 0;

    for (const auto &PObj : Objs) {

      const object::ObjectFile &Obj = *PObj;
      const RuntimeDyld::LoadedObjectInfo &L = *LoadedObjInfos[I];

      getDebugInfoForObject(Obj, L);

      recordRelocations(Obj, L);

      ++I;
    }
  }

private:
  /// \brief Get debug info for object and send to CLR EE
  ///
  /// \param Obj Object file to get debug info for
  void getDebugInfoForObject(const ObjectFile &Obj,
                             const RuntimeDyld::LoadedObjectInfo &L);

  /// \brief Record relocations for external symbols via Jit interface.
  ///
  /// \param Obj Object file to record relocations for.
  void recordRelocations(const ObjectFile &Obj,
                         const RuntimeDyld::LoadedObjectInfo &L);

  /// \brief Compute EE relocation type and addend from LLVM relocation.
  ///
  /// \param FixupAddress            Address where the reloc will be applied.
  /// \param LLVMRelocationType      LLVM relocation type to translate from.
  /// \param EERelocationType [out]  Corresponding EE relocation type.
  /// \param Addend [out]            Value in image to add to symbol's address.
  void getRelocationTypeAndAddend(uint8_t *FixupAddress,
                                  uint64_t LLVMRelocationType,
                                  uint64_t *EERelocationType, size_t *Addend);

  /// \brief Extract stack offsets for locals
  ///
  /// \param CU Dwarf Unit where locals exist
  /// \param DebugEntry Debug Entry to look for local info
  /// \param Offsets [out] List of offsets to record local info
  void extractLocalLocations(const DWARFUnit *CU,
                             const DWARFDebugInfoEntryMinimal *DebugEntry,
                             std::vector<uint64_t> &Offsets);

  /// \brief Extract debug info for locals from DWARF sections
  ///
  /// \param DwarfContext Dwarf Context to extract debug info from
  /// \param Size Size of the function we are gathering info from
  void getDebugInfoForLocals(DWARFContextInMemory &DwarfContext, uint64_t Addr,
                             uint64_t Size);

  /// \brief Convert DWARF register number to CLR register number
  ///
  /// \param DwarfRegister Register number to convert
  ICorDebugInfo::RegNum mapDwarfRegisterToRegNum(uint8_t DwarfRegister);

  /// \brief Find the Subprogram DebugInfoEntry in list of DIEs
  ///
  /// \param DebugEntry DebugInfoEntry to start from
  const DWARFDebugInfoEntryMinimal *
  getSubprogramDIE(const DWARFDebugInfoEntryMinimal *DebugEntry);

private:
  LLILCJitContext *Context;
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

LLILCJitContext::LLILCJitContext(LLILCJitPerThreadState *PerThreadState)
    : HasLoadedBitCode(false), State(PerThreadState) {
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

  if (JitOptions.IsBreakMethod) {
    dbgs() << "INFO:  Breaking for method " << Context.MethodName << "\n";
  }
  if (JitOptions.IsMSILDumpMethod) {
    dbgs() << "INFO:  Dumping MSIL for method " << Context.MethodName << "\n";
    ReaderBase::printMSIL(MethodInfo->ILCode, 0, MethodInfo->ILCodeSize);
  }
  CorJitResult Result = CORJIT_INTERNALERROR;
  if (JitOptions.IsAltJit && !JitOptions.IsExcludeMethod) {
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
    bool IsNgen = Context.Flags & CORJIT_FLG_PREJIT;
    bool IsReadyToRun = Context.Flags & CORJIT_FLG_READYTORUN;

    // Optimal code for ReadyToRun should have all calls in call [rel32] form to
    // enable crossgen to use shared delay-load thunks. We can't guarantee that
    // LLVM will always generate this form so we currently don't take advantage
    // of that and use non-shared delay-load thunks. The plan is to have as many
    // calls as possible in that form and use shared delay-load thunks when
    // possible. Setting OptLevel to Default increases the chances of calls via
    // memory and setting CodeModel to Default enables rel32 relocations.
    if ((Context.Options->OptLevel != ::OptLevel::DEBUG_CODE) || IsNgen ||
        IsReadyToRun) {
      OptLevel = CodeGenOpt::Level::Default;
    } else {
      OptLevel = CodeGenOpt::Level::None;
      // Options.NoFramePointerElim = true;
    }
    llvm::CodeModel::Model CodeModel =
        (IsNgen || IsReadyToRun) ? CodeModel::Default : CodeModel::JITDefault;
    TargetMachine *TM =
        TheTarget->createTargetMachine(LLILC_TARGET_TRIPLE, "", "", Options,
                                       Reloc::Default, CodeModel, OptLevel);
    Context.TM = TM;

    // Set target machine datalayout on the method module.
    Context.CurrentModule->setDataLayout(TM->createDataLayout());

    // Construct the jitting layers.
    EEMemoryManager MM(&Context);
    ObjectLoadListener Listener(&Context);
    orc::EEObjectLinkingLayer<decltype(Listener)> Loader(Listener);
    orc::IRCompileLayer<decltype(Loader)> Compiler(Loader,
                                                   orc::LLILCCompiler(*TM));

    // Now jit the method.
    if (Context.Options->DumpLevel == DumpLevel::VERBOSE) {
      dbgs() << "INFO:  jitting method " << Context.MethodName
             << " using LLILCJit\n";
    }
    bool HasMethod = this->readMethod(&Context);

#ifndef FEATURE_VERIFICATION
  bool IsImportOnly = (Context.Flags & CORJIT_FLG_IMPORT_ONLY) != 0;
  // If asked to verify, report that it is verifiable.
  if (IsImportOnly) {
    Result = CORJIT_OK;

    CorInfoMethodRuntimeFlags verFlag;
    verFlag = CORINFO_FLG_VERIFIABLE;

    JitInfo->setMethodAttribs(MethodInfo->ftn, verFlag);

    return Result;
  }
#endif

    if (HasMethod) {
      if (JitOptions.IsLLVMDumpMethod) {
        dbgs() << "INFO:  Dumping LLVM for method " << Context.MethodName
               << "\n";
        Context.CurrentModule->dump();
      }
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
      if (JitOptions.IsCodeRangeMethod) {
        errs() << "LLILC compiled: "
               << ", Entry = " << *NativeEntry
               << ", End = " << (*NativeEntry + *NativeSizeOfCode)
               << ", size = " << *NativeSizeOfCode
               << " method = " << Context.MethodName << '\n';
      }

      assert(*NativeEntry >= MM.getHotCodeBlock());
      GcInfoAllocator GcInfoAllocator;
      GCInfo GcInfo(&Context, MM.getStackMapSection(), &GcInfoAllocator,
                    *NativeEntry - MM.getHotCodeBlock());
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
      dbgs() << "INFO:  skipping jitting method " << Context.MethodName
             << " using LLILCJit\n";
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

  bool IsOk = !verifyModule(*JitContext->CurrentModule, &dbgs());
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
    JitContext->CurrentModule->dump();
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
    ErrorOr<uint64_t> AddrOrError = Symbol.getAddress();
    if (!AddrOrError) {
      continue; // Error.
    }
    uint64_t Addr = AddrOrError.get();
    uint64_t Size = Pair.second;

    uint32_t LastDebugOffset = (uint32_t)-1;
    uint32_t NumDebugRanges = 0;
    ICorDebugInfo::OffsetMapping *OM;

    DILineInfoTable Lines = DwarfContext.getLineInfoForAddressRange(Addr, Size);

    DILineInfoTable::iterator Begin = Lines.begin();
    DILineInfoTable::iterator End = Lines.end();

    // Count offset entries. Will skip an entry if the current IL offset
    // matches the previous offset.
    for (DILineInfoTable::iterator It = Begin; It != End; ++It) {
      uint32_t LineNumber = (It->second).Line;

      if (LineNumber != LastDebugOffset) {
        NumDebugRanges++;
        LastDebugOffset = LineNumber;
      }
    }

    // Reset offset
    LastDebugOffset = (uint32_t)-1;

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
        uint32_t LineNumber = (It->second).Line;

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

    getDebugInfoForLocals(DwarfContext, Addr, Size);
  }
}

void ObjectLoadListener::recordRelocations(
    const ObjectFile &Obj, const RuntimeDyld::LoadedObjectInfo &L) {
  for (section_iterator SI = Obj.section_begin(), SE = Obj.section_end();
       SI != SE; ++SI) {
    section_iterator Section = SI->getRelocatedSection();

    if (Section == SE) {
      continue;
    }

    StringRef SectionName;
    std::error_code ErrorCode = Section->getName(SectionName);
    if (ErrorCode) {
      assert(false && ErrorCode.message().c_str());
    }

    if (SectionName.startswith(".debug") ||
        SectionName.startswith(".rela.debug") ||
        !SectionName.compare(".pdata") ||
        SectionName.startswith(".rela.eh_frame")) {
      // Skip sections whose contents are not directly reported to the EE
      continue;
    }

    relocation_iterator I = SI->relocation_begin();
    relocation_iterator E = SI->relocation_end();

    for (; I != E; ++I) {
      symbol_iterator Symbol = I->getSymbol();
      assert(Symbol != Obj.symbol_end());
      ErrorOr<section_iterator> SymbolSectionOrErr = Symbol->getSection();
      assert(!SymbolSectionOrErr.getError());
      object::section_iterator SymbolSection = *SymbolSectionOrErr;
      const bool IsExtern = SymbolSection == Obj.section_end();
      uint64_t RelType = I->getType();
      uint64_t Offset = I->getOffset();
      uint8_t *RelocationTarget = nullptr;
      if (IsExtern) {
        // This is an external symbol. Verify that it's one we created for
        // a global variable and report the relocation via Jit interface.
        ErrorOr<StringRef> NameOrError = Symbol->getName();
        assert(NameOrError);
        StringRef TargetName = NameOrError.get();
        assert(Context->NameToHandleMap.count(TargetName) == 1);
        RelocationTarget = (uint8_t *)Context->NameToHandleMap[TargetName];
      } else {
        RelocationTarget = (uint8_t *)(L.getSectionLoadAddress(*SymbolSection) +
                                       Symbol->getValue());
      }

      uint64_t SectionAddress = L.getSectionLoadAddress(*Section);
      assert(SectionAddress != 0);
      uint8_t *FixupAddress = (uint8_t *)(SectionAddress + Offset);
      size_t Addend;
      uint64_t EERelType;

      getRelocationTypeAndAddend(FixupAddress, RelType, &EERelType, &Addend);

      Context->JitInfo->recordRelocation(FixupAddress,
                                         RelocationTarget + Addend, EERelType);
    }
  }
}

void ObjectLoadListener::getRelocationTypeAndAddend(uint8_t *FixupAddress,
                                                    uint64_t LLVMRelocationType,
                                                    uint64_t *EERelocationType,
                                                    size_t *Addend) {
  switch (LLVMRelocationType) {
  case IMAGE_REL_AMD64_ABSOLUTE:
    *EERelocationType = IMAGE_REL_BASED_ABSOLUTE;
    *Addend = *(uint32_t *)FixupAddress;
    break;
  case IMAGE_REL_AMD64_ADDR64:
    *EERelocationType = IMAGE_REL_BASED_DIR64;
    *Addend = *(uint64_t *)FixupAddress;
    break;
  case IMAGE_REL_AMD64_REL32:
    *EERelocationType = IMAGE_REL_BASED_REL32;
    *Addend = *(uint32_t *)FixupAddress;
    break;

  default:
    llvm_unreachable("Unknown reloc type.");
  }
}

void ObjectLoadListener::getDebugInfoForLocals(
    DWARFContextInMemory &DwarfContext, uint64_t Addr, uint64_t Size) {
  for (const auto &CU : DwarfContext.compile_units()) {
    const DWARFDebugInfoEntryMinimal *UnitDIE = CU->getUnitDIE(false);
    const DWARFDebugInfoEntryMinimal *SubprogramDIE = getSubprogramDIE(UnitDIE);

    ICorDebugInfo::RegNum FrameBaseRegister = ICorDebugInfo::REGNUM_COUNT;
    DWARFFormValue FormValue;

    // Find the frame_base register value
    if (SubprogramDIE->getAttributeValue(CU.get(), dwarf::DW_AT_frame_base,
                                         FormValue)) {
      Optional<ArrayRef<uint8_t>> FormValues = FormValue.getAsBlock();
      FrameBaseRegister = mapDwarfRegisterToRegNum(FormValues->back());
    }

    if (SubprogramDIE->getAttributeValue(CU.get(), dwarf::DW_AT_low_pc,
                                         FormValue)) {
      Optional<uint64_t> FormAddress = FormValue.getAsAddress(CU.get());

      // If the Form address doesn't match the address for the function passed
      // do not collect debug for locals since they do not go with the current
      // function being processed
      if (FormAddress.getValue() != Addr) {
        return;
      }
    }

    std::vector<uint64_t> Offsets;
    extractLocalLocations(CU.get(), SubprogramDIE, Offsets);

    // Allocate the array of NativeVarInfo objects that will be sent to the EE
    ICorDebugInfo::NativeVarInfo *LocalVars;
    unsigned SizeOfArray =
        Offsets.size() * sizeof(ICorDebugInfo::NativeVarInfo);
    if (SizeOfArray > 0) {
      LocalVars =
          (ICorDebugInfo::NativeVarInfo *)Context->JitInfo->allocateArray(
              SizeOfArray);

      unsigned CurrentDebugEntry = 0;

      for (auto &Offset : Offsets) {
        LocalVars[CurrentDebugEntry].startOffset = Addr;
        LocalVars[CurrentDebugEntry].endOffset = Addr + Size;
        LocalVars[CurrentDebugEntry].varNumber = CurrentDebugEntry;

        // Assume all locals are on the stack
        ICorDebugInfo::VarLoc VarLoc;
        VarLoc.vlType = ICorDebugInfo::VLT_STK;
        VarLoc.vlStk.vlsBaseReg = FrameBaseRegister;
        VarLoc.vlStk.vlsOffset = Offset;

        LocalVars[CurrentDebugEntry].loc = VarLoc;

        CurrentDebugEntry++;
      }

      CORINFO_METHOD_INFO *MethodInfo = Context->MethodInfo;
      CORINFO_METHOD_HANDLE MethodHandle = MethodInfo->ftn;

      Context->JitInfo->setVars(MethodHandle, Offsets.size(), LocalVars);
    }
  }
}

void ObjectLoadListener::extractLocalLocations(
    const DWARFUnit *CU, const DWARFDebugInfoEntryMinimal *DebugEntry,
    std::vector<uint64_t> &Offsets) {
  if (DebugEntry->isNULL())
    return;
  if (DebugEntry->getTag() == dwarf::DW_TAG_formal_parameter ||
      DebugEntry->getTag() == dwarf::DW_TAG_variable) {
    uint64_t Offset;

    // Extract offset for each local found
    DWARFFormValue FormValue;
    if (DebugEntry->getAttributeValue(CU, dwarf::DW_AT_location, FormValue)) {
      Optional<ArrayRef<uint8_t>> FormValues = FormValue.getAsBlock();
      Offset = FormValues->back();
    }

    Offsets.push_back(Offset);
  }

  const DWARFDebugInfoEntryMinimal *Child = DebugEntry->getFirstChild();

  // Extract info for DebugEntry's child and its child's siblings.
  while (Child) {
    extractLocalLocations(CU, Child, Offsets);
    Child = Child->getSibling();
  }
}

const DWARFDebugInfoEntryMinimal *ObjectLoadListener::getSubprogramDIE(
    const DWARFDebugInfoEntryMinimal *DebugEntry) {
  if (DebugEntry->isSubprogramDIE())
    return DebugEntry;
  else if (DebugEntry->hasChildren())
    return getSubprogramDIE(DebugEntry->getFirstChild());
  else
    return nullptr;
}

ICorDebugInfo::RegNum
ObjectLoadListener::mapDwarfRegisterToRegNum(uint8_t DwarfRegister) {
  ICorDebugInfo::RegNum Register = ICorDebugInfo::REGNUM_COUNT;
#if _TARGET_AMD64_
  switch (DwarfRegister) {
  case dwarf::DW_OP_reg0:
    Register = ICorDebugInfo::REGNUM_RAX;
    break;
  case dwarf::DW_OP_reg1:
    Register = ICorDebugInfo::REGNUM_RDX;
    break;
  case dwarf::DW_OP_reg2:
    Register = ICorDebugInfo::REGNUM_RCX;
    break;
  case dwarf::DW_OP_reg3:
    Register = ICorDebugInfo::REGNUM_RBX;
    break;
  case dwarf::DW_OP_reg4:
    Register = ICorDebugInfo::REGNUM_RSI;
    break;
  case dwarf::DW_OP_reg5:
    Register = ICorDebugInfo::REGNUM_RDI;
    break;
  case dwarf::DW_OP_reg6:
    Register = ICorDebugInfo::REGNUM_RBP;
    break;
  case dwarf::DW_OP_reg7:
    Register = ICorDebugInfo::REGNUM_RSP;
    break;
  case dwarf::DW_OP_reg8:
    Register = ICorDebugInfo::REGNUM_R8;
    break;
  case dwarf::DW_OP_reg9:
    Register = ICorDebugInfo::REGNUM_R9;
    break;
  case dwarf::DW_OP_reg10:
    Register = ICorDebugInfo::REGNUM_R10;
    break;
  case dwarf::DW_OP_reg11:
    Register = ICorDebugInfo::REGNUM_R11;
    break;
  case dwarf::DW_OP_reg12:
    Register = ICorDebugInfo::REGNUM_R12;
    break;
  case dwarf::DW_OP_reg13:
    Register = ICorDebugInfo::REGNUM_R13;
    break;
  case dwarf::DW_OP_reg14:
    Register = ICorDebugInfo::REGNUM_R14;
    break;
  case dwarf::DW_OP_reg15:
    Register = ICorDebugInfo::REGNUM_R15;
    break;
  default:
    Register = ICorDebugInfo::REGNUM_COUNT;
    break;
  }
#endif
  return Register;
}

unsigned LLILCJit::getMaxIntrinsicSIMDVectorLength(DWORD CpuCompileFlags) {
  return getLLILCJitContext()->Options->PreferredIntrinsicSIMDVectorLength;
}
