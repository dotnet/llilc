//===---- lib/Reader/readerir.cpp -------------------------------*- C++ -*-===//
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
/// \brief Convert from MSIL bytecode to LLVM IR.
///
//===----------------------------------------------------------------------===//

#include "earlyincludes.h"
#include "readerir.h"
#include "imeta.h"
#include "newvstate.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Debug.h"            // for dbgs()
#include "llvm/Support/Format.h"           // for format()
#include "llvm/Support/raw_ostream.h"      // for errs()
#include "llvm/Support/ConvertUTF.h"       // for ConvertUTF16toUTF8
#include "llvm/Transforms/Utils/Cloning.h" // for CloneBasicBlock/RemapInstr
#include "llvm/Transforms/Utils/Local.h"   // for removeUnreachableBlocks
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <cstdlib>
#include <new>

using namespace llvm;

#pragma region READER STACK MODEL

//===----------------------------------------------------------------------===//
//
// MSIL Reader Stack
//
//===----------------------------------------------------------------------===//

GenStack::GenStack(uint32_t MaxStack, ReaderBase *Rdr) {
  Reader = Rdr;
  Stack.reserve(MaxStack);
}

void GenStack::push(IRNode *NewVal) {
  ASSERT(NewVal != nullptr);
  ASSERT(GenIR::isValidStackType(NewVal));
  Stack.push_back(NewVal);
}

IRNode *GenStack::pop() {
  ASSERTM(size() > 0, "stack underflow");
  if (size() == 0) {
    LLILCJit::fatal(CORJIT_BADCODE);
  }

  IRNode *Result = Stack.back();
  Stack.pop_back();

  return Result;
}

void GenStack::assertEmpty() { ASSERT(empty()); }

#if !defined(NODEBUG)
void GenStack::print() {
  dbgs() << "{GenStack dump, Top first, size = " << size() << '\n';
  int32_t I = 0;
  for (auto N : *this) {
    dbgs() << "[" << I++ << "]: ";
    Reader->dbPrintIRNode(N);
  }
  dbgs() << "}\n";
}
#endif

ReaderStack *GenStack::copy() {
  void *Buffer = Reader->getTempMemory(sizeof(GenStack));
  GenStack *Copy = new (Buffer) GenStack(Stack.capacity(), Reader);
  for (auto Value : *this) {
    Copy->push(Value);
  }
  return Copy;
}

ReaderStack *GenIR::createStack() {
  void *Buffer = getTempMemory(sizeof(GenStack));
  return new (Buffer) GenStack(4, this);
}

#pragma endregion

#pragma region EH REGION BUILDER

//===----------------------------------------------------------------------===//
//
// MSIL Reader EH Region Builder
//
//===----------------------------------------------------------------------===//

struct EHRegion {
public:
  EHRegion *Parent;
  EHRegionList *Children;
  uint32_t StartMsilOffset;
  uint32_t EndMsilOffset;
  llvm::Instruction *HandlerEHPad;
  union {
    llvm::CatchPadInst *CatchPad;     // used for catch and filter-handler
    llvm::CleanupPadInst *CleanupPad; // Used for finally and fault
  };
  union {
    EHRegion *EntryRegion;              // Only used for Try regions
    llvm::SwitchInst *EndFinallySwitch; // Only used for Finally regions
    mdToken CatchClassToken;            // Only used for Catch regions
    EHRegion *HandlerRegion;            // Only used for Filter regions
    llvm::Value *Exception;             // Only used for Filter-Handler regions
  };
  ReaderBaseNS::RegionKind Kind;
};

struct EHRegionList {
public:
  EHRegion *Region;
  EHRegionList *NextRegionList;
};

EHRegion *GenIR::rgnAllocateRegion() {
  // TODO: Using ProcMemory here.
  // Do we really want these Region objects to persist?
  return (EHRegion *)getProcMemory(sizeof(EHRegion));
}

EHRegionList *GenIR::rgnAllocateRegionList() {
  // TODO: Using ProcMemory here.
  // Do we really want these Region objects to persist?
  return (EHRegionList *)getProcMemory(sizeof(EHRegionList));
}

EHRegionList *rgnListGetNext(EHRegionList *RegionList) {
  return RegionList->NextRegionList;
}

void rgnListSetNext(EHRegionList *RegionList, EHRegionList *Next) {
  RegionList->NextRegionList = Next;
}

EHRegion *rgnListGetRgn(EHRegionList *RegionList) { return RegionList->Region; }

void rgnListSetRgn(EHRegionList *RegionList, EHRegion *Region) {
  RegionList->Region = Region;
}

ReaderBaseNS::RegionKind rgnGetRegionType(EHRegion *Region) {
  return Region->Kind;
}

void rgnSetRegionType(EHRegion *Region, ReaderBaseNS::RegionKind Type) {
  Region->Kind = Type;
}

uint32_t rgnGetStartMSILOffset(EHRegion *Region) {
  return Region->StartMsilOffset;
}

void rgnSetStartMSILOffset(EHRegion *Region, uint32_t Offset) {
  Region->StartMsilOffset = Offset;
}

uint32_t rgnGetEndMSILOffset(EHRegion *Region) { return Region->EndMsilOffset; }

void rgnSetEndMSILOffset(EHRegion *Region, uint32_t Offset) {
  Region->EndMsilOffset = Offset;
}

void rgnSetParent(EHRegion *Region, EHRegion *Parent) {
  Region->Parent = Parent;
}

EHRegion *rgnGetParent(EHRegion *Region) { return Region->Parent; }

bool rgnIsOutsideParent(EHRegion *Region) {
  switch (Region->Kind) {
  case ReaderBaseNS::RegionKind::RGN_Fault:
  case ReaderBaseNS::RegionKind::RGN_Finally:
  case ReaderBaseNS::RegionKind::RGN_Filter:
  case ReaderBaseNS::RegionKind::RGN_MExcept:
  case ReaderBaseNS::RegionKind::RGN_MCatch:
    return true;
  default:
    return false;
  }
}

EHRegion *rgnGetEnclosingAncestor(EHRegion *Region) {
  EHRegion *OuterRegion = rgnGetParent(Region);
  if (rgnIsOutsideParent(Region)) {
    // We want the enclosing ancestor, not the immediate parent.
    OuterRegion = rgnGetParent(OuterRegion);
  }
  return OuterRegion;
}

void rgnSetChildList(EHRegion *Region, EHRegionList *Children) {
  Region->Children = Children;
}
EHRegionList *rgnGetChildList(EHRegion *Region) { return Region->Children; }

EHRegion *rgnGetFilterHandlerRegion(EHRegion *Region) {
  return Region->HandlerRegion;
}
void rgnSetFilterHandlerRegion(EHRegion *Region, EHRegion *Handler) {
  Region->HandlerRegion = Handler;
}
mdToken rgnGetCatchClassToken(EHRegion *CatchRegion) {
  return CatchRegion->CatchClassToken;
}
void rgnSetCatchClassToken(EHRegion *CatchRegion, mdToken Token) {
  CatchRegion->CatchClassToken = Token;
}

void rgnSetEntryRegion(EHRegion *TryRegion, EHRegion *EntryRegion) {
  assert(TryRegion->Kind == ReaderBaseNS::RegionKind::RGN_Try);
  TryRegion->EntryRegion = EntryRegion;
}
EHRegion *rgnGetEntryRegion(EHRegion *TryRegion) {
  assert(TryRegion->Kind == ReaderBaseNS::RegionKind::RGN_Try);
  return TryRegion->EntryRegion;
}

#pragma endregion

#pragma region MEMORY ALLOCATION

//===----------------------------------------------------------------------===//
//
// MSIL Reader memory allocation helpers
//
//===----------------------------------------------------------------------===//

// Get memory that will be freed at end of reader

void *GenIR::getTempMemory(size_t NumBytes) { return calloc(1, NumBytes); }

// Get memory that will persist after the reader
void *GenIR::getProcMemory(size_t NumBytes) { return calloc(1, NumBytes); }

#pragma endregion

#pragma region READER PASSES

//===----------------------------------------------------------------------===//
//
// MSIL Reader Passes
//
//===----------------------------------------------------------------------===//

static Argument *functionArgAt(Function *F, uint32_t I) {
  Function::arg_iterator Args = F->arg_begin();
  for (; I > 0; I--) {
    ++Args;
  }
  return &*Args;
}

static bool doesArgumentHaveHome(const ABIArgInfo &Info) {
  switch (Info.getKind()) {
  case ABIArgInfo::Indirect:
    return false;

  case ABIArgInfo::Direct:
    return !Info.getType()->isStructTy() && !Info.getType()->isVectorTy();

  default:
    return true;
  }
}

void GenIR::readerPrePass(uint8_t *Buffer, uint32_t NumBytes) {
  Triple PT(Triple::normalize(LLVM_DEFAULT_TARGET_TRIPLE));
  if (PT.isArch16Bit()) {
    TargetPointerSizeInBits = 16;
  } else if (PT.isArch32Bit()) {
    TargetPointerSizeInBits = 32;
  } else if (PT.isArch64Bit()) {
    TargetPointerSizeInBits = 64;
  } else {
    ASSERTNR(UNREACHED);
  }

  const uint32_t JitFlags = JitContext->Flags;

  CORINFO_METHOD_HANDLE MethodHandle = JitContext->MethodInfo->ftn;

  // Capture low-level info about the return type for use in Return.
  CORINFO_SIG_INFO Sig;
  getMethodSig(MethodHandle, &Sig);

  bool HasSecretParameter = (JitFlags & CORJIT_FLG_PUBLISH_SECRET_PARAM) != 0;

  uint32_t NumLocals = 0;
  initMethodInfo(HasSecretParameter, MethodSignature, NumLocals);

  if (this->MethodSignature.isVarArg()) {
    throw NotYetImplementedException("Vararg method");
  }

  new (&ABIMethodSig)
      ABIMethodSignature(MethodSignature, *this, *JitContext->TheABIInfo);
  Function = ABIMethodSig.createFunction(*this, *JitContext->CurrentModule);
  GcFuncInfo = JitContext->GcInfo->newGcInfo(Function);

  llvm::LLVMContext &LLVMContext = *JitContext->LLVMContext;
  EntryBlock = BasicBlock::Create(LLVMContext, "entry", Function);

  LLVMBuilder = new IRBuilder<>(LLVMContext);

  DBuilder = new DIBuilder(*JitContext->CurrentModule);
  LLILCDebugInfo.TheCU = DBuilder->createCompileUnit(dwarf::DW_LANG_C_plus_plus,
                                                     Function->getName().str(),
                                                     ".", "LLILCJit", 0, "", 0);

  LLVMBuilder->SetInsertPoint(EntryBlock);

  // Note numArgs may exceed the IL argument count when there
  // are hidden args like the varargs cookie or type descriptor.
  uint32_t NumArgs = MethodSignature.getArgumentTypes().size();
  ASSERT(NumArgs >= JitContext->MethodInfo->args.totalILArgs());

  Arguments.resize(NumArgs);
  LocalVars.resize(NumLocals);
  LocalVarCorTypes.resize(NumLocals);
  KeepGenericContextAlive = false;
  NeedsStackSecurityCheck = HasLocAlloc;
  NeedsSecurityObject = false;
  DoneBuildingFlowGraph = false;
  UnreachableContinuationBlock = nullptr;
  // Personality function is created on-demand.
  PersonalityFunction = nullptr;

  // Setup function for emiting debug locations
  DIFile *Unit = DBuilder->createFile(LLILCDebugInfo.TheCU->getFilename(),
                                      LLILCDebugInfo.TheCU->getDirectory());
  bool IsOptimized = JitContext->Options->EnableOptimization;
  DIScope *FContext = Unit;
  unsigned LineNo = 0;
  unsigned ScopeLine = ICorDebugInfo::PROLOG;
  bool IsDefinition = true;
  DISubprogram *SP = DBuilder->createFunction(
      FContext, Function->getName(), StringRef(), Unit, LineNo,
      createFunctionType(Function, Unit), Function->hasInternalLinkage(),
      IsDefinition, ScopeLine, DINode::FlagPrototyped, IsOptimized);

  LLILCDebugInfo.FunctionScope = SP;

  initParamsAndAutos(MethodSignature);

  // Add storage for the indirect result, if any.
  const ABIArgInfo &ResultInfo = ABIMethodSig.getResultInfo();
  bool HasIndirectResult = ResultInfo.getKind() == ABIArgInfo::Indirect;
  uint32_t IndirectResultIndex = (uint32_t)-1;
  if (HasIndirectResult) {
    IndirectResultIndex = ResultInfo.getIndex();
    IndirectResult = functionArgAt(Function, IndirectResultIndex);
  } else {
    IndirectResult = nullptr;
  }

  // Take note of the current insertion point in case we need
  // to add more allocas later.
  if (EntryBlock->empty()) {
    AllocaInsertionPoint = nullptr;
  } else {
    AllocaInsertionPoint = &EntryBlock->back();
  }

  // Home the method arguments.
  Function::arg_iterator ArgI = Function->arg_begin();
  Function::arg_iterator ArgE = Function->arg_end();
  for (uint32_t I = 0, J = 0; ArgI != ArgE; ++I, ++ArgI) {
    if (HasIndirectResult && I == IndirectResultIndex) {
      // Indirect results aren't homed.
      continue;
    }

    const ABIArgInfo &ArgInfo = ABIMethodSig.getArgumentInfo(J);
    switch (ArgInfo.getKind()) {
    case ABIArgInfo::Indirect:
      // Indirect arguments aren't homed.
      assert(!doesArgumentHaveHome(ArgInfo));
      break;

    case ABIArgInfo::Expand: {
      Type *BytePtrType = Type::getInt8PtrTy(LLVMContext, 0);
      Value *Home = LLVMBuilder->CreatePointerCast(Arguments[J], BytePtrType);

      ArrayRef<ABIArgInfo::Expansion> Expansions = ArgInfo.getExpansions();
      uint32_t ExpansionCount = Expansions.size();
      for (uint32_t E = 0; E < ExpansionCount - 1; ++E, ++I, ++ArgI) {
        ABISignature::collapse(*this, Expansions[E], &*ArgI, Home);
      }
      ABISignature::collapse(*this, Expansions[ExpansionCount - 1], &*ArgI,
                             Home);
      break;
    }

    default: {
      if (ArgInfo.getType()->isStructTy() || ArgInfo.getType()->isVectorTy()) {
        // LLVM byvalue struct arguments aren't homed.
        assert(!doesArgumentHaveHome(ArgInfo));
        assert(ArgInfo.getKind() == ABIArgInfo::Direct);
        assert(ArgI->getType()->isPointerTy());
        assert(ArgI->getType()->getPointerElementType() == ArgInfo.getType());
        assert(ArgI->hasByValAttr());
      } else {
        Value *Arg = &*ArgI;
        Value *Home = Arguments[J];
        Type *HomeType = Home->getType()->getPointerElementType();

        Arg = ABISignature::coerce(*this, HomeType, Arg);
        const bool IsVolatile = false;
        storeAtAddressNoBarrierNonNull((IRNode *)Home, (IRNode *)Arg, HomeType,
                                       IsVolatile);
      }
      break;
    }
    }

    J++;
  }

  zeroInitLocals();

  // Check for special cases where the Jit needs to do extra work.
  const uint32_t MethodFlags = getCurrentMethodAttribs();

  // Check for synchronized method. If a method is synchronized the JIT is
  // required to insert a helper call to MONITOR_ENTER before we start the user
  // code. A similar call to MONITOR_EXIT will be placed at the return site.
  // TODO: when we start catching exceptions we should create a try/fault for
  // the entire method so that we can exit the monitor on unhandled exceptions.
  MethodSyncHandle = nullptr;
  SyncFlag = nullptr;
  if (MethodFlags & CORINFO_FLG_SYNCH) {
    MethodSyncHandle = rdrGetCritSect();
    Type *SyncFlagType = Type::getInt8Ty(LLVMContext);

    // Create address taken local SyncFlag. This will be passed by address to
    // MONITOR_ENTER and MONITOR_EXIT. MONITOR_ENTER will set SyncFlag once lock
    // has been obtained, MONITOR_EXIT only releases lock if SyncFlag is set.
    SyncFlag = createTemporary(SyncFlagType, "SyncFlag");
    Constant *ZeroConst = Constant::getNullValue(SyncFlagType);
    LLVMBuilder->CreateStore(ZeroConst, SyncFlag);

    const bool IsEnter = true;
    callMonitorHelper(IsEnter);
  }

  if (!IsOptimized && !(JitFlags & CORJIT_FLG_IL_STUB)) {
    // Get the handle from the EE.
    bool IsIndirect = false;
    void *DebugHandle =
        getJustMyCodeHandle(getCurrentMethodHandle(), &IsIndirect);

    // If the handle is non-null, insert the hook.
    if (DebugHandle != nullptr) {
      const bool IsCallTarget = false;
      IRNode *JustMyCodeFlagAddress =
          handleToIRNode(mdtJMCHandle, DebugHandle, DebugHandle, IsIndirect,
                         IsIndirect, IsIndirect, IsCallTarget);
      const bool IsVolatile = false;
      const bool IsReadOnly = false;
      IRNode *JustMyCodeFlag =
          loadIndirNonNull(ReaderBaseNS::LdindI4, JustMyCodeFlagAddress,
                           Reader_AlignNatural, IsVolatile, IsReadOnly);
      Value *Condition = LLVMBuilder->CreateIsNotNull(JustMyCodeFlag, "JMC");
      IRNode *Zero = loadConstantI4(0);
      Type *Void = Type::getVoidTy(*JitContext->LLVMContext);
      const bool MayThrow = false;
      const bool CallReturns = true;
      genConditionalHelperCall(
          Condition, CorInfoHelpFunc::CORINFO_HELP_DBG_IS_JUST_MY_CODE,
          MayThrow, Void, JustMyCodeFlag, Zero, CallReturns, "JustMyCodeHook");
    }
  }

  // TODO: Insert class initialization check if necessary
  CorInfoInitClassResult InitResult =
      initClass(nullptr, getCurrentMethodHandle(), getCurrentContext());
  const bool InitClass = InitResult & CORINFO_INITCLASS_USE_HELPER;
  if (InitClass) {
    insertClassConstructor();
  }

  // Split the entry block at this point. The continuation will
  // be the first block to hold the IR for MSIL opcodes and will be
  // the target for MSIL offset 0 branches (and tail recursive calls).

  FlowGraphNode *CurrentFlowGraphNode =
      (FlowGraphNode *)LLVMBuilder->GetInsertBlock();
  Instruction *CurrentInstruction = &*LLVMBuilder->GetInsertPoint();
  IRNode *CurrentIRNode = (IRNode *)CurrentInstruction;
  FirstMSILBlock = fgSplitBlock(CurrentFlowGraphNode, CurrentIRNode);
}

void GenIR::readerMiddlePass() { return; }

void GenIR::readerPostVisit() {
  // Insert IR for some deferred prolog actions.  These logically have offset
  // zero and are outside any try regions.
  CurrInstrOffset = 0;
  CurrentRegion = EhRegionTree;

  // If the generic context must be kept live, make it so.
  if (KeepGenericContextAlive) {
    insertIRToKeepGenericContextAlive();
  }

  // If the method needs a security object, set one up.
  if (NeedsSecurityObject) {
    insertIRForSecurityObject();
  }

  // At this point the IR for finally handlers reflects the right semantics,
  // but is ill-formed because the cleanuppad at the entry of a finally doesn't
  // dominate its uses at the exits from that finally (because there is a
  // side-entry for the non-exceptional paths through the finally).
  // The cleanuppad has 'token' type, and tokens cannot be PHI'd, so we can't
  // just insert PHIs with undef at the side entry.  Instead, we copy the body
  // of the finally so that the exception path and the non-exception path
  // don't shre IR.  The long-term plan is to add a 'cleanupcall' operator
  // to LLVM which the non-exceptional entries can use to target the cleanuppad
  // directly (and subsequently run an optimization that will usually clone
  // out the non-exceptional paths so as to better-optimize them).
  cloneFinallyBodies();

  DBuilder->finalize();
}

void GenIR::cloneFinallyBodies() {
  if (EhRegionTree == nullptr) {
    // No regions => no finallies
    return;
  }

  // Find the finallies, pushing them onto a stack while walking down the
  // region tree so that we'll pop inner finallies before outer ones
  SmallVector<EHRegion *, 8> FinallyRegions;
  SmallVector<EHRegion *, 8> Worklist;
  Worklist.push_back(EhRegionTree);

  while (!Worklist.empty()) {
    EHRegion *Region = Worklist.pop_back_val();
    if (Region->Kind == ReaderBaseNS::RegionKind::RGN_Finally) {
      // Queue finally for cloning
      FinallyRegions.push_back(Region);
    }
    // Queue all child regions to continue searching for finallies
    for (EHRegionList *RegionList = rgnGetChildList(Region); RegionList;
         RegionList = rgnListGetNext(RegionList)) {
      EHRegion *ChildRegion = rgnListGetRgn(RegionList);
      Worklist.push_back(ChildRegion);
    }
  }

  while (!FinallyRegions.empty()) {
    EHRegion *Finally = FinallyRegions.pop_back_val();
    cloneFinallyBody(Finally);
  }

  // As a temporary measure during EH bring-up, insert a FAIL_FAST at the start
  // of each unsupported exception handler; this makes it easier to diagnose
  // whether a failing test might be failing because it was supposed to catch
  // an exception.
  for (BasicBlock &BB : *Function) {
    if (BB.empty()) {
      continue;
    }
    Instruction *First = BB.getFirstNonPHI();
    if (!isa<CatchPadInst>(First) && !isa<CleanupPadInst>(First)) {
      continue;
    }
    if (canExecuteHandler(BB)) {
      continue;
    }

    LLVMBuilder->SetInsertPoint(&BB, std::next(First->getIterator()));
    Type *VoidType = Type::getVoidTy(*JitContext->LLVMContext);
    const bool MayThrow = false;
    callHelperImpl(CORINFO_HELP_FAIL_FAST, MayThrow, VoidType);
  }
}

void GenIR::cloneFinallyBody(EHRegion *FinallyRegion) {
  CleanupPadInst *Cleanup = FinallyRegion->CleanupPad;
  SwitchInst *ExitSwitch = FinallyRegion->EndFinallySwitch;

  // Find the exceptional exits
  BasicBlock *CleanupRetBlock = nullptr;

  for (User *U : Cleanup->users()) {
    if (isa<CleanupReturnInst>(U)) {
      assert(CleanupRetBlock == nullptr && "Expected just one cleanupret");
      CleanupRetBlock = cast<Instruction>(U)->getParent();
      // Check that the switch is the only predecessor of the cleanupret, since
      // this is assumed below when we rewrite edges targeting it.
      assert(CleanupRetBlock->getSinglePredecessor() ==
                 ExitSwitch->getParent() &&
             "Expected cleanupret only reached via exit switch");
    }
  }
  assert(CleanupRetBlock && "Could not find cleanupret");

  // Walk the flow graph to find the blocks that make up the finally.
  // Don't include the cleanuppad itself or the cleanupret,
  // since those only need to exist on the exception path.
  SmallPtrSet<BasicBlock *, 8> FinallyBlocks;
  SmallVector<BasicBlock *, 4> Worklist;

  Worklist.push_back(Cleanup->getParent());
  BasicBlock *ExitSwitchBlock = ExitSwitch->getParent();

  while (!Worklist.empty()) {
    BasicBlock *Block = Worklist.pop_back_val();
    for (BasicBlock *Succ : successors(Block)) {
      if (Succ->isEHPad()) {
        // Need to figure out if this unwind edge exits the finally or not.
        // If it stays within the finally, then its target will be a descendant
        // of the finally per the parent linkage on the EH pads.
        bool FoundAncestry = false;
        for (Value *Pad = Succ->getFirstNonPHI(); !isa<ConstantTokenNone>(Pad);
             Pad = isa<CatchSwitchInst>(Pad)
                       ? cast<CatchSwitchInst>(Pad)->getParentPad()
                       : cast<FuncletPadInst>(Pad)->getParentPad()) {
          if (Pad == Cleanup) {
            FoundAncestry = true;
            break;
          }
        }
        if (!FoundAncestry) {
          // This EH edge exits the funclet, so we do not need to clone the
          // successor block.
          continue;
        }
      }
      if (FinallyBlocks.insert(Succ).second) {
        if (Succ == ExitSwitchBlock) {
          // Don't need to clone past the switch.
          continue;
        }
        Worklist.push_back(Succ);
      }
    }
  }

  // Clone the blocks using the CloneBasicBlock utility.  This makes straight
  // copies of the blocks but builds up a map from original Values to cloned
  // Values.
  SmallVector<BasicBlock *, 16> CloneBlocks;
  ValueToValueMapTy ValueMap;
  for (BasicBlock *Original : FinallyBlocks) {
    BasicBlock *Clone = CloneBasicBlock(Original, ValueMap, ".non-exn");
    Clone->insertInto(Function, Original);
    ValueMap[Original] = Clone;
    CloneBlocks.push_back(Clone);
    // Tell the reader to treat the clone as visited if the original was.
    if (fgNodeIsVisited((FlowGraphNode *)Original)) {
      fgNodeSetVisited((FlowGraphNode *)Clone, true);
    }
  }

  // The cleanupret is targeted just by the switch in the case that handles
  // the exceptional continuation, so on the non-exception path it can target
  // the `unreachable` that is the switch default instead.
  assert(isa<UnreachableInst>(ExitSwitch->getDefaultDest()->getFirstNonPHI()));
  ValueMap[CleanupRetBlock] = ExitSwitch->getDefaultDest();

  // Map the finally itself to its parent funclet, so that any calls or nested
  // funclets which reference the finally being cloned as their parent will
  // have their clones on the non-exception path reference the outer parent
  // instead.
  Value *ParentPad = Cleanup->getParentPad();
  bool TrueParentIsNone = isa<ConstantTokenNone>(ParentPad);
  if (TrueParentIsNone) {
    // Any call/invoke bundles referring to the cleanup will need to be
    // removed.  Place a sentinel in the map to make them easy to find and
    // fix up later.
    ParentPad = CleanupPadInst::Create(ParentPad);
  }
  ValueMap[Cleanup] = ParentPad;

  // Now apply the value map fixups to the cloned instructions using the
  // RemapInstruction utility.
  for (BasicBlock *CloneBlock : CloneBlocks) {
    for (Instruction &CloneInstr : *CloneBlock) {
      RemapInstruction(&CloneInstr, ValueMap,
                       RF_IgnoreMissingEntries | RF_NoModuleLevelChanges);
    }
  }

  if (TrueParentIsNone) {
    // Remove references to the sentinel parent.
    SmallVector<Instruction *, 8> ReplacedInstructions;
    for (User *U : ParentPad->users()) {
      Instruction *OldInstruction;
      Instruction *NewInstruction;
      if (auto *Call = dyn_cast<CallInst>(U)) {
        OldInstruction = Call;
        NewInstruction = CallInst::Create(Call, {}, Call);
      } else if (auto *Invoke = dyn_cast<InvokeInst>(U)) {
        OldInstruction = Invoke;
        NewInstruction = InvokeInst::Create(Invoke, {}, Invoke);
      } else {
        continue;
      }
      NewInstruction->setDebugLoc(OldInstruction->getDebugLoc());
      OldInstruction->replaceAllUsesWith(NewInstruction);
      // Make sure the funclet bundle is the only one we're removing
      assert(CallSite(OldInstruction).getNumOperandBundles() == 1);
      assert(CallSite(OldInstruction).getOperandBundleAt(0).getTagID() ==
             LLVMContext::OB_funclet);
      // Queue the old instruction for deferred removal so we don't messs up
      // the user iterator.
      ReplacedInstructions.push_back(OldInstruction);
    }
    for (Instruction *OldInstruction : ReplacedInstructions) {
      OldInstruction->eraseFromParent();
    }

    // Any remaining users should be child EH pads, which need to get their
    // parent set to "none"
    assert(llvm::all_of(ParentPad->users(), [](User *U) {
      return cast<Instruction>(U)->isEHPad();
    }));
    ConstantTokenNone *TrueParent =
        ConstantTokenNone::get(*JitContext->LLVMContext);
    ParentPad->replaceAllUsesWith(TrueParent);
    delete ParentPad;
    ParentPad = TrueParent;
  }

  // Now the clone should hold the right code for the non-exceptional path,
  // so update all non-exceptional entries to target it.
  BasicBlock *PadBlock = Cleanup->getParent();
  BasicBlock *HeadBlock = PadBlock->getSingleSuccessor();
  BasicBlock *NewHead =
      static_cast<BasicBlock *>(static_cast<Value *>(ValueMap[HeadBlock]));
  for (Value::use_iterator UI = HeadBlock->use_begin(),
                           UE = HeadBlock->use_end();
       UI != UE;) {
    // Increment iterator here because we may delete the use
    Use &U = *UI++;
    BasicBlock *UseBlock = cast<Instruction>(U.getUser())->getParent();
    if ((UseBlock != PadBlock) && !FinallyBlocks.count(UseBlock)) {
      U.set(NewHead);
    }
  }

  // The switch in the exceptional path is no longer necessary and can just
  // branch to the cleanupret.
  IRBuilder<> Builder(ExitSwitch);
  Builder.CreateBr(CleanupRetBlock);
  ExitSwitch->eraseFromParent();
}

bool GenIR::canExecuteHandler(BasicBlock &Handler) {
  if (JitContext->Options->ExecuteHandlers) {
    // Flags indicate that all handlers should be executed.

    return true;
  }

  // Handler execution is still off by default, since precise GC reporting
  // isn't correct on exception paths.

  return false;
}

void GenIR::readerPostPass(bool IsImportOnly) {

  SmallVector<Value *, 4> EscapingLocs;
  GcFuncInfo->getEscapingLocations(EscapingLocs);

  if (EscapingLocs.size() > 0) {
    Value *FrameEscape = Intrinsic::getDeclaration(JitContext->CurrentModule,
                                                   Intrinsic::localescape);

    // Zero Initialize all the GC-Objects recorded in GcFuncInfo
    assert(AllocaInsertionPoint != nullptr);
    LLVMBuilder->SetInsertPoint(AllocaInsertionPoint->getParent(),
                                std::next(AllocaInsertionPoint->getIterator()));

    for (Value *EscapingValue : EscapingLocs) {
      if (GcInfo::isGcAllocation(EscapingValue)) {
        zeroInit(EscapingValue);
      }
    }

    // Insert the LocalEscape Intrinsic at the end of the
    // Prolog block, after all local allocations,
    // before the basic-block Terminator
    Instruction *InsertionPoint = Function->begin()->getTerminator();
    LLVMBuilder->SetInsertPoint(InsertionPoint);
    LLVMBuilder->CreateCall(FrameEscape, EscapingLocs);
  }

  // Crossgen in ReadyToRun mode records structs that method compilation
  // depends on via getClassSize calls. Therefore, we shouldn't cache type info
  // across methods.
  if (JitContext->Flags & CORJIT_FLG_READYTORUN) {
    // Clearing the cache is only safe if we're at the "top-level" context.
    // We shouldn't have re-entrant jit requests in a prejit scenario.
    assert(JitContext->Next == nullptr);
    ClassTypeMap->clear();
    ArrayTypeMap->clear();
    ReverseClassTypeMap->clear();
    BoxedTypeMap->clear();
  }

  // Cleanup the memory we've been using.
  delete DBuilder;
  delete LLVMBuilder;

  // While Jitting a method, SafepointPoll must appear after the function
  // actually being Jitted. EE's DebugInfoManager depends on the fact that
  // the Jitted function starts at the allocated code block.
  if (JitContext->Options->DoInsertStatepoints) {
    createSafepointPoll();
  }
}

void GenIR::insertIRToKeepGenericContextAlive() {
  IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
  Value *ContextLocalAddress = nullptr;
  CorInfoOptions Options = JitContext->MethodInfo->options;
  Instruction *InsertPoint = AllocaInsertionPoint;
  ASSERT(InsertPoint != nullptr);
  GENERIC_CONTEXTPARAM_TYPE ParamType = GENERIC_CONTEXTPARAM_NONE;

  if (Options & CORINFO_GENERICS_CTXT_FROM_THIS) {
    // The this argument might be modified in the method body, so
    // make a copy of the incoming this in a scratch local.
    ASSERT(MethodSignature.hasThis());
    Value *This = thisObj();
    Instruction *ScratchLocalAlloca = createTemporary(This->getType());
    // Put the store just after the newly added alloca.
    LLVMBuilder->SetInsertPoint(ScratchLocalAlloca->getParent(),
                                std::next(ScratchLocalAlloca->getIterator()));
    InsertPoint = makeStoreNonNull(This, ScratchLocalAlloca, false);
    // The scratch local's address is the saved context address.
    ContextLocalAddress = ScratchLocalAlloca;
    ParamType = GENERIC_CONTEXTPARAM_THIS;
  } else {
    // We know the type arg is unmodified so we can use its initial
    // spilled value location for reporting.
    if (Options & CORINFO_GENERICS_CTXT_FROM_METHODDESC) {
      ParamType = GENERIC_CONTEXTPARAM_MD;
    } else if (Options & CORINFO_GENERICS_CTXT_FROM_METHODTABLE) {
      ParamType = GENERIC_CONTEXTPARAM_MT;
    } else {
      assert(false && "Unexpected Option");
    }

    ASSERT(MethodSignature.hasTypeArg());
    ContextLocalAddress = Arguments[MethodSignature.getTypeArgIndex()];
  }

  GcFuncInfo->recordGenericsContext(cast<AllocaInst>(ContextLocalAddress),
                                    ParamType);

  // This method now requires a frame pointer.
  // TargetMachine *TM = JitContext->TM;
  // TM->Options.NoFramePointerElim = true;

  // Don't move AllocaInsertionPoint up since what we added was not an alloca
  LLVMBuilder->restoreIP(SavedInsertPoint);
}

void GenIR::insertIRForSecurityObject() {
  IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
  Type *Ty = getManagedPointerType(Type::getInt8Ty(*JitContext->LLVMContext));
  Instruction *SecurityObjectAddress = createTemporary(Ty, "SecurityObject");

  // Zero out the security object.
  LLVMBuilder->SetInsertPoint(SecurityObjectAddress->getParent(),
                              std::next(SecurityObjectAddress->getIterator()));
  IRNode *NullValue = loadNull();
  const bool IsVolatile = true;
  makeStoreNonNull(NullValue, SecurityObjectAddress, IsVolatile);

  // Call the helper, passing the method handle and the security object.
  bool IsIndirect = false;
  CORINFO_METHOD_HANDLE MethodHandle = getCurrentMethodHandle();
  CORINFO_METHOD_HANDLE EmbeddedHandle =
      embedMethodHandle(MethodHandle, &IsIndirect);
  const bool IsRelocatable = true;
  const bool IsCallTarget = false;
  IRNode *MethodNode = handleToIRNode(getMethodDefFromMethod(MethodHandle),
                                      EmbeddedHandle, MethodHandle, IsIndirect,
                                      IsIndirect, IsRelocatable, IsCallTarget);
  CorInfoHelpFunc SecurityHelper =
      JitContext->JitInfo->getSecurityPrologHelper(MethodHandle);
  const bool MayThrow = true;
  callHelper(SecurityHelper, MayThrow, nullptr, MethodNode,
             (IRNode *)SecurityObjectAddress);

  LLVMBuilder->restoreIP(SavedInsertPoint);
  GcFuncInfo->recordSecurityObject(cast<AllocaInst>(SecurityObjectAddress));
}

void GenIR::callMonitorHelper(bool IsEnter) {
  CorInfoHelpFunc HelperId;
  const uint32_t MethodFlags = getCurrentMethodAttribs();
  if (MethodFlags & CORINFO_FLG_STATIC) {
    HelperId =
        IsEnter ? CORINFO_HELP_MON_ENTER_STATIC : CORINFO_HELP_MON_EXIT_STATIC;
  } else {
    HelperId = IsEnter ? CORINFO_HELP_MON_ENTER : CORINFO_HELP_MON_EXIT;
  }
  const bool MayThrow = false;
  callHelperImpl(HelperId, MayThrow, Type::getVoidTy(*JitContext->LLVMContext),
                 MethodSyncHandle, (IRNode *)SyncFlag);
}

void GenIR::insertIRForUnmanagedCallFrame() {
  // There's nothing more to do if we've already inserted the necessary IR.
  if (UnmanagedCallFrame != nullptr) {
    assert(ThreadPointer != nullptr);
    return;
  }

  const struct CORINFO_EE_INFO::InlinedCallFrameInfo &CallFrameInfo =
      JitContext->EEInfo.inlinedCallFrameInfo;
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  Type *Int8Ty = Type::getInt8Ty(LLVMContext);
  Type *Int32Ty = Type::getInt32Ty(LLVMContext);
  Type *Int8PtrTy = getUnmanagedPointerType(Int8Ty);
  IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();

  // Mark this function as requiring a frame pointer and as using GC.
  Function->addFnAttr("no-frame-pointer-elim", "true");

  assert(GcInfo::isGcFunction(Function));

  // The call frame data structure is modeled as an opaque blob of bytes.
  Type *CallFrameTy = ArrayType::get(Int8Ty, CallFrameInfo.size);
  Instruction *CallFrameAddress =
      createTemporary(CallFrameTy, "InlinedCallFrame");

  Type *ThreadPointerTy = getUnmanagedPointerType(ArrayType::get(Int8Ty, 0));
  Instruction *ThreadPointerAddress =
      createTemporary(ThreadPointerTy, "ThreadPointer");

  // Intialize the call frame
  LLVMBuilder->SetInsertPoint(ThreadPointerAddress->getParent(),
                              std::next(ThreadPointerAddress->getIterator()));

  // Argument 0: &InlinedCallFrame[CallFrameInfo.offsetOfFrameVptr]
  Value *FrameVPtrIndices[] = {
      ConstantInt::get(Int32Ty, 0),
      ConstantInt::get(Int32Ty, CallFrameInfo.offsetOfFrameVptr)};
  Value *FrameVPtrAddress =
      LLVMBuilder->CreateInBoundsGEP(CallFrameAddress, FrameVPtrIndices);

  // Argument 1: the secret parameter, if any
  Value *SecretParam = nullptr;
  if (MethodSignature.hasSecretParameter()) {
    SecretParam = (Value *)secretParam();
  } else {
    SecretParam = Constant::getNullValue(Int8PtrTy);
  }

  // Call CORINFO_HELP_INIT_PINVOKE_FRAME
  const bool MayThrow = false;
  Value *ThreadPointerValue =
      callHelperImpl(CORINFO_HELP_INIT_PINVOKE_FRAME, MayThrow, ThreadPointerTy,
                     (IRNode *)FrameVPtrAddress, (IRNode *)SecretParam)
          .getInstruction();
  LLVMBuilder->CreateStore(ThreadPointerValue, ThreadPointerAddress);

  // Store the stack and frame pointers
  Type *RegType = Type::getIntNTy(LLVMContext, TargetPointerSizeInBits);
  Type *ReadRegisterTypes[] = {RegType};
  llvm::Function *ReadRegister = Intrinsic::getDeclaration(
      JitContext->CurrentModule, Intrinsic::read_register, ReadRegisterTypes);

  Value *CallSiteSPIndices[] = {
      ConstantInt::get(Int32Ty, 0),
      ConstantInt::get(Int32Ty, CallFrameInfo.offsetOfCallSiteSP)};
  Value *CallSiteSPAddress =
      LLVMBuilder->CreateInBoundsGEP(CallFrameAddress, CallSiteSPIndices);
  CallSiteSPAddress = LLVMBuilder->CreatePointerCast(
      CallSiteSPAddress, getUnmanagedPointerType(RegType));
  MDString *SPName = MDString::get(LLVMContext, "rsp");
  Metadata *SPNameNodeMDs[]{SPName};
  MDNode *SPNameNode = MDNode::get(LLVMContext, SPNameNodeMDs);
  Value *SPNameValue = MetadataAsValue::get(LLVMContext, SPNameNode);
  Value *SPValue = LLVMBuilder->CreateCall(ReadRegister, SPNameValue);
  LLVMBuilder->CreateStore(SPValue, CallSiteSPAddress);

  Value *CalleeSavedFPIndices[] = {
      ConstantInt::get(Int32Ty, 0),
      ConstantInt::get(Int32Ty, CallFrameInfo.offsetOfCalleeSavedFP)};
  Value *CalleeSavedFPAddress =
      LLVMBuilder->CreateInBoundsGEP(CallFrameAddress, CalleeSavedFPIndices);
  CalleeSavedFPAddress = LLVMBuilder->CreatePointerCast(
      CalleeSavedFPAddress, getUnmanagedPointerType(RegType));
  MDString *FPName = MDString::get(LLVMContext, "rbp");
  Metadata *FPNameNodeMDs[]{FPName};
  MDNode *FPNameNode = MDNode::get(LLVMContext, FPNameNodeMDs);
  Value *FPNameValue = MetadataAsValue::get(LLVMContext, FPNameNode);
  Value *FPValue = LLVMBuilder->CreateCall(ReadRegister, FPNameValue);
  LLVMBuilder->CreateStore(FPValue, CalleeSavedFPAddress);

  LLVMBuilder->restoreIP(SavedInsertPoint);

  UnmanagedCallFrame = CallFrameAddress;
  ThreadPointer = ThreadPointerAddress;
}

#pragma endregion

#pragma region UTILITIES

//===----------------------------------------------------------------------===//
//
// MSIL Reader Utilities
//
//===----------------------------------------------------------------------===//

AllocaInst *GenIR::createAlloca(Type *T, Value *ArraySize, const Twine &Name) {
  AllocaInst *AllocaInst = LLVMBuilder->CreateAlloca(T, ArraySize, Name);

  if (GcInfo::isGcType(T)) {
    GcFuncInfo->recordGcAlloca(AllocaInst);
  }

  return AllocaInst;
}

void GenIR::createSym(uint32_t Num, bool IsAuto, CorInfoType CorType,
                      CORINFO_CLASS_HANDLE Class, bool IsPinned,
                      ReaderSpecialSymbolType SymType) {

  // Give the symbol a plausible name.
  //
  // The user names for args and locals are stored in the PDB,
  // not in the metadata, so we can't directly access it via the jit interface.
  const char *SymName = IsAuto ? "loc" : "arg";
  bool UseNumber = false;
  uint32_t Number = Num;

  switch (SymType) {
  case ReaderSpecialSymbolType::Reader_ThisPtr:
    ASSERT(MethodSignature.hasThis());
    SymName = "this";
    break;

  case ReaderSpecialSymbolType::Reader_InstParam:
    ASSERT(MethodSignature.hasTypeArg());
    SymName = "$TypeArg";
    break;

  case ReaderSpecialSymbolType::Reader_VarArgsToken:
    ASSERT(MethodSignature.isVarArg());
    SymName = "$VarargsToken";
    break;

  case ReaderSpecialSymbolType::Reader_SecretParam:
    ASSERT(MethodSignature.hasSecretParameter());
    SymName = "$SecretParam";
    break;

  default:
    UseNumber = true;
    if (!IsAuto) {
      Number = MethodSignature.getILArgForArgIndex(Num);
    }
    break;
  }

  Type *LLVMType = this->getType(CorType, Class);
  if (!IsAuto) {
    const ABIArgInfo &Info = ABIMethodSig.getArgumentInfo(Num);
    if (!doesArgumentHaveHome(Info)) {
      Arguments[Num] = functionArgAt(Function, Info.getIndex());
      return;
    }

    // Look for unsafe locals (eventually we'll need to keep track
    // of these and reorder the stack and such).
    if (LLVMType->isStructTy() && (Class != nullptr)) {
      uint32_t ClassFlags = getClassAttribs(Class);
      if (ClassFlags & CORINFO_FLG_UNSAFE_VALUECLASS) {
        NeedsStackSecurityCheck = true;
      }
    }
  }

  AllocaInst *AllocaInst =
      createAlloca(LLVMType, nullptr,
                   UseNumber ? Twine(SymName) + Twine(Number) : Twine(SymName));

  if (IsPinned) {
    // If the allocated value is an aggregate, all GC pointers within that
    // aggregate is considered pinned.
    //
    // Such a case exists in the test
    // JIT\Methodical\ELEMENT_TYPE_IU\_il_relu_vfld.exe
    //  .locals (valuetype Test.AA pinned V_0)
    // where Test.AA is a struct containing a GC pointer.
    GcFuncInfo->recordPinned(AllocaInst);
  }

  DIFile *Unit = DBuilder->createFile(LLILCDebugInfo.TheCU->getFilename(),
                                      LLILCDebugInfo.TheCU->getDirectory());

  DIType *DebugType = convertType(LLVMType);
  bool AlwaysPreserve = false;
  unsigned Flags = 0;

  std::string Name =
      (UseNumber ? Twine(SymName) + Twine(Number) : Twine(SymName)).str();

  if (IsAuto) {
    auto *DebugVar =
        DBuilder->createAutoVariable(LLILCDebugInfo.FunctionScope, Name, Unit,
                                     0, DebugType, AlwaysPreserve, Flags);
    auto DL = llvm::DebugLoc::get(0, 0, LLILCDebugInfo.FunctionScope);
    DBuilder->insertDeclare(AllocaInst, DebugVar, DBuilder->createExpression(),
                            DL, LLVMBuilder->GetInsertBlock());

    LocalVars[Num] = AllocaInst;
    LocalVarCorTypes[Num] = CorType;
  } else {
    unsigned ArgNo = Num + 1;

    auto *DebugVar = DBuilder->createParameterVariable(
        LLILCDebugInfo.FunctionScope, Name, ArgNo, Unit, 0, DebugType,
        AlwaysPreserve, Flags);
    auto DL = llvm::DebugLoc::get(0, 0, LLILCDebugInfo.FunctionScope);
    DBuilder->insertDeclare(AllocaInst, DebugVar, DBuilder->createExpression(),
                            DL, LLVMBuilder->GetInsertBlock());
    Arguments[Num] = AllocaInst;
  }
}

void GenIR::zeroInit(Value *Var) {
  // TODO: If not isZeroInitLocals(), we only have to zero initialize
  // GC pointers and GC pointer fields on structs. For now we are zero
  // initalizing all fields in structs that have gc fields.
  //
  // TODO: We should try to lay out GC pointers contiguously on the stack
  // frame and use memset to initialize them.
  //
  // TODO: We can avoid zero-initializing some gc pointers if we can
  // ensure that we are not reporting uninitialized GC pointers at gc-safe
  // points.

  Type *VarType = Var->getType()->getPointerElementType();
  StructType *StructTy = dyn_cast<StructType>(VarType);
  if (StructTy != nullptr) {
    const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();
    const StructLayout *TheStructLayout = DataLayout->getStructLayout(StructTy);
    zeroInitBlock(Var, TheStructLayout->getSizeInBytes());
  } else {
    Constant *ZeroConst = Constant::getNullValue(VarType);
    LLVMBuilder->CreateStore(ZeroConst, Var);
  }
}

void GenIR::zeroInitLocals() {
  if (isZeroInitLocals()) {
    for (const auto &LocalVar : LocalVars) {
      if (!GcInfo::isGcAllocation(LocalVar)) {
        // All GC values are zero-initizlied in the post-pass.
        // So, only initialize the remaining ones if necessary.
        zeroInit(LocalVar);
      }
    }
  }

#ifndef NDEBUG
  // Ensure all GC-Locals are recorded to be initialized
  // Regardless of isZeroInitLocals()
  for (const auto &LocalVar : LocalVars) {
    if (GcInfo::isGcAllocation(LocalVar)) {
      assert(GcFuncInfo->hasRecord(cast<AllocaInst>(LocalVar)) &&
             "Missing GcAlloc Record");
    }
  }
#endif // !NDEBUG
}

void GenIR::zeroInitBlock(Value *Address, uint64_t Size) {
  bool IsSigned = false;
  ConstantInt *BlockSize = ConstantInt::get(
      *JitContext->LLVMContext, APInt(TargetPointerSizeInBits, Size, IsSigned));
  zeroInitBlock(Address, BlockSize);
}

void GenIR::zeroInitBlock(Value *Address, Value *Size) {
  // TODO: For small structs we may want to generate an integer StoreInst
  // instead of calling a helper.
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  const bool MayThrow = false;
  Type *VoidTy = Type::getVoidTy(LLVMContext);
  Value *ZeroByte = ConstantInt::get(LLVMContext, APInt(8, 0, true));
  callHelperImpl(CORINFO_HELP_MEMSET, MayThrow, VoidTy, (IRNode *)Address,
                 (IRNode *)ZeroByte, (IRNode *)Size);
}

void GenIR::copyStructNoBarrier(Type *StructTy, Value *DestinationAddress,
                                Value *SourceAddress, bool IsVolatile,
                                ReaderAlignType Alignment) {
  // TODO: For small structs we may want to generate an integer StoreInst
  // instead of calling a helper.
  const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();
  const StructLayout *TheStructLayout =
      DataLayout->getStructLayout(cast<StructType>(StructTy));
  IRNode *StructSize =
      (IRNode *)ConstantInt::get(Type::getInt32Ty(*JitContext->LLVMContext),
                                 TheStructLayout->getSizeInBytes());
  cpBlk(StructSize, (IRNode *)SourceAddress, (IRNode *)DestinationAddress,
        Alignment, IsVolatile);
}

void GenIR::copyStruct(CORINFO_CLASS_HANDLE Class, IRNode *Dst, IRNode *Src,
                       ReaderAlignType Alignment, bool IsVolatile,
                       bool IsUnchecked) {
  // We should potentially cache this or leverage LLVM type info instead.
  GCLayout *RuntimeGCInfo = getClassGCLayout(Class);
  if (RuntimeGCInfo != nullptr) {
    const uint32_t PointerSize = getPointerByteSize();
    uint32_t OffsetAfterLastGCPointer = 0;
    StructType *StructTy =
        cast<StructType>(getType(CORINFO_TYPE_VALUECLASS, Class));
    const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();
    const StructLayout *TheStructLayout = DataLayout->getStructLayout(StructTy);
    uint64_t StructSizeInBytes = TheStructLayout->getSizeInBytes();
    for (uint64_t CurrentOffset = 0; CurrentOffset < StructSizeInBytes;
         CurrentOffset += PointerSize) {
      uint32_t I = CurrentOffset / PointerSize;
      if (RuntimeGCInfo->GCPointers[I] != CorInfoGCType::TYPE_GC_NONE) {
        // Check if there is a block without GC pointers that we need to copy.
        // This covers cases when the block is before the first GC pointer or
        // between GC pointers.
        if (CurrentOffset != OffsetAfterLastGCPointer) {
          IRNode *Size =
              loadConstantI4(CurrentOffset - OffsetAfterLastGCPointer);
          IRNode *Offset = loadConstantI4(OffsetAfterLastGCPointer);
          IRNode *NonGCSrcAddr = binaryOp(ReaderBaseNS::Add, Src, Offset);
          IRNode *NonGCDstAddr = binaryOp(ReaderBaseNS::Add, Dst, Offset);
          cpBlk(Size, NonGCSrcAddr, NonGCDstAddr, Alignment, IsVolatile);
        }

        // Copy GC pointer with a write barrier.
        IRNode *GCPointerOffset = loadConstantI4(CurrentOffset);
        IRNode *GCSrcAddr = binaryOp(ReaderBaseNS::Add, Src, GCPointerOffset);
        IRNode *GCDstAddr = binaryOp(ReaderBaseNS::Add, Dst, GCPointerOffset);

        // Find the type of the GC pointer.
        // We may have nested structs. Loop until GC pointer is found.
        uint32_t OffsetInElementType = CurrentOffset;
        Type *ElementType = StructTy;
        do {
          StructType *ElementStructTy = cast<StructType>(ElementType);
          const StructLayout *ElementStructLayout =
              DataLayout->getStructLayout(ElementStructTy);
          unsigned int ElementIndex =
              ElementStructLayout->getElementContainingOffset(
                  OffsetInElementType);
          uint64_t ElementOffset =
              ElementStructLayout->getElementOffset(ElementIndex);
          ElementType = ElementStructTy->getElementType(ElementIndex);
          OffsetInElementType = OffsetInElementType - ElementOffset;
        } while (!GcInfo::isGcPointer(ElementType));

        Type *ElementTypePointer = PointerType::get(
            ElementType, GCSrcAddr->getType()->getPointerAddressSpace());
        GCSrcAddr = (IRNode *)LLVMBuilder->CreatePointerCast(
            GCSrcAddr, ElementTypePointer);
        IRNode *GCSrc = (IRNode *)makeLoadNonNull(GCSrcAddr, IsVolatile);
        const bool MayThrow = true;
        CorInfoHelpFunc HelperID = IsUnchecked
                                       ? CORINFO_HELP_ASSIGN_REF
                                       : CORINFO_HELP_CHECKED_ASSIGN_REF;
        IRNode *CallDst = nullptr;
        IRNode *Arg3 = nullptr;
        IRNode *Arg4 = nullptr;
        callHelper(HelperID, MayThrow, CallDst, GCDstAddr, GCSrc, Arg3, Arg4,
                   Alignment, IsVolatile);
        OffsetAfterLastGCPointer = CurrentOffset + PointerSize;
      }
    }
    // Check if we need to copy the tail after the last GC pointer.
    if (OffsetAfterLastGCPointer < StructSizeInBytes) {
      IRNode *Size =
          loadConstantI4(StructSizeInBytes - OffsetAfterLastGCPointer);
      IRNode *Offset = loadConstantI4(OffsetAfterLastGCPointer);
      IRNode *NonGCSrcAddr = binaryOp(ReaderBaseNS::Add, Src, Offset);
      IRNode *NonGCDstAddr = binaryOp(ReaderBaseNS::Add, Dst, Offset);
      cpBlk(Size, NonGCSrcAddr, NonGCDstAddr, Alignment, IsVolatile);
    }
    free(RuntimeGCInfo);
  } else {
    // If the class doesn't have a gc layout then use a memcopy
    IRNode *Size = loadConstantI4(getClassSize(Class));
    cpBlk(Size, Src, Dst, Alignment, IsVolatile);
  }
}

bool GenIR::doesValueRepresentStruct(Value *TheValue) {
  return StructPointers.count(TheValue) == 1;
}

void GenIR::setValueRepresentsStruct(Value *TheValue) {
  StructPointers.insert(TheValue);
}

// Return true if this IR node is a reference to the
// original this pointer passed to the method. Can
// conservatively return false.
bool GenIR::objIsThis(IRNode *Obj) { return false; }

// Create a new temporary with the indicated type.
Instruction *GenIR::createTemporary(Type *Ty, const Twine &Name) {
  // Put the alloca for this temporary into the entry block so
  // the temporary uses can appear anywhere.
  IRBuilder<>::InsertPoint IP = LLVMBuilder->saveIP();

  BasicBlock::iterator InsertPoint;
  BasicBlock *Block = nullptr;
  if (AllocaInsertionPoint == nullptr) {
    // There are no local, param or temp allocas in the entry block, so set
    // the insertion point to the first point in the block.
    InsertPoint = EntryBlock->getFirstInsertionPt();
    Block = EntryBlock;
  } else {
    // There are local, param or temp allocas. TempInsertionPoint refers to
    // the last of them. Set the insertion point to the next instruction since
    // the builder will insert new instructions before the insertion point.
    InsertPoint = std::next(AllocaInsertionPoint->getIterator());
    Block = AllocaInsertionPoint->getParent();
  }

  LLVMBuilder->SetInsertPoint(Block, InsertPoint);

  AllocaInst *AllocaInst = createAlloca(Ty, nullptr, Name);
  // Update the end of the alloca range.
  AllocaInsertionPoint = AllocaInst;
  LLVMBuilder->restoreIP(IP);

  return AllocaInst;
}

// Get the value of the unmodified this object.
IRNode *GenIR::thisObj() {
  ASSERT(MethodSignature.hasThis());
  uint32_t I = MethodSignature.getThisIndex();
  I = ABIMethodSig.getArgumentInfo(I).getIndex();
  return (IRNode *)functionArgAt(Function, I);
  ;
}

// Get the value of the secret parameter to an IL stub.
IRNode *GenIR::secretParam() {
  assert(MethodSignature.hasSecretParameter());
  assert(MethodSignature.getSecretParameterIndex() == 0);
  Function::arg_iterator Args = Function->arg_begin();
  Value *SecretParameter = &*Args;
  return (IRNode *)SecretParameter;
}

// Get the value of the varargs token (aka argList).
IRNode *GenIR::argList() {
  ASSERT(MethodSignature.isVarArg());
  uint32_t I = MethodSignature.getVarArgIndex();
  I = ABIMethodSig.getArgumentInfo(I).getIndex();
  return (IRNode *)functionArgAt(Function, I);
}

// Get the value of the instantiation parameter (aka type parameter).
IRNode *GenIR::instParam() {
  ASSERT(MethodSignature.hasTypeArg());
  uint32_t I = MethodSignature.getTypeArgIndex();
  I = ABIMethodSig.getArgumentInfo(I).getIndex();
  return (IRNode *)functionArgAt(Function, I);
}

// Convert ReaderAlignType to byte alighnment
uint32_t GenIR::convertReaderAlignment(ReaderAlignType ReaderAlignment) {
  uint32_t Result = (ReaderAlignment == Reader_AlignNatural)
                        ? TargetPointerSizeInBits / 8
                        : ReaderAlignment;
  return Result;
}

// Create the special @gc.safepoint_poll function
//
// This helper is required by the LLVM GC-Statepoint insertion phase.
// Statepoint lowering inlines the body of @gc.safepoint_poll function
// at function entry and at loop-back-edges.
//
// The current strategy is to use an unconditional call to the GCPoll helper.
// TODO: Inline calls to GCPoll helper when CORJIT_FLG_GCPOLL_INLINE is set.
//
// The following code is inserted into the module:
//
// define void @gc.safepoint_poll()
// {
// entry:
//   call void inttoptr(i64 <JIT_GCPoll> to void()*)()
//   ret void
// }

void GenIR::createSafepointPoll() {
  Module *M = JitContext->CurrentModule;
  llvm::LLVMContext *LLVMContext = JitContext->LLVMContext;

  Type *VoidType = Type::getVoidTy(*LLVMContext);
  FunctionType *VoidFnType = FunctionType::get(VoidType, false);

  llvm::Function *SafepointPoll = dyn_cast<llvm::Function>(
      M->getOrInsertFunction("gc.safepoint_poll", VoidFnType));

  assert(SafepointPoll->empty());

  // Safepoint Poll function itself is never expected to be called
  // at runtime, so we can save generating unwind information for it.
  //
  // PlaceSafepoints phase inlines the contents of @gc.SafepointPoll()
  // at all gc-polling locations.
  SafepointPoll->addFnAttr(Attribute::NoUnwind);

  BasicBlock *EntryBlock =
      BasicBlock::Create(*LLVMContext, "entry", SafepointPoll);

  IRNode *Address = getHelperCallAddress(CORINFO_HELP_POLL_GC);
  Value *Target =
      LLVMBuilder->CreateIntToPtr(Address, getUnmanagedPointerType(VoidFnType));

  CallInst::Create(Target, "", EntryBlock);
  ReturnInst::Create(*LLVMContext, EntryBlock);
}

bool GenIR::doTailCallOpt() { return JitContext->Options->DoTailCallOpt; }

// Set the Debug Location for the current instruction
void GenIR::setDebugLocation(uint32_t CurrOffset, bool IsCall) {
  assert(LLILCDebugInfo.FunctionScope != nullptr);

  DebugLoc Loc =
      DebugLoc::get(CurrOffset, IsCall, LLILCDebugInfo.FunctionScope);

  LLVMBuilder->SetCurrentDebugLocation(Loc);
}

llvm::DISubroutineType *GenIR::createFunctionType(llvm::Function *F,
                                                  DIFile *Unit) {
  SmallVector<Metadata *, 8> EltTys;
  // for each param, etc. convert the type to DIType and add it to the array.
  FunctionType *FunctionTy = F->getFunctionType();

  DIType *ReturnTy = convertType(Function->getReturnType());
  EltTys.push_back(ReturnTy);
  auto ParamEnd = FunctionTy->param_end();

  for (auto ParamIterator = FunctionTy->param_begin();
       ParamIterator != ParamEnd; ParamIterator++) {
    Type *Ty = *ParamIterator;
    DIType *ParamTy = convertType(Ty);

    EltTys.push_back(ParamTy);
  }
  return DBuilder->createSubroutineType(DBuilder->getOrCreateTypeArray(EltTys));
}

llvm::DIType *GenIR::convertType(Type *Ty) {
  StringRef TyName;
  llvm::dwarf::TypeKind Encoding;

  if (Ty->isVoidTy()) {
    return nullptr;
  }
  if (Ty->isIntegerTy()) {
    Encoding = llvm::dwarf::DW_ATE_signed;
    unsigned BitWidth = Ty->getIntegerBitWidth();

    switch (BitWidth) {
    case 8:
      TyName = "char";
      Encoding = llvm::dwarf::DW_ATE_unsigned_char;
      break;
    case 16:
      TyName = "short";
      break;
    case 32:
      TyName = "int";
      break;
    case 64:
      TyName = "long int";
      break;
    case 128:
      TyName = "long long int";
      break;
    default:
      TyName = "int";
      break;
    }
    llvm::DIType *DbgTy =
        DBuilder->createBasicType(TyName, BitWidth, BitWidth, Encoding);
    return DbgTy;
  }
  if (Ty->isFloatingPointTy()) {
    Encoding = llvm::dwarf::DW_ATE_float;
    unsigned BitWidth = Ty->getPrimitiveSizeInBits();
    if (Ty->isHalfTy()) {
      TyName = "half";
    }
    if (Ty->isFloatTy()) {
      TyName = "float";
    }
    if (Ty->isDoubleTy()) {
      TyName = "double";
    }
    if (Ty->isX86_FP80Ty()) {
      TyName = "long double";
    }

    llvm::DIType *DbgTy =
        DBuilder->createBasicType(TyName, BitWidth, BitWidth, Encoding);
    return DbgTy;
  }

  if (Ty->isPointerTy()) {
    uint64_t Size = JitContext->TM->createDataLayout().getPointerTypeSize(Ty);
    uint64_t Align =
        JitContext->TM->createDataLayout().getPrefTypeAlignment(Ty);
    llvm::DIType *DbgTy = DBuilder->createPointerType(
        convertType(Ty->getPointerElementType()), Size, Align);

    return DbgTy;
  }

  // TODO: These are currently empty types to prevent LLVM from accessing a
  // nullptr. Since we do not care about the types for debugging with the CLR,
  // this is sufficient, however, eventually we may want to actually fill these
  // out

  if (Ty->isStructTy()) {
    DIFile *Unit = DBuilder->createFile(LLILCDebugInfo.TheCU->getFilename(),
                                        LLILCDebugInfo.TheCU->getDirectory());
    llvm::DIType *DbgTy =
        DBuilder->createStructType(LLILCDebugInfo.TheCU, "csharp_object", Unit,
                                   0, 0, 0, 0, nullptr, llvm::DINodeArray());

    return DbgTy;
  }

  if (Ty->isArrayTy()) {
    llvm::DIType *DbgTy = DBuilder->createArrayType(
        0, 0, convertType(Ty->getArrayElementType()), llvm::DINodeArray());

    return DbgTy;
  }

  if (Ty->isVectorTy()) {
    llvm::DIType *DbgTy = DBuilder->createVectorType(
        0, 0, convertType(Ty->getVectorElementType()), llvm::DINodeArray());

    return DbgTy;
  }

  // LabelTy and MetadataTy do not correspond to a DIType.
  // Need to implement X86_MMX and Function types here
  return nullptr;
}

bool GenIR::doSimdIntrinsicOpt() {
  return JitContext->Options->DoSIMDIntrinsic;
}

#pragma endregion

#pragma region DIAGNOSTICS

//===----------------------------------------------------------------------===//
//
// MSIL Reader Diagnostics
//
//===----------------------------------------------------------------------===//

// Notify client of alignment problem
void GenIR::verifyStaticAlignment(void *FieldAddress, CorInfoType CorType,
                                  uint32_t MinClassAlign) {
  bool AlignmentError;
  const char *TypeName;

  AlignmentError = false;

  switch (CorType) {
  case CORINFO_TYPE_DOUBLE:
    TypeName = "CORINFO_TYPE_DOUBLE";
    goto ALIGN_8;
  case CORINFO_TYPE_STRING:
    TypeName = "CORINFO_TYPE_STRING";
    goto ALIGN_8;
  case CORINFO_TYPE_PTR:
    TypeName = "CORINFO_TYPE_PTR";
    goto ALIGN_8;
  case CORINFO_TYPE_BYREF:
    TypeName = "CORINFO_TYPE_BYREF";
    goto ALIGN_8;

  case CORINFO_TYPE_REFANY:
    TypeName = "CORINFO_TYPE_REFANY";
    goto RESOLVE_ALIGNMENT_BY_SIZE;
  case CORINFO_TYPE_VALUECLASS:
    TypeName = "CORINFO_TYPE_VALUECLASS";
    goto RESOLVE_ALIGNMENT_BY_SIZE;

  RESOLVE_ALIGNMENT_BY_SIZE:

    switch (MinClassAlign) {
    case 1:
      goto ALIGN_1;
    case 2:
      goto ALIGN_2;
    case 4:
      goto ALIGN_4;
    case 8:
      goto ALIGN_8;
    default:
      ASSERTNR(UNREACHED);
      break;
    }

  case CORINFO_TYPE_CLASS:
    TypeName = "CORINFO_TYPE_CLASS";
    goto ALIGN_8;

  ALIGN_8:

    // Require 8-byte alignment
    AlignmentError = ((7 & (uintptr_t)FieldAddress) != 0);
    break;

  case CORINFO_TYPE_INT:
    TypeName = "CORINFO_TYPE_INT";
    goto ALIGN_4;
  case CORINFO_TYPE_UINT:
    TypeName = "CORINFO_TYPE_UINT";
    goto ALIGN_4;
  case CORINFO_TYPE_LONG:
    TypeName = "CORINFO_TYPE_LONG";
    goto ALIGN_8;
  case CORINFO_TYPE_NATIVEINT:
    TypeName = "CORINFO_TYPE_NATIVEINT";
    goto ALIGN_8;
  case CORINFO_TYPE_NATIVEUINT:
    TypeName = "CORINFO_TYPE_NATIVEUINT";
    goto ALIGN_8;
  case CORINFO_TYPE_ULONG:
    TypeName = "CORINFO_TYPE_ULONG";
    goto ALIGN_8;
  case CORINFO_TYPE_FLOAT:
    TypeName = "CORINFO_TYPE_FLOAT";
    goto ALIGN_4;

  ALIGN_4:

    // Require 4-byte alignment
    AlignmentError = ((3 & (uintptr_t)FieldAddress) != 0);
    break;

  case CORINFO_TYPE_SHORT:
    TypeName = "CORINFO_TYPE_SHORT";
    goto ALIGN_2;
  case CORINFO_TYPE_USHORT:
    TypeName = "CORINFO_TYPE_USHORT";
    goto ALIGN_2;
  case CORINFO_TYPE_CHAR: // unicode
    TypeName = "CORINFO_TYPE_CHAR";
    goto ALIGN_2;

  ALIGN_2:

    // Require 2-byte alignment
    AlignmentError = ((1 & (uintptr_t)FieldAddress) != 0);
    break;

  case CORINFO_TYPE_BOOL:
    TypeName = "CORINFO_TYPE_BOOL";
    goto ALIGN_1;
  case CORINFO_TYPE_BYTE:
    TypeName = "CORINFO_TYPE_BYTE";
    goto ALIGN_1;
  case CORINFO_TYPE_UBYTE:
    TypeName = "CORINFO_TYPE_UBYTE";
    goto ALIGN_1;

  ALIGN_1:
  default:
    // Require 1-byte alignment - no constraints.
    break;
  }

// TODO: the commented out parts depend on debug code
// which we haven't ported.
#if defined(_DEBUG)
  if (AlignmentError /*&& ifdb(DB_UNALIGNEDSTATICASSERT)*/) {
    /*dbgs() << format
    ("Warning - unaligned static field found at address, type:%s, "
    "value 0x%I64x\n", typeName, fieldAddress);
    if ((corInfoType == CORINFO_TYPE_VALUECLASS) ||
    (corInfoType == CORINFO_TYPE_REFANY)) {
    dbgs() << format("minClassAlign: %d\n", minClassAlign);
    }*/
    ASSERT(UNREACHED);
  }
#endif
}

// Fatal error, reader cannot continue.
void ReaderBase::fatal(int ErrNum) { LLILCJit::fatal(LLILCJIT_FATAL_ERROR); }

#pragma endregion

#pragma region TYPES

//===----------------------------------------------------------------------===//
//
// MSIL READER CLR and LLVM Type Support
//
//===----------------------------------------------------------------------===//

Type *
GenIR::getType(CorInfoType CorType, CORINFO_CLASS_HANDLE ClassHandle,
               bool GetAggregateFields,
               std::list<CORINFO_CLASS_HANDLE> *DeferredDetailAggregates) {
  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;

  switch (CorType) {
  case CorInfoType::CORINFO_TYPE_UNDEF:
    return nullptr;

  case CorInfoType::CORINFO_TYPE_VOID:
    return Type::getVoidTy(LLVMContext);

  case CorInfoType::CORINFO_TYPE_BOOL:
  case CorInfoType::CORINFO_TYPE_BYTE:
  case CorInfoType::CORINFO_TYPE_UBYTE:
    return Type::getInt8Ty(LLVMContext);

  case CorInfoType::CORINFO_TYPE_SHORT:
  case CorInfoType::CORINFO_TYPE_USHORT:
  case CorInfoType::CORINFO_TYPE_CHAR:
    return Type::getInt16Ty(LLVMContext);

  case CorInfoType::CORINFO_TYPE_INT:
  case CorInfoType::CORINFO_TYPE_UINT:
    return Type::getInt32Ty(LLVMContext);

  case CorInfoType::CORINFO_TYPE_LONG:
  case CorInfoType::CORINFO_TYPE_ULONG:
    return Type::getInt64Ty(LLVMContext);

  case CorInfoType::CORINFO_TYPE_NATIVEINT:
  case CorInfoType::CORINFO_TYPE_NATIVEUINT:
    return Type::getIntNTy(LLVMContext, TargetPointerSizeInBits);

  case CorInfoType::CORINFO_TYPE_FLOAT:
    return Type::getFloatTy(LLVMContext);

  case CorInfoType::CORINFO_TYPE_DOUBLE:
    return Type::getDoubleTy(LLVMContext);

  case CorInfoType::CORINFO_TYPE_CLASS:
  case CorInfoType::CORINFO_TYPE_VALUECLASS:
  case CorInfoType::CORINFO_TYPE_REFANY: {
    ASSERT(ClassHandle != nullptr);
    return getClassType(ClassHandle, GetAggregateFields,
                        DeferredDetailAggregates);
  }

  case CorInfoType::CORINFO_TYPE_PTR:
  case CorInfoType::CORINFO_TYPE_BYREF: {
    ASSERT(ClassHandle != 0);
    bool IsPtr = (CorType == CorInfoType::CORINFO_TYPE_PTR);
    Type *ClassType = nullptr;
    CORINFO_CLASS_HANDLE ChildClassHandle = nullptr;
    CorInfoType ChildCorType = getChildType(ClassHandle, &ChildClassHandle);
    // LLVM does not allow void*, so use char* instead.
    if (ChildCorType == CORINFO_TYPE_VOID) {
      ASSERT(IsPtr);
      ClassType = getType(CORINFO_TYPE_CHAR, nullptr);
    } else if (ChildCorType == CORINFO_TYPE_UNDEF) {
      // Presumably a value class...?
      ClassType = getType(CORINFO_TYPE_VALUECLASS, ClassHandle,
                          GetAggregateFields, DeferredDetailAggregates);
    } else {
      ClassType = getType(ChildCorType, ChildClassHandle, GetAggregateFields,
                          DeferredDetailAggregates);
    }

    // Byrefs are reported as potential GC pointers.
    if (IsPtr) {
      return getUnmanagedPointerType(ClassType);
    } else {
      return getManagedPointerType(ClassType);
    }
  }

  case CorInfoType::CORINFO_TYPE_STRING: // Not used, should remove

  // CORINFO_TYPE_VAR is for a generic type variable.
  // Generic type variables only appear when the JIT is doing
  // verification (not NOT compilation) of generic code
  // for the EE, in which case we're running
  // the JIT in "import only" mode.

  case CorInfoType::CORINFO_TYPE_VAR:
  default:
    throw NotYetImplementedException("unexpected CorInfoType in GetType");
  }
}

Type *
GenIR::getClassType(CORINFO_CLASS_HANDLE ClassHandle, bool GetAggregateFields,
                    std::list<CORINFO_CLASS_HANDLE> *DeferredDetailAggregates) {
  Type *Result = nullptr;
  if (DeferredDetailAggregates == nullptr) {
    // Keep track of any aggregates that we deferred examining in detail, so we
    // can come back to them when this aggregate is filled in.
    std::list<CORINFO_CLASS_HANDLE> TheDeferredDetailAggregates;
    DeferredDetailAggregates = &TheDeferredDetailAggregates;

    if (!GetAggregateFields) {
      DeferredDetailAggregates->push_back(ClassHandle);
    }
    Result = getClassTypeWorker(ClassHandle, GetAggregateFields,
                                DeferredDetailAggregates);

    // Now that this aggregate's fields are filled in, go back
    // and fill in the details for those aggregates we deferred
    // handling earlier.

    std::list<CORINFO_CLASS_HANDLE>::iterator It =
        TheDeferredDetailAggregates.begin();
    while (It != TheDeferredDetailAggregates.end()) {
      CORINFO_CLASS_HANDLE DeferredClassHandle = *It;
      getClassTypeWorker(DeferredClassHandle, GetAggregateFields,
                         DeferredDetailAggregates);
      ++It;
    }
  } else {
    if (!GetAggregateFields) {
      DeferredDetailAggregates->push_back(ClassHandle);
    }
    Result = getClassTypeWorker(ClassHandle, GetAggregateFields,
                                DeferredDetailAggregates);
  }
  if (doSimdIntrinsicOpt() &&
      JitContext->JitInfo->isInSIMDModule(ClassHandle)) {
    std::string ClassName =
        appendClassNameAsString(ClassHandle, true, false, false);
    Type *VectorResult = 0;
    if (ClassName.length() == 23 &&
        ClassName.compare(0, 22, "System.Numerics.Vector") == 0) {
      LLVMContext &LLVMContext = *JitContext->LLVMContext;
      Type *FloatTy = Type::getFloatTy(LLVMContext);
      switch (ClassName[22]) {
      case '2':
        VectorResult = VectorType::get(FloatTy, 2);
        break;
      case '3':
        VectorResult = VectorType::get(FloatTy, 3);
        break;
      case '4':
        VectorResult = VectorType::get(FloatTy, 4);
        break;
      default:
        assert(UNREACHED);
      }
    } else {
      int NameLength = ClassName.length();
      if (ClassName.compare(NameLength - 2, NameLength, "[]") != 0) {
        int Length = 0;
        bool IsGeneric = false;
        bool IsSigned = false;
        Type *ElementTy = getBaseTypeAndSizeOfSIMDType(ClassHandle, Length,
                                                       IsGeneric, IsSigned);
        if (ElementTy && Length) {
          VectorResult = llvm::VectorType::get(ElementTy, Length);
        }
      }
    }
    if (VectorResult) {
      if (VectorTypeToStructType.count(VectorResult) == 0) {
        VectorTypeToStructType[VectorResult] = Result;
      }
      return VectorResult;
    }
  }
  return Result;
}

std::string GenIR::appendClassNameAsString(CORINFO_CLASS_HANDLE Class,
                                           bool IncludeNamespace, bool FullInst,
                                           bool IncludeAssembly) {
  int NameSize = 0;
  NameSize = appendClassName(nullptr, &NameSize, Class, IncludeNamespace,
                             FullInst, IncludeAssembly);
  std::string ResultString;
  if (NameSize > 0) {
    // Add one for terminating null.
    int32_t BufferLength = NameSize + 1;
    int32_t BufferRemaining = BufferLength;
    char16_t *WideCharBuffer = new char16_t[BufferLength];
    char16_t *BufferPtrToChange = WideCharBuffer;
    appendClassName(&BufferPtrToChange, &BufferRemaining, Class,
                    IncludeNamespace, FullInst, IncludeAssembly);
    ASSERT(BufferRemaining == 1);

    // Note that this is a worst-case estimate.
    size_t UTF8Size = (NameSize * UNI_MAX_UTF8_BYTES_PER_CODE_POINT) + 1;
    UTF8 *ClassName = new UTF8[UTF8Size];
    UTF8 *UTF8Start = ClassName;
    const UTF16 *UTF16Start = (UTF16 *)WideCharBuffer;
    ConversionResult Result =
        ConvertUTF16toUTF8(&UTF16Start, &UTF16Start[NameSize + 1], &UTF8Start,
                           &UTF8Start[UTF8Size], strictConversion);
    if (Result == conversionOK) {
      ASSERT((size_t)(&WideCharBuffer[BufferLength] -
                      (const char16_t *)UTF16Start) == 0);
      ResultString = (char *)ClassName;
    }
    delete[] ClassName;
    delete[] WideCharBuffer;
  }
  return ResultString;
}

// Map this class handle into an LLVM type.
//
// Classes are modelled via LLVM structs. Fields in a class
// correspond to .Net fields. We make the LLVM layout
// match the EE's layout here by accounting for the vtable
// and any internal padding.
//
// Note there may be some inter-element padding that
// is not accounted for here (eg array of value classes).
// We also do not model things like the preheader so overall
// size is accurate only for value classes.
//
// If GetAggregateFields is false, then we won't fill in the
// field information for aggregates. This is used to avoid
// getting trapped in cycles in the type reference graph.
Type *GenIR::getClassTypeWorker(
    CORINFO_CLASS_HANDLE ClassHandle, bool GetAggregateFields,
    std::list<CORINFO_CLASS_HANDLE> *DeferredDetailClasses) {
  // Check if we've already created a type for this class handle.
  Type *ResultTy = nullptr;
  StructType *StructTy = nullptr;
  uint32_t ArrayRank = getArrayRank(ClassHandle);
  bool IsArray = ArrayRank > 0;
  bool IsVector = isSDArray(ClassHandle);
  CORINFO_CLASS_HANDLE ArrayElementHandle = nullptr;
  CorInfoType ArrayElementType = CorInfoType::CORINFO_TYPE_UNDEF;

  // Two different handles can identify the same array: the actual array handle
  // and the handle of its MethodTable. Because of that we have a separate map
  // for arrays with <element type, element handle, array rank> tuple as key.
  if (IsArray) {
    ArrayElementType = getChildType(ClassHandle, &ArrayElementHandle);
    auto MapElement = ArrayTypeMap->find(std::make_tuple(
        ArrayElementType, ArrayElementHandle, ArrayRank, IsVector));
    if (MapElement != ArrayTypeMap->end()) {
      ResultTy = MapElement->second;
    }
  } else {
    auto MapElement = ClassTypeMap->find(ClassHandle);
    if (MapElement != ClassTypeMap->end()) {
      ResultTy = MapElement->second;
    }
  }

  bool IsRefClass = !JitContext->JitInfo->isValueClass(ClassHandle);

  if (ResultTy != nullptr) {
    // See if we can just return this result.
    if (IsRefClass) {
      // ResultTy should be ptr-to struct.
      ASSERT(ResultTy->isPointerTy());
      Type *ReferentTy = cast<PointerType>(ResultTy)->getPointerElementType();
      ASSERT(ReferentTy->isStructTy());
      StructTy = cast<StructType>(ReferentTy);
    } else {
      // Value classes should be structs
      ASSERT(ResultTy->isStructTy());
      StructTy = cast<StructType>(ResultTy);
    }

    // If we need fields and don't have them yet, we
    // can't return the cached type without doing some
    // work to finish it off.
    if (!GetAggregateFields || !StructTy->isOpaque()) {
      return ResultTy;
    }
  }

  // Cache the context and data layout.
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();

  // We need to fill in or create a new type for this class.
  if (StructTy == nullptr) {
    // Need to create one ... add it to the map now so it's
    // there if we make a recursive request.
    StructTy = StructType::create(LLVMContext);
    ResultTy =
        IsRefClass ? (Type *)getManagedPointerType(StructTy) : (Type *)StructTy;
    if (IsArray) {
      (*ArrayTypeMap)[std::make_tuple(ArrayElementType, ArrayElementHandle,
                                      ArrayRank, IsVector)] = ResultTy;
    } else {
      (*ClassTypeMap)[ClassHandle] = ResultTy;
      (*ReverseClassTypeMap)[ResultTy] = ClassHandle;
    }

    // Fetch the name of this type for use in dumps.

    // Note some constructed types like arrays may not have names.

    const bool IncludeNamespace = true;
    const bool FullInst = false;
    const bool IncludeAssembly = false;
    // We are using appendClassName instead of getClassName because
    // getClassName omits namespaces from some types (e.g., nested classes).
    // We may still get the same name for two different structs because
    // two classes with the same fully-qualified names may live in different
    // assemblies. In that case StructType->setName will append a unique suffix
    // to the conflicting name.
    std::string Name = appendClassNameAsString(ClassHandle, IncludeNamespace,
                                               FullInst, IncludeAssembly);
    if (Name.length()) {
      StructTy->setName(Name.c_str());
    }
  }

  // Bail out if we just want a placeholder for an aggregate.
  // We will fill in details later.
  if (!GetAggregateFields) {
    return ResultTy;
  }

  // We want to build up a description of the fields in
  // this type, including those from parent classes. We are
  // going to "inject" parent class fields into this type.
  // .Net only allows single inheritance so we know that
  // parent class's layout forms a prefix for this class's layout.
  //
  // Note getClassNumInstanceFields includes fields from
  // all ancestor classes. We'll need to subtract those out to figure
  // out how many fields this class uniquely contributes.
  const uint32_t NumFields = getClassNumInstanceFields(ClassHandle);
  std::vector<Type *> Fields;
  uint32_t ByteOffset = 0;
  uint32_t NumParentFields = 0;

  // Look for cases that require special handling.
  bool IsString = false;
  bool IsUnion = false;
  bool IsObject = false;
  bool IsTypedByref = false;
  CORINFO_CLASS_HANDLE ObjectClassHandle =
      getBuiltinClass(CorInfoClassId::CLASSID_SYSTEM_OBJECT);
  CORINFO_CLASS_HANDLE StringClassHandle =
      getBuiltinClass(CorInfoClassId::CLASSID_STRING);
  CORINFO_CLASS_HANDLE TypedByrefClassHandle =
      getBuiltinClass(CorInfoClassId::CLASSID_TYPED_BYREF);

  if (ClassHandle == ObjectClassHandle) {
    IsObject = true;
  } else if (ClassHandle == StringClassHandle) {
    IsString = true;
  } else if (ClassHandle == TypedByrefClassHandle) {
    IsTypedByref = true;
  } else {
    uint32_t ClassAttributes = getClassAttribs(ClassHandle);
    if ((ClassAttributes & CORINFO_FLG_ARRAY) != 0) {
      ASSERT(IsArray);
    }
    if ((ClassAttributes & CORINFO_FLG_OVERLAPPING_FIELDS) != 0) {
      IsUnion = true;
    }
  }

  uint32_t EEClassSize = 0;
  bool HaveClassSize = false;
  if (!IsRefClass) {
    try {
      EEClassSize = getClassSize(ClassHandle);
      HaveClassSize = true;
    } catch (...) {
      // In ReadyToRun mode a call to getClassSize triggers encoding of special
      // fixups in the image so that the runtime can verify the assumptions
      // about value type layouts before native code can be used. The runtime
      // will fall back to jit if value type layout changed. Currently
      // encoding of fixups is limited to just the existing assembly references.
      // If the jit asks about a valuetype from an assembly that's not
      // referenced from the current assembly, an exception is thrown.
      // Consider this example:
      //   Assembly A: public struct SA { SB b; }
      //   Assembly B : public struct  SB { SC c; }
      //   Assembly C : public struct  SC {}
      // If we are compiling a method in A and creating a type for
      // SA, we'll eventually get to SC. Since C is not referenced from A, the
      // call to getClassSize will throw.
      // Catching and swallowing the exception should be safe as long as
      // we won't use SC directly, only as part of SB or SA. Any change in SC
      // layout will change the SA and SB layout and the runtime will detect
      // those changes.
      // The LLVM type for SC may miss padding at the end. That shouldn't be a
      // problem since we'll insert the missing padding in the enclosing struct
      // (SB in this case).
      // The long-term plan of record is to get framework build-time tooling in
      // place that marks valuetype layout changes as breaking changes. With
      // that in place getClassSize won't throw.
      // TODO: we may want to add validation that the exception is caught only
      // for types that are embedded into other types.
      assert(JitContext->Flags & CORJIT_FLG_READYTORUN);
      HaveClassSize = false;
    }
  }

  // System.Object is a special case, it has no explicit
  // fields but we need to account for the vtable slot.
  if (IsObject) {
    ASSERT(NumFields == 0);
    ASSERT(IsRefClass);

    // Vtable is an array of pointer-sized things.
    Type *VtableSlotTy =
        Type::getIntNPtrTy(LLVMContext, TargetPointerSizeInBits);
    Type *VtableTy = ArrayType::get(VtableSlotTy, 0);
    Type *VtablePtrTy = VtableTy->getPointerTo();
    Fields.push_back(VtablePtrTy);
    ByteOffset += DataLayout->getTypeSizeInBits(VtablePtrTy) / 8;
  } else {
    // If we have a ref class, make sure the parent class
    // field information is filled in first.
    if (IsRefClass) {
      CORINFO_CLASS_HANDLE ParentClassHandle =
          JitContext->JitInfo->getParentType(ClassHandle);

      if (ParentClassHandle != nullptr) {
        // It's always ok to ask for the details of a parent type.
        const bool GetParentAggregateFields = true;
        Type *PointerToParentTy = getClassTypeWorker(
            ParentClassHandle, GetParentAggregateFields, DeferredDetailClasses);
        assert(StructTy->isOpaque());

        StructType *ParentTy =
            cast<StructType>(PointerToParentTy->getPointerElementType());

        // Add all the parent fields into the current struct.
        for (auto FieldIterator = ParentTy->subtype_begin();
             FieldIterator != ParentTy->subtype_end(); FieldIterator++) {
          Fields.push_back(*FieldIterator);
        }

        // Set number of parent fields and cumulative offset into this object.
        NumParentFields = getClassNumInstanceFields(ParentClassHandle);
        ByteOffset = DataLayout->getTypeSizeInBits(ParentTy) / 8;
      } else {
        NumParentFields = 0;
        ByteOffset = 0;
      }
    }

    // Determine how many fields are added at this level of derivation.
    ASSERT(NumFields >= NumParentFields);
    const uint32_t NumDerivedFields = NumFields - NumParentFields;

    // Add the fields (if any) contributed by this class.
    // We need to add them in increasing order of offset, but the EE
    // gives them to us in somewhat arbitrary order. So we have to sort.
    std::vector<std::pair<uint32_t, CORINFO_FIELD_HANDLE>> DerivedFields;

    for (uint32_t I = 0; I < NumDerivedFields; I++) {
      CORINFO_FIELD_HANDLE FieldHandle = getFieldInClass(ClassHandle, I);
      if (FieldHandle == nullptr) {
        // Likely a class that derives from System.__ComObject. See
        // LLILC issue #557. We'll just have to cope with an incomplete
        // picture of this type.
        assert(IsRefClass && "need to see all fields of value classes");
        break;
      }
      const uint32_t FieldOffset = getFieldOffset(FieldHandle);
      DerivedFields.push_back(std::make_pair(FieldOffset, FieldHandle));
    }

    // Putting offset first in the pair lets us use the
    // default comparator here.
    std::sort(DerivedFields.begin(), DerivedFields.end());

    // If we find overlapping fields, we'll stash them here so we can look
    // at them collectively.
    std::vector<std::pair<uint32_t, Type *>> OverlappingFields;

    // Now walk the fields in increasing offset order, adding
    // them and padding to the struct as we go.
    for (const auto &FieldPair : DerivedFields) {
      const uint32_t FieldOffset = FieldPair.first;
      CORINFO_FIELD_HANDLE FieldHandle = FieldPair.second;

      // Prepare to add this field to the collection.
      //
      // If this field is a ref class reference, we don't need the full
      // details on the referred-to class, and asking for the details here
      // causes trouble with certain recursive type graphs, for example:
      //
      // class A { B b; }
      // class B : extends A { int c; }
      //
      // We need to know the size of A before we can finish B. So we can't
      // ask for B's details while filling out A.
      CORINFO_CLASS_HANDLE FieldClassHandle;
      CorInfoType CorInfoType = getFieldType(FieldHandle, &FieldClassHandle);

      const bool GetAggregateFields = ((CorInfoType != CORINFO_TYPE_CLASS) &&
                                       (CorInfoType != CORINFO_TYPE_PTR) &&
                                       (CorInfoType != CORINFO_TYPE_BYREF));
      Type *FieldTy = getType(CorInfoType, FieldClassHandle, GetAggregateFields,
                              DeferredDetailClasses);
      // Double check that if the field is of struct type, we got its field
      // details.
      assert(!FieldTy->isStructTy() || !cast<StructType>(FieldTy)->isOpaque());

      // If we see an overlapping field, we need to handle it specially.
      if (FieldOffset < ByteOffset) {
        assert(IsUnion && "unexpected overlapping fields");
        // TypedByref and String get special treatement which we will skip
        // in processing overlaps.
        assert(!IsTypedByref && "No overlap expected in this type");
        assert(!IsString && "No overlap expected in this type");
        if (OverlappingFields.empty()) {
          // The previously processed field is also part of the overlap
          // set. Back it out of the main field collection and add it to the
          // overlap collection instead.
          Type *PreviousFieldTy = Fields.back();
          Fields.pop_back();
          uint32_t PreviousSize =
              DataLayout->getTypeSizeInBits(PreviousFieldTy) / 8;
          uint32_t PreviousOffset = ByteOffset - PreviousSize;
          addFieldsRecursively(OverlappingFields, PreviousOffset,
                               PreviousFieldTy);
        }

        // Add the current field to the overlap set.
        uint32_t FieldSize = DataLayout->getTypeSizeInBits(FieldTy) / 8;
        addFieldsRecursively(OverlappingFields, FieldOffset, FieldTy);

        // Determine new extent of the overlap region.
        ByteOffset = std::max(ByteOffset, FieldOffset + FieldSize);

        // Defer further processing until we find the end of the overlap
        // region.
        continue;
      }

      // This new field begins after any existing field. If we have an overlap
      // set in the works, we need to process it now.
      if (!OverlappingFields.empty()) {
        createOverlapFields(OverlappingFields, Fields);
        assert(OverlappingFields.empty());
      }

      // Account for padding by injecting a field.
      if (FieldOffset > ByteOffset) {
        const uint32_t PadSize = FieldOffset - ByteOffset;
        Type *PadTy = ArrayType::get(Type::getInt8Ty(LLVMContext), PadSize);
        Fields.push_back(PadTy);
        ByteOffset += DataLayout->getTypeSizeInBits(PadTy) / 8;
      }

      // We should be at the field offset now.
      ASSERT(FieldOffset == ByteOffset);

      // Validate or record this field's index in the map.
      auto FieldMapEntry = FieldIndexMap->find(FieldHandle);

      if (FieldMapEntry != FieldIndexMap->end()) {
        // We evidently can see the same field in different types
        // with shared generics. Just make sure they all agree
        // on the index.
        ASSERT(FieldMapEntry->second == Fields.size());
      } else {
        (*FieldIndexMap)[FieldHandle] = Fields.size();
      }

      // The first field of a typed byref is really GC (interior)
      // pointer. It's described in metadata as a pointer-sized integer.
      // Tweak it back...
      if (IsTypedByref && (FieldHandle == DerivedFields[0].second)) {
        FieldTy = getManagedPointerType(FieldTy);
      }

      // The last field of a string is really the start of an array
      // of characters. In LLVM we use a zero-sized array to
      // describe this.
      if (IsString &&
          (FieldHandle == DerivedFields[NumDerivedFields - 1].second)) {
        FieldTy = ArrayType::get(FieldTy, 0);
      }

      Fields.push_back(FieldTy);

      // Account for size of this field.
      ByteOffset += DataLayout->getTypeSizeInBits(FieldTy) / 8;
    }

    // If we have one final overlap set in the works, we need to process it now.
    if (!OverlappingFields.empty()) {
      createOverlapFields(OverlappingFields, Fields);
      assert(OverlappingFields.empty());
    }

    // If this is a value class, account for any additional end
    // padding that the runtime sees fit to add.
    //
    // We'd like to get the overall size right for
    // other types, but we have no way to check. So we'll have to
    // settle for having their fields at the right offsets.

    if (HaveClassSize) {
      ASSERT(EEClassSize >= ByteOffset);
      const uint32_t EndPadSize = EEClassSize - ByteOffset;

      if (EndPadSize > 0) {
        // We ought to be able to assert that the pad size
        // is not too large, but there are cases like
        // System.Reflection.MetadataEnumResult.<smallResult>e__FixedBuffer
        // where the runtime adds a lot more padding than one might
        // expect.
        Type *PadTy = ArrayType::get(Type::getInt8Ty(LLVMContext), EndPadSize);
        Fields.push_back(PadTy);
        ByteOffset += DataLayout->getTypeSizeInBits(PadTy) / 8;
      }
    }

    // If this is an array, there is an implicit length field
    // and an array of elements. Multidimensional arrays also
    // have lengths and lower bounds for each dimension.
    if (IsArray) {
      // Fill in the remaining fields.
      CORINFO_CLASS_HANDLE ArrayElementHandle = nullptr;
      CorInfoType ArrayElementCorTy =
          getChildType(ClassHandle, &ArrayElementHandle);
      const bool GetElementAggregateFields =
          ((ArrayElementCorTy != CORINFO_TYPE_CLASS) &&
           (ArrayElementCorTy != CORINFO_TYPE_PTR) &&
           (ArrayElementCorTy != CORINFO_TYPE_BYREF));
      Type *ElementTy =
          getType(ArrayElementCorTy, ArrayElementHandle,
                  GetElementAggregateFields, DeferredDetailClasses);

      ByteOffset += addArrayFields(Fields, IsVector, ArrayRank, ElementTy);

      // Verify that the offset we calculated matches the expected offset
      // for single-dimensional arrays of objects (note the last field is
      // size zero so ByteOffset is currently the offset of the array data).
      if (IsVector && (ArrayElementCorTy == CORINFO_TYPE_CLASS)) {
        ASSERTNR(ByteOffset == JitContext->EEInfo.offsetOfObjArrayData);
      }
    }
  }

  assert(StructTy->isOpaque());

  // Install the field list (even if empty) to complete the struct.
  // Since padding is explicit, this is an LLVM packed struct.
  StructTy->setBody(Fields, true /* isPacked */);

// For value classes we can do further checking and validate
// against the runtime's view of the class.
//
// Note the runtime only gives us size and gc info for value classes so
// we can't do this more generally.

#ifndef NDEBUG
  if (HaveClassSize) {
    const uint32_t PointerSize = DataLayout->getPointerSize();
    llvm::SmallVector<uint32_t, 4> GcPtrOffsets;

    GCLayout *RuntimeGCInfo = getClassGCLayout(ClassHandle);
    GcInfo::getGcPointers(StructTy, *DataLayout, GcPtrOffsets);

    if (RuntimeGCInfo != nullptr) {
      assert(RuntimeGCInfo->NumGCPointers == GcPtrOffsets.size() &&
             "Runtime and LLVM Types differ in #GC-pointers");

      for (uint32_t GcOffset : GcPtrOffsets) {
        assert((RuntimeGCInfo->GCPointers[GcOffset / PointerSize] !=
                CorInfoGCType::TYPE_GC_NONE) &&
               "Runtime and LLVM Types differ in GC-pointer Locations");
      }

      free(RuntimeGCInfo);

    } else {
      assert(GcPtrOffsets.size() == 0 &&
             "Runtime and LLVM Types differ in GC-ness");
    }
  }
#endif

  // Return the struct or a pointer to it as requested.
  return ResultTy;
}

void GenIR::addFieldsRecursively(
    std::vector<std::pair<uint32_t, llvm::Type *>> &Fields, uint32_t Offset,
    llvm::Type *Ty) {
  StructType *StructTy = dyn_cast<StructType>(Ty);
  if (StructTy != nullptr) {
    const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();
    for (Type *SubTy : StructTy->subtypes()) {
      addFieldsRecursively(Fields, Offset, SubTy);
      Offset += DataLayout->getTypeSizeInBits(SubTy) / 8;
    }
  } else {
    Fields.push_back(std::make_pair(Offset, Ty));
  }
}

void GenIR::createOverlapFields(
    std::vector<std::pair<uint32_t, llvm::Type *>> &OverlapFields,
    std::vector<llvm::Type *> &Fields) {

  // Prepare to create and measure types.
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();

  // Order the OverlapFields by offset.
  std::sort(OverlapFields.begin(), OverlapFields.end());

  // Walk the fields, accumulating the unique starting offsets of the gc
  // references in increasing offset order.
  std::vector<uint32_t> GcOffsets;
  uint32_t OverlapEndOffset = 0;
  for (const auto &OverlapField : OverlapFields) {
    uint32_t Offset = OverlapField.first;
    uint32_t Size = DataLayout->getTypeSizeInBits(OverlapField.second) / 8;
    OverlapEndOffset = std::max(OverlapEndOffset, Offset + Size);
    if (GcInfo::isGcPointer(OverlapField.second)) {
      assert(((Offset % getPointerByteSize()) == 0) &&
             "expect aligned gc pointers");
      if (GcOffsets.empty()) {
        GcOffsets.push_back(Offset);
      } else {
        uint32_t LastOffset = GcOffsets.back();
        assert((Offset >= LastOffset) && "expect offsets to be sorted");
        if (Offset > LastOffset) {
          GcOffsets.push_back(Offset);
        }
      }
    }
  }

  // Walk the GC reference offsets, creating representative fields.
  uint32_t FirstOffset = OverlapFields.begin()->first;
  uint32_t CurrentOffset = FirstOffset;
  for (const auto &GcOffset : GcOffsets) {
    assert((GcOffset >= CurrentOffset) && "expect offsets to be sorted");
    uint32_t NonGcPreambleSize = GcOffset - CurrentOffset;
    if (NonGcPreambleSize > 0) {
      Type *NonGcTy =
          ArrayType::get(Type::getInt8Ty(LLVMContext), NonGcPreambleSize);
      Fields.push_back(NonGcTy);
    }
    Fields.push_back(getBuiltInObjectType());
    CurrentOffset += getPointerByteSize();
  }

  // Create a trailing non-gc field if needed.
  assert((CurrentOffset <= OverlapEndOffset) && "overlap size overflow");
  if (CurrentOffset < OverlapEndOffset) {
    uint32_t RemainingSize = OverlapEndOffset - CurrentOffset;
    Type *NonGcTy = ArrayType::get(Type::getInt8Ty(LLVMContext), RemainingSize);
    Fields.push_back(NonGcTy);
  }

  // Clear out the overlap fields as promised.
  OverlapFields.clear();
}

char *GenIR::getClassNameWithNamespace(CORINFO_CLASS_HANDLE ClassHandle) {
  // Fetch the name of this type.
  // Note some constructed types like arrays may not have names.
  int32_t NameSize = 0;
  const bool IncludeNamespace = true;
  const bool FullInst = false;
  const bool IncludeAssembly = false;
  // We are using appendClassName instead of getClassName because
  // getClassName omits namespaces from some types (e.g., nested classes).
  // We may still get the same name for two different classes because
  // two classes with the same fully-qualified names may live in different
  // assemblies.
  NameSize = appendClassName(nullptr, &NameSize, ClassHandle, IncludeNamespace,
                             FullInst, IncludeAssembly);
  if (NameSize > 0) {
    // Add one for terminating null.
    int32_t BufferLength = NameSize + 1;
    int32_t BufferRemaining = BufferLength;
    char16_t *WideCharBuffer = new char16_t[BufferLength];
    char16_t *BufferPtrToChange = WideCharBuffer;
    appendClassName(&BufferPtrToChange, &BufferRemaining, ClassHandle,
                    IncludeNamespace, FullInst, IncludeAssembly);
    ASSERT(BufferRemaining == 1);

    // Note that this is a worst-case estimate.
    size_t UTF8Size = (NameSize * UNI_MAX_UTF8_BYTES_PER_CODE_POINT) + 1;
    UTF8 *ClassName = new UTF8[UTF8Size];
    UTF8 *UTF8Start = ClassName;
    const UTF16 *UTF16Start = (UTF16 *)WideCharBuffer;
    ConversionResult Result =
        ConvertUTF16toUTF8(&UTF16Start, &UTF16Start[NameSize + 1], &UTF8Start,
                           &UTF8Start[UTF8Size], strictConversion);
    delete[] WideCharBuffer;
    if (Result == conversionOK) {
      assert((size_t)(&WideCharBuffer[BufferLength] -
                      (const char16_t *)UTF16Start) == 0);
      return (char *)ClassName;
    } else {
      delete[] ClassName;
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

Type *GenIR::getBoxedType(CORINFO_CLASS_HANDLE Class) {
  assert(JitContext->JitInfo->isValueClass(Class));

  // Check to see if the boxed version of this type has already been generated.
  auto MapElement = BoxedTypeMap->find(Class);
  if (MapElement != BoxedTypeMap->end()) {
    return MapElement->second;
  }

  // Normalize the source type from Nullable<T> to T, if necessary.
  CORINFO_CLASS_HANDLE TypeToBox = getTypeForBox(Class);

  CorInfoType CorType = ReaderBase::getClassType(TypeToBox);
  Type *ValueType = getType(CorType, TypeToBox);

  if (CorType == CORINFO_TYPE_CLASS) {
    // NGen will try to compile __Canon instantiations for all generic types.
    // It does not check constraints to see whether __Canon instantiation is
    // useful. Because of that Class here may be Nullable<__Canon> even though
    // __Canon is a reference type and Nullable<T> has a constraint that T is a
    // value class. In that case getTypeForBox returns __Canon. Match what
    // RyuJit does and type the result as __Canon.
    assert((JitContext->Flags & CORJIT_FLG_PREJIT) != 0);
    return ValueType;
  }

  // Treat the boxed type as a subclass of Object with a single field of the
  // source type.
  Type *ObjectPtrType = getBuiltInObjectType();
  StructType *ObjectType =
      cast<StructType>(ObjectPtrType->getPointerElementType());
  ArrayRef<Type *> ObjectFields = ObjectType->elements();

  std::vector<Type *> Fields(ObjectFields.size() + 1);

  int I = 0;
  for (auto F : ObjectFields) {
    Fields[I++] = F;
  }
  Fields[I] = ValueType;

  LLVMContext &Context = *JitContext->LLVMContext;
  const bool IsPacked = true;
  Type *BoxedType;

  std::string BoxedTypeName;
  StructType *TheStructType = dyn_cast<StructType>(ValueType);
  if (TheStructType != nullptr) {
    StringRef StructTypeName = TheStructType->getStructName();
    if (!StructTypeName.empty()) {
      BoxedTypeName = Twine("Boxed_", StructTypeName).str();
    } else {
      BoxedTypeName = "Boxed_AnonStruct";
    }
  }

  VectorType *TheVectorType = dyn_cast<VectorType>(ValueType);
  if (TheVectorType != nullptr) {
    BoxedTypeName = "Boxed_Vector";
  }

  if (BoxedTypeName.empty()) {
    assert(ValueType->isFloatTy() || ValueType->isDoubleTy() ||
           ValueType->isIntegerTy());
    BoxedTypeName = "Boxed_Primitive";
  }

  BoxedType = StructType::create(Context, Fields, BoxedTypeName, IsPacked);
  BoxedType = getManagedPointerType(BoxedType);
  (*BoxedTypeMap)[TypeToBox] = BoxedType;
  if (Class != TypeToBox) {
    (*BoxedTypeMap)[Class] = BoxedType;
  }
  return BoxedType;
}

// Verify that this value's type is a valid type
// for an operand on the evaluation stack.
bool GenIR::isValidStackType(IRNode *Node) {
  Type *Ty = Node->getType();
  bool IsValid = false;

  switch (Ty->getTypeID()) {
  case Type::TypeID::IntegerTyID: {
    const uint32_t Size = Ty->getIntegerBitWidth();
    IsValid = (Size == 32) || (Size == 64);
    break;
  }

  case Type::TypeID::PointerTyID:
  case Type::TypeID::FloatTyID:
  case Type::TypeID::DoubleTyID:
    IsValid = true;
    break;
  case Type::TypeID::VectorTyID:
    IsValid = true;
    break;

  default:
    ASSERT(UNREACHED);
  }

  return IsValid;
}

// Given an integral or float CorInfoType, determine its size
// once pushed on the evaluation stack.
uint32_t GenIR::stackSize(CorInfoType CorType) {

  switch (CorType) {
  case CorInfoType::CORINFO_TYPE_BOOL:
  case CorInfoType::CORINFO_TYPE_CHAR:
  case CorInfoType::CORINFO_TYPE_BYTE:
  case CorInfoType::CORINFO_TYPE_UBYTE:
  case CorInfoType::CORINFO_TYPE_SHORT:
  case CorInfoType::CORINFO_TYPE_USHORT:
  case CorInfoType::CORINFO_TYPE_INT:
  case CorInfoType::CORINFO_TYPE_UINT:
  case CorInfoType::CORINFO_TYPE_FLOAT:
    return 32;

  case CorInfoType::CORINFO_TYPE_LONG:
  case CorInfoType::CORINFO_TYPE_ULONG:
  case CorInfoType::CORINFO_TYPE_DOUBLE:
    return 64;

  case CorInfoType::CORINFO_TYPE_NATIVEINT:
  case CorInfoType::CORINFO_TYPE_NATIVEUINT:
  case CorInfoType::CORINFO_TYPE_PTR:
  case CorInfoType::CORINFO_TYPE_BYREF:
  case CorInfoType::CORINFO_TYPE_CLASS:
    return TargetPointerSizeInBits;

  default:
    ASSERT(UNREACHED);
    return 0; // Silence the return value warning
  }
}

// Given an integral, pointer, or float CorInfoType, determine its size
uint32_t GenIR::size(CorInfoType CorType) {

  switch (CorType) {
  case CorInfoType::CORINFO_TYPE_BOOL:
  case CorInfoType::CORINFO_TYPE_CHAR:
  case CorInfoType::CORINFO_TYPE_BYTE:
    return 8;

  case CorInfoType::CORINFO_TYPE_UBYTE:
  case CorInfoType::CORINFO_TYPE_SHORT:
  case CorInfoType::CORINFO_TYPE_USHORT:
    return 16;

  case CorInfoType::CORINFO_TYPE_INT:
  case CorInfoType::CORINFO_TYPE_UINT:
  case CorInfoType::CORINFO_TYPE_FLOAT:
    return 32;

  case CorInfoType::CORINFO_TYPE_LONG:
  case CorInfoType::CORINFO_TYPE_ULONG:
  case CorInfoType::CORINFO_TYPE_DOUBLE:
    return 64;

  case CorInfoType::CORINFO_TYPE_NATIVEINT:
  case CorInfoType::CORINFO_TYPE_NATIVEUINT:
  case CorInfoType::CORINFO_TYPE_PTR:
  case CorInfoType::CORINFO_TYPE_BYREF:
  case CorInfoType::CORINFO_TYPE_CLASS:
    return TargetPointerSizeInBits;

  default:
    ASSERT(UNREACHED);
    return 0; // Silence the return value warning
  }
}

bool GenIR::isSignedIntegralType(CorInfoType CorType) {
  switch (CorType) {
  case CorInfoType::CORINFO_TYPE_UNDEF:
  case CorInfoType::CORINFO_TYPE_VOID:
  case CorInfoType::CORINFO_TYPE_FLOAT:
  case CorInfoType::CORINFO_TYPE_DOUBLE:
  case CorInfoType::CORINFO_TYPE_VALUECLASS:
  case CorInfoType::CORINFO_TYPE_REFANY:
    return false;

  default:
    return isSigned(CorType);
  }
}

// Given an CorInfoType, determine if it is
// signed or unsigned. Treats pointer
// types as unsigned.
bool GenIR::isSigned(CorInfoType CorType) {

  switch (CorType) {
  case CorInfoType::CORINFO_TYPE_BOOL:
  case CorInfoType::CORINFO_TYPE_CHAR:
  case CorInfoType::CORINFO_TYPE_UBYTE:
  case CorInfoType::CORINFO_TYPE_USHORT:
  case CorInfoType::CORINFO_TYPE_UINT:
  case CorInfoType::CORINFO_TYPE_ULONG:
  case CorInfoType::CORINFO_TYPE_NATIVEUINT:
  case CorInfoType::CORINFO_TYPE_PTR:
  case CorInfoType::CORINFO_TYPE_BYREF:
  case CorInfoType::CORINFO_TYPE_CLASS:
    return false;

  case CorInfoType::CORINFO_TYPE_BYTE:
  case CorInfoType::CORINFO_TYPE_SHORT:
  case CorInfoType::CORINFO_TYPE_INT:
  case CorInfoType::CORINFO_TYPE_LONG:
  case CorInfoType::CORINFO_TYPE_NATIVEINT:
    return true;

  default:
    ASSERT(UNREACHED);
    return false;
  }
}

// Given an integral CorInfoType, get the
// LLVM type that represents it on the stack
Type *GenIR::getStackType(CorInfoType CorType) {
  const uint32_t Size = stackSize(CorType);
  return Type::getIntNTy(*JitContext->LLVMContext, Size);
}

// Convert this result to a valid stack type,
// extending size as necessary for integer types.
//
// Because LLVM's type system can't describe unsigned
// types, we also pass in CorType to convey whether integral-typed
// Nodes should be handled as unsigned types.
IRNode *GenIR::convertToStackType(IRNode *Node, CorInfoType CorType) {
  Type *Ty = Node->getType();
  IRNode *Result = Node;

  switch (Ty->getTypeID()) {
  case Type::TypeID::IntegerTyID: {
    ASSERT(Ty->isIntegerTy());
    const uint32_t Size = Ty->getIntegerBitWidth();
    const uint32_t DesiredSize = stackSize(CorType);
    ASSERT(Size <= DesiredSize);

    if (Size < DesiredSize) {
      // Need to sign or zero extend, figure out which from the CorType.
      Type *ResultTy = getStackType(CorType);
      const bool IsSigned = isSigned(CorType);
      Result = (IRNode *)LLVMBuilder->CreateIntCast(Node, ResultTy, IsSigned);
    }
    break;
  }

  case Type::TypeID::PointerTyID:
  case Type::TypeID::FloatTyID:
  case Type::TypeID::DoubleTyID:
    // Already a valid stack type.
    break;
  case Type::TypeID::VectorTyID:
    // Already a valid stack type.
    break;

  case Type::TypeID::StructTyID:
  default:
    // An invalid type
    ASSERTNR(UNREACHED);
  }

  return Result;
}

// Convert a Node on the stack to the desired type, by:
// - truncating integer types
// - lengthening float to double (since we allow floats on the stack)
// - fixing pointer referent types
// - implicit conversions from int to/from ptr
// - sign extending int32 to native int
//
// Because LLVM's type system can't describe unsigned
// types, we also pass in CorType to convey whether integral
// ResultTys should be handled as unsigned types.
IRNode *GenIR::convertFromStackType(IRNode *Node, CorInfoType CorType,
                                    Type *ResultTy) {
  Type *Ty = Node->getType();
  IRNode *Result = Node;
  switch (Ty->getTypeID()) {
  case Type::TypeID::IntegerTyID: {
    const uint32_t Size = Ty->getIntegerBitWidth();
    const uint32_t DesiredSize = size(CorType);

    // A convert is needed if we're changing size
    // or implicitly converting int to ptr.
    const bool NeedsExtension = (Size < DesiredSize);
    const bool NeedsTruncation = (Size > DesiredSize);
    const bool NeedsReinterpret =
        ((CorType == CorInfoType::CORINFO_TYPE_PTR) ||
         (CorType == CorInfoType::CORINFO_TYPE_BYREF));

    if (NeedsTruncation) {
      assert(!NeedsReinterpret && "cannot reinterpret and truncate");
      const bool IsSigned = isSigned(CorType);
      Result = (IRNode *)LLVMBuilder->CreateIntCast(Node, ResultTy, IsSigned);
    } else if (NeedsExtension) {
      assert(!NeedsReinterpret && "cannot reinterpret and extend");
      assert((CorType == CorInfoType::CORINFO_TYPE_NATIVEINT ||
              CorType == CorInfoType::CORINFO_TYPE_NATIVEUINT) &&
             "only expect to extend to native int or uint");
      const bool IsSigned = CorType == CorInfoType::CORINFO_TYPE_NATIVEINT;
      Result = (IRNode *)LLVMBuilder->CreateIntCast(Node, ResultTy, IsSigned);
    } else if (NeedsReinterpret) {
      Result = (IRNode *)LLVMBuilder->CreateIntToPtr(Node, ResultTy);
    }

    break;
  }

  case Type::TypeID::FloatTyID:
  case Type::TypeID::DoubleTyID: {
    // Because we allow f32 on the stack we may
    // need a lengthening convert here.
    const uint32_t Size = Ty->getPrimitiveSizeInBits();
    const uint32_t DesiredSize = size(CorType);

    if (Size != DesiredSize) {
      Type *ResultTy = getType(CorType, nullptr);
      Result = (IRNode *)LLVMBuilder->CreateFPCast(Node, ResultTy);
    }

    break;
  }

  case Type::TypeID::PointerTyID: {
    bool PointerRepresentsStruct = doesValueRepresentStruct(Node);
    if (PointerRepresentsStruct) {
      if (Ty->getPointerElementType() != ResultTy) {
        assert(cast<StructType>(ResultTy)->isLayoutIdentical(
            cast<StructType>(Ty->getPointerElementType())));
        PointerType *NodePointerType = cast<PointerType>(Ty);
        ResultTy =
            PointerType::get(ResultTy, NodePointerType->getAddressSpace());
      } else {
        break;
      }
    }
    // May need to cast referent types.
    if (Ty != ResultTy) {
      Result = (IRNode *)LLVMBuilder->CreatePointerCast(Node, ResultTy);
      if (PointerRepresentsStruct) {
        setValueRepresentsStruct(Result);
      }
    }
    break;
  }
  case Type::TypeID::VectorTyID:
    Result = (IRNode *)Node;
    break;

  case Type::TypeID::StructTyID:
  // We don't allow structs on the stack. They are represented by pointers.
  // Fall through to default.

  default:
    // An invalid type
    ASSERTNR(UNREACHED);
  }

  return Result;
}

PointerType *GenIR::getManagedPointerType(Type *ElementType) {
  return PointerType::get(ElementType, GcInfo::ManagedAddressSpace);
}

PointerType *GenIR::getUnmanagedPointerType(Type *ElementType) {
  return PointerType::get(ElementType, GcInfo::UnmanagedAddressSpace);
}

uint32_t GenIR::addArrayFields(std::vector<llvm::Type *> &Fields, bool IsVector,
                               uint32_t ArrayRank, Type *ElementTy) {
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  const DataLayout *DataLayout = &JitContext->CurrentModule->getDataLayout();
  uint32_t FieldByteSize = 0;
  // Array length is (u)int32 ....
  Type *ArrayLengthTy = Type::getInt32Ty(LLVMContext);
  Fields.push_back(ArrayLengthTy);
  FieldByteSize += DataLayout->getTypeSizeInBits(ArrayLengthTy) / 8;

  // For 64 bit targets there's then a 32 bit pad.
  const uint32_t PointerSize = DataLayout->getPointerSizeInBits();
  if (PointerSize == 64) {
    Type *ArrayPadTy = ArrayType::get(Type::getInt8Ty(LLVMContext), 4);
    Fields.push_back(ArrayPadTy);
    FieldByteSize += DataLayout->getTypeSizeInBits(ArrayPadTy) / 8;
  }

  // For multi-dimensional arrays and single-dimensional arrays with a non-zero
  // lower bound there are arrays of dimension lengths and lower bounds.
  if (!IsVector) {
    Type *Int32Ty = Type::getInt32Ty(LLVMContext);
    Type *ArrayOfDimLengthsTy = ArrayType::get(Int32Ty, ArrayRank);
    Fields.push_back(ArrayOfDimLengthsTy);
    FieldByteSize += DataLayout->getTypeSizeInBits(ArrayOfDimLengthsTy) / 8;

    Type *ArrayOfDimLowerBoundsTy = ArrayType::get(Int32Ty, ArrayRank);
    Fields.push_back(ArrayOfDimLowerBoundsTy);
    FieldByteSize += DataLayout->getTypeSizeInBits(ArrayOfDimLowerBoundsTy) / 8;
  }

  Type *ArrayOfElementTy = ArrayType::get(ElementTy, 0);
  Fields.push_back(ArrayOfElementTy);
  return FieldByteSize;
}

bool GenIR::isArrayType(llvm::Type *ArrayTy, llvm::Type *ElementTy) {
  // Do some basic sanity checks that this type is one we created to model
  // a CLR array. Note we can't be 100% sure without keeping a whitelist
  // when we create these types.
  assert(GcInfo::isGcPointer(ArrayTy) && "expected managed pointer");
  Type *Type = cast<PointerType>(ArrayTy)->getPointerElementType();
  if (!Type->isStructTy()) {
    return false;
  }

  // An array type may have varying fields depending on pointer size.
  // Array payload is the last field and is a zero-element LLVM array.
  unsigned int IndexOfElements = getPointerByteSize() == 4 ? 2 : 3;
  StructType *ArrayStructType = cast<StructType>(Type);
  unsigned ElementCount = ArrayStructType->getNumElements();
  if (ElementCount != (IndexOfElements + 1)) {
    return false;
  }
  llvm::Type *ElementsArrayFieldType =
      ArrayStructType->getContainedType(IndexOfElements);
  if (!ElementsArrayFieldType->isArrayTy()) {
    return false;
  }
  ArrayType *ElementsArrayType = cast<ArrayType>(ElementsArrayFieldType);
  if (ElementsArrayType->getArrayNumElements() != 0) {
    return false;
  }
  llvm::Type *ActualElementTy = ElementsArrayType->getArrayElementType();
  if ((ElementTy != nullptr) && (ElementTy != ActualElementTy)) {
    return false;
  }
  return true;
}

IRNode *GenIR::ensureIsArray(IRNode *Array, llvm::Type *ElementTy) {
  Type *AddressTy = Array->getType();
  if (!this->isArrayType(AddressTy, ElementTy)) {
    // This must be System.__Canon or similar. Cast to array of desired
    // element type.
    if (ElementTy == nullptr) {
      ElementTy = this->getBuiltInObjectType();
    }
    PointerType *DesiredArrayType = getArrayOfElementType(ElementTy);
    Array = (IRNode *)LLVMBuilder->CreatePointerCast(Array, DesiredArrayType);
  }
  return Array;
}

PointerType *GenIR::getArrayOfElementType(llvm::Type *ElementTy) {
  auto It = ElementToArrayTypeMap.find(ElementTy);
  if (It != ElementToArrayTypeMap.end()) {
    return It->second;
  }
  PointerType *Array = createArrayOfElementType(ElementTy);
  ElementToArrayTypeMap[ElementTy] = Array;
  return Array;
}

PointerType *GenIR::createArrayOfElementType(llvm::Type *ElementTy) {
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  StructType *StructTy = StructType::create(LLVMContext);
  std::vector<Type *> Fields;

  // Vtable is an array of pointer-sized things.
  Type *VtableSlotTy = Type::getIntNPtrTy(LLVMContext, TargetPointerSizeInBits);
  Type *VtableTy = ArrayType::get(VtableSlotTy, 0);
  Type *VtablePtrTy = VtableTy->getPointerTo();
  Fields.push_back(VtablePtrTy);

  // Fill in the rest.

  const uint32_t ArrayRank = 1;
  const bool IsZeroLowerBoundSDArray = true;
  addArrayFields(Fields, IsZeroLowerBoundSDArray, ArrayRank, ElementTy);

  // Install fields and give this a recognizable name.
  StructTy->setBody(Fields, true /* isPacked */);
  std::string TypeName;

  // We use a raw_string_ostream as a handly way to construct
  // the name of the array type. This lets us use the
  // Type::print method to get the element type name.
  raw_string_ostream StringStream(TypeName);
  ElementTy->print(StringStream);
  StringStream << "[]";
  StringStream.str(); // will flush stream to TypeName.
  StructTy->setName(TypeName);

  // Set result as managed pointer to the struct
  PointerType *Result = getManagedPointerType(StructTy);
  return Result;
}

Type *GenIR::getBuiltInObjectType() {
  if (this->BuiltinObjectType == nullptr) {
    CORINFO_CLASS_HANDLE ObjectClassHandle =
        getBuiltinClass(CorInfoClassId::CLASSID_SYSTEM_OBJECT);
    this->BuiltinObjectType = getType(CORINFO_TYPE_CLASS, ObjectClassHandle);
  }
  return this->BuiltinObjectType;
}

Type *GenIR::getBuiltInStringType() {
  CORINFO_CLASS_HANDLE StringClassHandle =
      getBuiltinClass(CorInfoClassId::CLASSID_STRING);
  return getType(CORINFO_TYPE_CLASS, StringClassHandle);
}

#pragma endregion

#pragma region FLOW GRAPH

//===----------------------------------------------------------------------===//
//
// MSIL READER Flow Graph Support
//
//===----------------------------------------------------------------------===//

EHRegion *GenIR::fgNodeGetRegion(FlowGraphNode *Node) {
  return FlowGraphInfoMap[Node].Region;
}

void GenIR::fgNodeSetRegion(FlowGraphNode *Node, EHRegion *Region) {
  assert(fgNodeGetRegion(Node) == nullptr && "unexpected region change");
  FlowGraphInfoMap[Node].Region = Region;
}

void GenIR::fgNodeChangeRegion(FlowGraphNode *Node, EHRegion *Region) {
  FlowGraphInfoMap[Node].Region = Region;
}

FlowGraphNode *GenIR::fgGetHeadBlock() {
  return ((FlowGraphNode *)&Function->getEntryBlock());
}

FlowGraphNode *GenIR::fgGetTailBlock() {
  return ((FlowGraphNode *)&Function->back());
}

FlowGraphNode *GenIR::makeFlowGraphNode(uint32_t TargetOffset,
                                        FlowGraphNode *PreviousNode) {
  BasicBlock *NextBlock =
      (PreviousNode == nullptr ? nullptr : PreviousNode->getNextNode());
  FlowGraphNode *Node = (FlowGraphNode *)BasicBlock::Create(
      *JitContext->LLVMContext, "", Function, NextBlock);
  fgNodeSetStartMSILOffset(Node, TargetOffset);
  return Node;
}

bool irNodeIsLabel(IRNode *Node) { return Node->getType()->isLabelTy(); }

IRNode *GenIR::fgMakeBranch(IRNode *LabelNode, IRNode *InsertNode,
                            uint32_t CurrentOffset, bool IsConditional,
                            bool IsNominal) {
  LLVMBuilder->SetInsertPoint((BasicBlock *)InsertNode);
  BranchInst *BranchInst = nullptr;
  if (IsConditional) {
    // Fake condition. The real condition will be inserted when
    // processing basic blocks.
    unsigned NumBits = 1;
    bool IsSigned = false;
    ConstantInt *ZeroConst =
        ConstantInt::get(*JitContext->LLVMContext, APInt(NumBits, 0, IsSigned));
    BranchInst =
        LLVMBuilder->CreateCondBr(ZeroConst, (BasicBlock *)LabelNode, nullptr);
  } else {
    BranchInst = LLVMBuilder->CreateBr((BasicBlock *)LabelNode);
  }
  return (IRNode *)BranchInst;
}

uint32_t GenIR::fgNodeGetStartMSILOffset(FlowGraphNode *Fg) {
  return FlowGraphInfoMap[Fg].StartMSILOffset;
}

void GenIR::fgNodeSetStartMSILOffset(FlowGraphNode *Fg, uint32_t Offset) {
  FlowGraphInfoMap[Fg].StartMSILOffset = Offset;
}

uint32_t GenIR::fgNodeGetEndMSILOffset(FlowGraphNode *Fg) {
  return FlowGraphInfoMap[Fg].EndMSILOffset;
}

void GenIR::fgNodeSetEndMSILOffset(FlowGraphNode *Fg, uint32_t Offset) {
  FlowGraphInfoMap[Fg].EndMSILOffset = Offset;
}

FlowGraphNode *GenIR::fgSplitBlock(FlowGraphNode *Block, IRNode *Node) {
  Instruction *Inst = (Instruction *)Node;
  BasicBlock *TheBasicBlock = (BasicBlock *)Block;
  bool PropagatesStack = fgNodePropagatesOperandStack(Block);
  BasicBlock *NewBlock;
  if (Inst == nullptr) {
    NewBlock = BasicBlock::Create(*JitContext->LLVMContext, "", Function,
                                  TheBasicBlock->getNextNode());
    TerminatorInst *TermInst = TheBasicBlock->getTerminator();
    if (TermInst != nullptr) {
      if (isa<UnreachableInst>(TermInst)) {
        // do nothing
      } else {
        BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
        if (BranchInstruction != nullptr) {
          if (BranchInstruction->isConditional()) {
            BranchInstruction->setSuccessor(1, NewBlock);
          }
        } else {
          SwitchInst *SwitchInstruction = cast<SwitchInst>(TermInst);
          // Since cases are not added yet, the successor index is 0.
          SwitchInstruction->setSuccessor(0, NewBlock);
        }
      }
    } else {
      BranchInst::Create(NewBlock, TheBasicBlock);
    }
  } else {
    assert(TheBasicBlock != nullptr);
    if (TheBasicBlock->getTerminator() != nullptr) {
      NewBlock = TheBasicBlock->splitBasicBlock(Inst);
    } else {
      NewBlock = BasicBlock::Create(*JitContext->LLVMContext, "", Function,
                                    TheBasicBlock->getNextNode());
      NewBlock->getInstList().splice(NewBlock->end(),
                                     TheBasicBlock->getInstList(),
                                     Inst->getIterator(), TheBasicBlock->end());
      BranchInst::Create(NewBlock, TheBasicBlock);
    }
  }
  fgNodeSetPropagatesOperandStack((FlowGraphNode *)NewBlock, PropagatesStack);
  return (FlowGraphNode *)NewBlock;
}

void GenIR::fgRemoveUnusedBlocks(FlowGraphNode *FgHead) {
#ifndef NDEBUG
  bool IsOk = !verifyFunction(*Function, &dbgs());
  assert(IsOk && "verification failed");
#endif
  removeUnreachableBlocks(*this->Function);
}

void GenIR::fgDeleteBlock(FlowGraphNode *Node) {
  BasicBlock *Block = (BasicBlock *)Node;
  Block->eraseFromParent();
}

void GenIR::fgDeleteNodesFromBlock(FlowGraphNode *Node) {
  // Note: this will remove all instructions in the block, including
  // the terminator, which means we'll lose track of the successor
  // blocks.  That's ok since the caller always wants to drop the
  // successor edges as well, but is a difference compared to legacy jits.
  BasicBlock *Block = (BasicBlock *)Node;
  Block->getInstList().clear();
}

IRNode *GenIR::fgMakeSwitch(IRNode *DefaultLabel, IRNode *Insert) {
  LLVMBuilder->SetInsertPoint((BasicBlock *)Insert);

  // Create switch with null condition because it is invoked during
  // flow-graph build. The subsequent pass of Reader will set
  // this operanad properly.
  Value *NullValue =
      Constant::getNullValue(Type::getInt32Ty(*JitContext->LLVMContext));

  return (IRNode *)LLVMBuilder->CreateSwitch(NullValue,
                                             (BasicBlock *)DefaultLabel);
}

IRNode *GenIR::fgAddCaseToCaseList(IRNode *SwitchNode, IRNode *LabelNode,
                                   unsigned Element) {
  ConstantInt *Case = ConstantInt::get(*JitContext->LLVMContext,
                                       APInt(32, (uint64_t)Element, false));
  ((SwitchInst *)SwitchNode)->addCase(Case, (BasicBlock *)LabelNode);
  return SwitchNode;
}

// The legacy jit implementation inserts a sentinel for throw (followed by an
// unreached) and returns the sentinel.  Here we just generate the unreached
// and return it.
IRNode *GenIR::fgMakeThrow(IRNode *Insert) {
  BasicBlock *ThrowBlock = (BasicBlock *)Insert;

  LLVMBuilder->SetInsertPoint(ThrowBlock);

  // Create an unreachable that will follow the throw.

  UnreachableInst *Unreachable = LLVMBuilder->CreateUnreachable();
  return (IRNode *)Unreachable;
}

IRNode *GenIR::fgMakeReturn(IRNode *Insert) {
  BasicBlock *ReturnBlock = (BasicBlock *)Insert;

  LLVMBuilder->SetInsertPoint(ReturnBlock);

  UnreachableInst *Unreachable = LLVMBuilder->CreateUnreachable();
  return (IRNode *)Unreachable;
}

SwitchInst *GenIR::createFinallyDispatch(EHRegion *FinallyRegion) {
  LLVMContext &Context = *JitContext->LLVMContext;
  Type *SelectorType = IntegerType::getInt32Ty(Context);
  Value *SelectorAddr = createTemporary(SelectorType, "finally_cont");

  if (UnreachableContinuationBlock == nullptr) {
    // First finally for this function; generate an unreachable block
    // that can be used as the default switch target.
    UnreachableContinuationBlock =
        createPointBlock(MethodInfo->ILCodeSize, "NullDefault");
    new UnreachableInst(Context, UnreachableContinuationBlock);
    fgNodeSetPropagatesOperandStack(
        (FlowGraphNode *)UnreachableContinuationBlock, false);
  }

  LoadInst *Load = new LoadInst(SelectorAddr);
  SwitchInst *Switch =
      SwitchInst::Create(Load, UnreachableContinuationBlock, 4);
  FinallyRegion->EndFinallySwitch = Switch;
  return Switch;
}

IRNode *GenIR::fgMakeEndFinally(IRNode *InsertNode, EHRegion *FinallyRegion,
                                uint32_t CurrentOffset) {
  assert(FinallyRegion != nullptr);

  BasicBlock *Block = (BasicBlock *)InsertNode;
  SwitchInst *Switch = FinallyRegion->EndFinallySwitch;
  if (Switch == nullptr) {
    // We haven't yet seen a leave from the associated protected region. We may
    // well never see one. Assume the latter is rare and create the necessary
    // dispatch machinery.
    Switch = createFinallyDispatch(FinallyRegion);
  }

  BasicBlock *TargetBlock = Switch->getParent();
  if (TargetBlock == nullptr) {
    // This is the first endfinally for this finally.  Generate a block to
    // hold the switch. Use the finally end offset as the switch block's
    // begin/end.
    TargetBlock = createPointBlock(FinallyRegion->EndMsilOffset, "endfinally");
    LLVMBuilder->SetInsertPoint(TargetBlock);

    // Insert the load of the selector variable and the switch.
    LoadInst *Load = (LoadInst *)Switch->getCondition();
    LLVMBuilder->Insert(Load);
    LLVMBuilder->Insert(Switch);

    // Create the cleanupret to handle the exceptional continuation
    BasicBlock *UnwindDest = FinallyRegion->HandlerEHPad
                                 ? FinallyRegion->HandlerEHPad->getParent()
                                 : nullptr;
    BasicBlock *ExitBlock =
        createPointBlock(FinallyRegion->EndMsilOffset, "exn.continue");
    LLVMBuilder->SetInsertPoint(ExitBlock);
    LLVMBuilder->CreateCleanupRet(FinallyRegion->CleanupPad, UnwindDest);
    ConstantInt *Zero = ConstantInt::get(cast<IntegerType>(Load->getType()), 0);
    Switch->addCase(Zero, ExitBlock);

    // Add the corresponding store on the exceptional entry.
    TerminatorInst *PadTerminator =
        FinallyRegion->CleanupPad->getParent()->getTerminator();
    LLVMBuilder->SetInsertPoint(PadTerminator);
    LLVMBuilder->CreateStore(Zero, Load->getPointerOperand());
  }

  // Generate and return branch to the block that holds the switch
  LLVMBuilder->SetInsertPoint(Block);
  return (IRNode *)LLVMBuilder->CreateBr(TargetBlock);
}

IRNode *GenIR::fgMakeEndFault(IRNode *InsertNode, EHRegion *FaultRegion,
                              uint32_t CurrentOffset) {
  // Fault handlers can only be reached by exceptions, and we don't
  // yet support handling exceptions, so this can't be reached.
  // Generate an UnreachableInst to keep the IR well-formed.
  // When we do support handlers, this will become a branch to the
  // next outer handler.

  BasicBlock *Block = (BasicBlock *)InsertNode;
  LLVMBuilder->SetInsertPoint(Block);
  return (IRNode *)LLVMBuilder->CreateUnreachable();
}

void GenIR::beginFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrOffset,
                               bool IsVerifyOnly) {
  IRNode *InsertInst = fgNodeGetEndInsertIRNode(Fg);
  if (InsertInst != nullptr) {
    LLVMBuilder->SetInsertPoint((Instruction *)InsertInst);
  } else {
    LLVMBuilder->SetInsertPoint(Fg);
  }
}

void GenIR::fgEnterRegion(EHRegion *Region) {
  if (Region->Kind != ReaderBaseNS::RegionKind::RGN_Try) {
    // Nothing to do when entering other region kinds -- handler regions get
    // set up when we enter the try region they handle.
    return;
  }

  // Find the outer region handler first.
  EHRegion *Parent = rgnGetParent(Region);
  assert(!rgnIsOutsideParent(Region));

  // Also find the nearest non-try ancestor -- this corresponds to the funclet
  // that this try is nested within (if any), which we'll need to record on
  // the EH pad generated here.
  EHRegion *EnclosingHandler = Parent;
  while (EnclosingHandler->Kind == ReaderBaseNS::RegionKind::RGN_Try) {
    assert(!rgnIsOutsideParent(EnclosingHandler));
    EnclosingHandler = rgnGetParent(EnclosingHandler);
  }
  Value *EnclosingPad;
  switch (EnclosingHandler->Kind) {
  case ReaderBaseNS::RegionKind::RGN_Root:
    EnclosingPad = ConstantTokenNone::get(*JitContext->LLVMContext);
    break;
  case ReaderBaseNS::RegionKind::RGN_MCatch:
  case ReaderBaseNS::RegionKind::RGN_MExcept:
    EnclosingPad = EnclosingHandler->CatchPad;
    break;
  case ReaderBaseNS::RegionKind::RGN_Finally:
  case ReaderBaseNS::RegionKind::RGN_Fault:
    EnclosingPad = EnclosingHandler->CleanupPad;
    break;
  case ReaderBaseNS::RegionKind::RGN_Filter:
    EnclosingPad = EnclosingHandler->HandlerRegion->CatchPad;
    break;
  default:
    llvm_unreachable("Unexpected region kind");
  }

  // Visit each child of the try, creating handler code for any that are
  // handlers.
  CatchSwitchInst *CatchSwitch = nullptr;
  Instruction *NewPad = nullptr;
  for (EHRegionList *ChildNode = rgnGetChildList(Region); ChildNode;
       ChildNode = rgnListGetNext(ChildNode)) {
    EHRegion *Child = rgnListGetRgn(ChildNode);
    switch (Child->Kind) {
    case ReaderBaseNS::RegionKind::RGN_Try:
      // This is a nested region, not an attached handler.
      // We'll create its handlers when we enter it.
      break;
    case ReaderBaseNS::RegionKind::RGN_MExcept:
      // This doesn't need an EH pad because the filter gets a catchpad and
      // branches to the handler at the point of the endfilter.
      break;
    case ReaderBaseNS::RegionKind::RGN_MCatch:
    case ReaderBaseNS::RegionKind::RGN_Filter: {
      // These become catchpads, which need to be embedded in a catchswitch.
      if (CatchSwitch == nullptr) {
        assert(NewPad == nullptr &&
               "try with finally/fault can't also have catch/filter");
        BasicBlock *SwitchBlock =
            createPointBlock(Region->EndMsilOffset, "exn.dispatch");
        LLVMBuilder->SetInsertPoint(SwitchBlock);
        // Compute the number of handlers we'll attach to this catchswitch
        unsigned NumHandlers = 1;
        for (EHRegionList *Node = rgnListGetNext(ChildNode); Node != nullptr;
             Node = rgnListGetNext(Node)) {
          if (rgnListGetRgn(Node)->Kind ==
              ReaderBaseNS::RegionKind::RGN_MExcept) {
            // This will share its filter's handler
            continue;
          }
          // The rest of the list should just be more catch/filter handlers.
          assert(rgnListGetRgn(Node)->Kind ==
                     ReaderBaseNS::RegionKind::RGN_MCatch ||
                 rgnListGetRgn(Node)->Kind ==
                     ReaderBaseNS::RegionKind::RGN_Filter);
          ++NumHandlers;
        }
        // Create the catchswitch
        BasicBlock *UnwindDest =
            Parent->HandlerEHPad ? Parent->HandlerEHPad->getParent() : nullptr;
        NewPad = CatchSwitch = LLVMBuilder->CreateCatchSwitch(
            EnclosingPad, UnwindDest, NumHandlers, "catchswitch");
      }
      // Any exception bubbling out of this handler will unwind to the same
      // thing that the parent region unwinds to.
      Child->HandlerEHPad = Parent->HandlerEHPad;
      // Create the catchpad
      Instruction *CatchPad = createEHPad(Child, CatchSwitch);
      CatchSwitch->addHandler(CatchPad->getParent());
      break;
    }
    case ReaderBaseNS::RegionKind::RGN_Finally:
    case ReaderBaseNS::RegionKind::RGN_Fault:
      assert(NewPad == nullptr &&
             "try with finally/fault cannot have multiple handlers");
      // Any exception bubbling out of this handler will unwind to the same
      // thing that the parent region unwinds to.
      Child->HandlerEHPad = Parent->HandlerEHPad;
      // Create the cleanuppad
      NewPad = createEHPad(Child, EnclosingPad);
      break;
    default:
      llvm_unreachable("Unexpected region kind");
    }
  }

  // Record the handler so we can hook up exception edges later.
  Region->HandlerEHPad = NewPad;
}

Instruction *GenIR::createEHPad(EHRegion *Handler, Value *EnclosingPad) {
  LLVMContext &LLVMContext = *JitContext->LLVMContext;

  if (PersonalityFunction == nullptr) {
    // The EE provides a CorInfoHelpFunc handle for the actual personality
    // routine (CORINFO_HELP_EE_PERSONALITY_ROUTINE), but
    // A) the handle is opqaue, and we need an llvm::Function, and
    // B) LLVM's EH lowering expects the personality routine to have one of a
    //    few known names.
    // So, create an llvm::Function called ProcessCLRException (this is the
    // name of the function that CORINFO_HELP_EE_PERSONALITY_ROUTINE resolves
    // to) to match LLVM's expectations.
    // Use a dummy function type.
    FunctionType *PersonalityTy =
        FunctionType::get(Type::getVoidTy(LLVMContext), false);
    PersonalityFunction =
        Function::Create(PersonalityTy, Function::ExternalLinkage,
                         "ProcessCLRException", JitContext->CurrentModule);
    Function->setPersonalityFn(PersonalityFunction);
  }

  FlowGraphNode *HandlerCodeBlock = nullptr;
  fgAddNodeMSILOffset(&HandlerCodeBlock, Handler->StartMsilOffset);

  if (needSequencePoints()) {
    // We're supposed to setSequencePoint on entry to the handler; probably
    // will want to insert another point block before HandlerCodeBlock in
    // case HandlerCodeBlock itself has other preds.
    throw NotYetImplementedException("Handler sequence point");
  }

  Instruction *Pad;
  switch (Handler->Kind) {
  case ReaderBaseNS::RegionKind::RGN_Filter:
  case ReaderBaseNS::RegionKind::RGN_MCatch: {
    // Get the token to attach to the catchpad so we can emit it in the EH
    // tables (for filters we use System.Object), and the type to use in the
    // IR for the exception pointer.
    mdToken ClassToken;
    Type *CaughtType;
    if (Handler->Kind == ReaderBaseNS::RegionKind::RGN_MCatch) {
      ClassToken = rgnGetCatchClassToken(Handler);
      CORINFO_RESOLVED_TOKEN ResolvedToken;
      resolveToken(ClassToken, CORINFO_TOKENKIND_Class, &ResolvedToken);
      CaughtType = getType(CORINFO_TYPE_CLASS, ResolvedToken.hClass);
    } else {
      ClassToken = mdTypeRefNil;
      CaughtType =
          getType(CORINFO_TYPE_CLASS,
                  getBuiltinClass(CorInfoClassId::CLASSID_SYSTEM_OBJECT));
    }

    // Build an IR Value representing the metadata token for the caught type.
    static_assert(sizeof(mdToken) == sizeof(int32_t), "Unexpected token size");
    IntegerType *TokenType = Type::getInt32Ty(*JitContext->LLVMContext);
    Constant *TokenConstant = ConstantInt::get(TokenType, ClassToken);

    // Create a thunk block that splits the edge from exception dispatch to
    // the start of the handler (that will NOT be the target of any back-edges
    // from within the handler to the start of the handler).  This block holds
    // a call to the @llvm.eh.exceptionpointer intrinsic that retrieves the
    // exception pointer as a function of the catchpad.
    BasicBlock *PadBlock = createPointBlock(Handler->StartMsilOffset, "catch");
    LLVMBuilder->SetInsertPoint(PadBlock);
    Pad = Handler->CatchPad =
        LLVMBuilder->CreateCatchPad(EnclosingPad, {TokenConstant}, "catch.pad");
    llvm::Function *GetException =
        Intrinsic::getDeclaration(JitContext->CurrentModule,
                                  Intrinsic::eh_exceptionpointer, {CaughtType});
    Instruction *Exception =
        LLVMBuilder->CreateCall(GetException, {Pad}, "exn");
    LLVMBuilder->CreateBr(HandlerCodeBlock);
    // Propagate the exception object on the evaluation stack
    FlowGraphNode *ExceptionBlock = (FlowGraphNode *)Exception->getParent();
    ReaderStack *ExceptionBlockStack = createStack();
    ExceptionBlockStack->push((IRNode *)Exception);
    fgNodeSetOperandStack(ExceptionBlock, ExceptionBlockStack);
    fgNodeSetPropagatesOperandStack(ExceptionBlock, true);

    if (Handler->Kind == ReaderBaseNS::RegionKind::RGN_Filter) {
      // copy relevant attributes to the filter-handler.
      EHRegion *FilterHandler = Handler->HandlerRegion;
      FilterHandler->CatchPad = cast<CatchPadInst>(Pad);
      FilterHandler->HandlerEHPad = Handler->HandlerEHPad;
      FilterHandler->Exception = Exception;
    }
    break;
  }
  case ReaderBaseNS::RegionKind::RGN_Finally: {
    // Create a thunk block that splits the edge from exception dispatch to
    // the start of the handler (that will NOT be the target of any back-edges
    // from within the handler to the start of the handler).  It just has the
    // cleanuppad and a branch to the start of the handler.
    BasicBlock *PadBlock =
        createPointBlock(Handler->StartMsilOffset, "finally");
    LLVMBuilder->SetInsertPoint(PadBlock);
    Pad = Handler->CleanupPad =
        LLVMBuilder->CreateCleanupPad(EnclosingPad, {}, "finally.pad");
    LLVMBuilder->CreateBr(HandlerCodeBlock);
    break;
  }
  case ReaderBaseNS::RegionKind::RGN_Fault: {
    // Create a thunk block that splits the edge from exception dispatch to
    // the start of the handler (that will NOT be the target of any back-edges
    // from within the handler to the start of the handler).  It just has the
    // cleanuppad and a branch to the start of the handler.
    BasicBlock *PadBlock = createPointBlock(Handler->StartMsilOffset, "fault");
    LLVMBuilder->SetInsertPoint(PadBlock);
    // Add an i32 argument to faults, to distinguish them from filters.
    Value *Undef = UndefValue::get(Type::getInt32Ty(LLVMContext));
    Pad = Handler->CleanupPad =
        LLVMBuilder->CreateCleanupPad(EnclosingPad, {Undef}, "fault.pad");
    LLVMBuilder->CreateBr(HandlerCodeBlock);
    break;
  }
  default:
    llvm_unreachable("Unexpected handler type");
  }

  return Pad;
}

void GenIR::endFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrOffset) { return; }

IRNode *GenIR::findBlockSplitPointAfterNode(IRNode *Node) {
  if (Node == nullptr) {
    return nullptr;
  }
  return (IRNode *)((Instruction *)Node)->getNextNode();
}

// Get the last non-placekeeping node in block
IRNode *GenIR::fgNodeGetEndInsertIRNode(FlowGraphNode *FgNode) {
  BasicBlock *Block = (BasicBlock *)FgNode;
  uint32_t EndOffset = fgNodeGetEndMSILOffset(FgNode);
  Instruction *InsertInst = ContinuationStoreMap.lookup(EndOffset);
  if (InsertInst == nullptr) {
    InsertInst = Block->getTerminator();
  }
  return (IRNode *)InsertInst;
}

void GenIR::movePointBlocks(BasicBlock *OldBlock, BasicBlock *NewBlock) {
  BasicBlock *MoveBeforeBlock = NewBlock;
  uint32_t PointOffset = fgNodeGetStartMSILOffset((FlowGraphNode *)OldBlock);
  BasicBlock *PointBlock = OldBlock->getPrevNode();
  BasicBlock *PrevBlock = PointBlock->getPrevNode();
  while (
      (PointOffset == fgNodeGetStartMSILOffset((FlowGraphNode *)PointBlock)) &&
      (PointOffset == fgNodeGetEndMSILOffset((FlowGraphNode *)PointBlock))) {
    PointBlock->moveBefore(MoveBeforeBlock);
    MoveBeforeBlock = PointBlock;
    PointBlock = PrevBlock;
    PrevBlock = PrevBlock->getPrevNode();
  }
}

void GenIR::replaceFlowGraphNodeUses(FlowGraphNode *OldNode,
                                     FlowGraphNode *NewNode) {
  BasicBlock *OldBlock = (BasicBlock *)OldNode;
  movePointBlocks(OldBlock, NewNode);
  OldBlock->replaceAllUsesWith(NewNode);
  OldBlock->eraseFromParent();
}

bool fgEdgeIsNominal(FlowGraphEdgeIterator &FgEdgeIterator) {
  BasicBlock *Successor = fgEdgeIteratorGetSink(FgEdgeIterator);
  Instruction *First = Successor->getFirstNonPHI();
  return (First != nullptr) && First->isEHPad();
}

bool fgEdgeIteratorIsEnd(FlowGraphEdgeIterator &FgEdgeIterator) {
  return FgEdgeIterator.isEnd();
}

// Small helper function that gets the next IDOM. It was pulled out-of-line
// so that it can be called in a loop in FgNodeGetIDom.
// TODO (Issue #38): currently we conservatively return single predecessor
// without
// computing the immediate dominator.
FlowGraphNode *getNextIDom(FlowGraphNode *FgNode) {
  return (FlowGraphNode *)FgNode->getSinglePredecessor();
}

FlowGraphNode *GenIR::fgNodeGetIDom(FlowGraphNode *FgNode) {
  FlowGraphNode *Idom = getNextIDom(FgNode);

  //  If the dominating block is in an EH region
  //  and the original block is not in the same region, then this
  //  is not a true dominance relationship. Progress to the next
  //  dominator in the chain until we reach a true dominating
  //  block or there are no more blocks.
  while (nullptr != Idom && fgNodeGetRegion(Idom) != EhRegionTree &&
         fgNodeGetRegion(Idom) != fgNodeGetRegion(FgNode)) {
    Idom = getNextIDom(Idom);
  }

  return Idom;
}

FlowGraphEdgeIterator GenIR::fgNodeGetSuccessors(FlowGraphNode *FgNode) {
  EHRegion *Region = fgNodeGetRegion(FgNode);
  auto Impl =
      llvm::make_unique<FlowGraphSuccessorEdgeIteratorImpl>(FgNode, Region);
  FlowGraphEdgeIterator Iterator(std::move(Impl));
  return Iterator;
}

FlowGraphEdgeIterator GenIR::fgNodeGetPredecessors(FlowGraphNode *FgNode) {
  auto Impl = llvm::make_unique<FlowGraphPredecessorEdgeIteratorImpl>(FgNode);
  FlowGraphEdgeIterator Iterator(std::move(Impl));
  return Iterator;
}

bool fgEdgeIteratorMoveNextSuccessor(FlowGraphEdgeIterator &Iterator) {
  assert(!Iterator.isEnd() && "iterating past end!");
  Iterator.moveNext();
  return !Iterator.isEnd();
}

bool fgEdgeIteratorMoveNextPredecessor(FlowGraphEdgeIterator &Iterator) {
  assert(!Iterator.isEnd() && "iterating past end!");
  Iterator.moveNext();
  return !Iterator.isEnd();
}

FlowGraphNode *fgEdgeIteratorGetSink(FlowGraphEdgeIterator &Iterator) {
  assert(!Iterator.isEnd() && "iterator at end!");
  return Iterator.getSink();
}

FlowGraphNode *fgEdgeIteratorGetSource(FlowGraphEdgeIterator &Iterator) {
  assert(!Iterator.isEnd() && "iterator at end!");
  return Iterator.getSource();
}

FlowGraphSuccessorEdgeIteratorImpl::FlowGraphSuccessorEdgeIteratorImpl(
    FlowGraphNode *Fg, EHRegion *Region)
    : FlowGraphEdgeIteratorImpl(), SuccIterator(Fg->getTerminator()),
      SuccIteratorEnd(Fg->getTerminator(), true), NominalSucc(nullptr) {
  if (Region != nullptr) {
    Instruction *EHPad = Region->HandlerEHPad;
    if (EHPad != nullptr) {
      NominalSucc = EHPad->getParent();
    }
  }
}

bool FlowGraphSuccessorEdgeIteratorImpl::isEnd() {
  if (NominalSucc != nullptr) {
    // Haven't visited the nominal succ yet.
    return false;
  }
  return (SuccIterator == SuccIteratorEnd);
}

void FlowGraphSuccessorEdgeIteratorImpl::moveNext() {
  if (NominalSucc == nullptr) {
    // No nominal successor(s) left; just iterate the real successors.
    SuccIterator++;
  } else {
    // Done with nominal successor.
    NominalSucc = nullptr;
  }
}

FlowGraphNode *FlowGraphSuccessorEdgeIteratorImpl::getSink() {
  if (NominalSucc != nullptr) {
    return (FlowGraphNode *)NominalSucc;
  }
  return (SuccIterator == SuccIteratorEnd) ? nullptr
                                           : (FlowGraphNode *)*SuccIterator;
}

FlowGraphNode *FlowGraphSuccessorEdgeIteratorImpl::getSource() {
  return (FlowGraphNode *)SuccIterator.getSource();
}

void GenIR::fgNodeSetOperandStack(FlowGraphNode *Fg, ReaderStack *Stack) {
  FlowGraphInfoMap[Fg].TheReaderStack = Stack;
}

ReaderStack *GenIR::fgNodeGetOperandStack(FlowGraphNode *Fg) {
  return FlowGraphInfoMap[Fg].TheReaderStack;
}

bool GenIR::fgNodeIsVisited(FlowGraphNode *Fg) {
  return FlowGraphInfoMap[Fg].IsVisited;
}

void GenIR::fgNodeSetVisited(FlowGraphNode *Fg, bool Visited) {
  FlowGraphInfoMap[Fg].IsVisited = Visited;
}

// Check whether this node propagates operand stack.
bool GenIR::fgNodePropagatesOperandStack(FlowGraphNode *Fg) {
  return FlowGraphInfoMap[Fg].PropagatesOperandStack;
}

// Set whether this node propagates operand stack.
void GenIR::fgNodeSetPropagatesOperandStack(FlowGraphNode *Fg,
                                            bool PropagatesOperandStack) {
  FlowGraphInfoMap[Fg].PropagatesOperandStack = PropagatesOperandStack;
}

FlowGraphNode *GenIR::fgNodeGetNext(FlowGraphNode *FgNode) {
  if (FgNode == &(Function->getBasicBlockList().back())) {
    return nullptr;
  } else {
    return (FlowGraphNode *)((BasicBlock *)FgNode)->getNextNode();
  }
}

FlowGraphNode *GenIR::fgPrePhase(FlowGraphNode *Fg) { return FirstMSILBlock; }

void GenIR::fgPostPhase() { DoneBuildingFlowGraph = true; }

void GenIR::fgAddLabelToBranchList(IRNode *LabelNode, IRNode *BranchNode) {
  return;
}

void GenIR::insertIBCAnnotations() { return; }

IRNode *GenIR::fgNodeFindStartLabel(FlowGraphNode *Block) { return nullptr; }

bool GenIR::fgBlockHasFallThrough(FlowGraphNode *Block) { return false; }

#pragma endregion

#pragma region MSIL OPCODES

//===----------------------------------------------------------------------===//
//
// MSIL READER opcode to LLVM IR translation
//
//===----------------------------------------------------------------------===//

IRNode *GenIR::loadConstantI4(int32_t Constant) {
  uint32_t NumBits = 32;
  bool IsSigned = true;

  return (IRNode *)ConstantInt::get(*JitContext->LLVMContext,
                                    APInt(NumBits, Constant, IsSigned));
}

IRNode *GenIR::loadConstantI8(int64_t Constant) {
  uint32_t NumBits = 64;
  bool IsSigned = true;

  return (IRNode *)ConstantInt::get(*JitContext->LLVMContext,
                                    APInt(NumBits, Constant, IsSigned));
}

IRNode *GenIR::loadConstantI(size_t Constant) {
  uint32_t NumBits = TargetPointerSizeInBits;
  bool IsSigned = true;
  return (IRNode *)ConstantInt::get(*JitContext->LLVMContext,
                                    APInt(NumBits, Constant, IsSigned));
}

IRNode *GenIR::loadConstantR4(float Value) {
  return (IRNode *)ConstantFP::get(*JitContext->LLVMContext, APFloat(Value));
}

IRNode *GenIR::loadConstantR8(double Value) {
  return (IRNode *)ConstantFP::get(*JitContext->LLVMContext, APFloat(Value));
}

// Load the array length field.
IRNode *GenIR::loadLen(IRNode *Array, bool ArrayMayBeNull) {
  // For length element type does not matter.
  Array = this->ensureIsArray(Array, nullptr);

  if (ArrayMayBeNull && UseExplicitNullChecks) {
    // Check whether the array pointer, rather than the pointer to its
    // length field, is null.
    Array = genNullCheck(Array);
    ArrayMayBeNull = false;
  }

  // Length field is at index 1. Get its address.
  Value *LengthFieldAddress = LLVMBuilder->CreateStructGEP(nullptr, Array, 1);

  // Load and return the length.
  // TODO: this load cannot be aliased.
  Value *Length = makeLoad(LengthFieldAddress, false, ArrayMayBeNull);

  // Result is an unsigned native int.
  IRNode *Result = convertToStackType((IRNode *)Length,
                                      CorInfoType::CORINFO_TYPE_NATIVEUINT);
  return (IRNode *)Result;
}

// Load the string length field.
IRNode *GenIR::loadStringLen(IRNode *Address) {
  // Address should be a managed pointer type.
  Type *AddressTy = Address->getType();
  ASSERT(GcInfo::isGcPointer(AddressTy));

  // Optionally do an explicit null check.
  bool NullCheckBeforeLoad = UseExplicitNullChecks;
  if (NullCheckBeforeLoad) {
    // Check whether the string pointer, rather than the pointer to its
    // length field, is null.
    Address = genNullCheck(Address);
  }

  // See if this type is the one we use to model strings, and if not, do the
  // requisite cast (note we can see System.__Canon* here, for instance).
  Type *BuiltInStringType = getBuiltInStringType();
  if (AddressTy != BuiltInStringType) {
    Address =
        (IRNode *)LLVMBuilder->CreatePointerCast(Address, BuiltInStringType);
  }

  // Length field is at field index 1. Get its address.
  Value *LengthFieldAddress = LLVMBuilder->CreateStructGEP(nullptr, Address, 1);

  // Load and return the length.
  // TODO: this load cannot be aliased.
  Value *Length = makeLoad(LengthFieldAddress, false, !NullCheckBeforeLoad);
  return (IRNode *)Length;
}

// Load a character from a string.
IRNode *GenIR::stringGetChar(IRNode *Address, IRNode *Index) {
  // Address should be a managed pointer type.
  Type *AddressTy = Address->getType();
  ASSERT(GcInfo::isGcPointer(AddressTy));

  // Optionally do an explicit null check.
  bool NullCheckBeforeLoad = UseExplicitNullChecks;
  if (NullCheckBeforeLoad) {
    // Check whether the string pointer, rather than the pointer to its
    // length field, is null.
    Address = genNullCheck(Address);
  }

  // See if this type is the one we use to model strings, and if not, do the
  // requisite cast (note we can see System.__Canon* here, for instance).
  Type *BuiltInStringType = getBuiltInStringType();
  if (AddressTy != BuiltInStringType) {
    Address =
        (IRNode *)LLVMBuilder->CreatePointerCast(Address, BuiltInStringType);
  }

  // Cache the context
  LLVMContext &Context = *JitContext->LLVMContext;

  // Build up gep indices.
  Value *Indexes[] = {ConstantInt::get(Type::getInt32Ty(Context), 0),
                      ConstantInt::get(Type::getInt32Ty(Context), 2), Index};

  // Index to the desired char.
  Value *CharAddress = LLVMBuilder->CreateInBoundsGEP(Address, Indexes);

  // Load and return the char.
  Value *Char = makeLoad(CharAddress, false, !NullCheckBeforeLoad);
  IRNode *Result =
      convertToStackType((IRNode *)Char, CorInfoType::CORINFO_TYPE_CHAR);

  return Result;
}

IRNode *GenIR::loadNull() {
  Type *NullType =
      getManagedPointerType(Type::getInt8Ty(*JitContext->LLVMContext));
  return (IRNode *)Constant::getNullValue(NullType);
}

IRNode *GenIR::ckFinite(IRNode *Arg) {
  // Reinterpret the float as an integer, extract exponent, and compare to
  // an exponent of all 1's.
  Type *ArgTy = Arg->getType();
  assert(ArgTy->isFloatingPointTy());
  uint32_t ArgSize = ArgTy->getPrimitiveSizeInBits();
  Type *IntTy = nullptr;
  Value *ExponentMask = nullptr;
  if (ArgSize == 32) {
    IntTy = Type::getInt32Ty(*JitContext->LLVMContext);
    ExponentMask = ConstantInt::get(IntTy, APInt(ArgSize, 0x7F800000));
  } else {
    IntTy = Type::getInt64Ty(*JitContext->LLVMContext);
    ExponentMask = ConstantInt::get(IntTy, APInt(ArgSize, 0x7FF0000000000000));
  }
  Value *IntBits = LLVMBuilder->CreateBitCast(Arg, IntTy);
  Value *ExponentBits = LLVMBuilder->CreateAnd(IntBits, ExponentMask);
  Value *IsFinite = LLVMBuilder->CreateICmpEQ(ExponentBits, ExponentMask);
  CorInfoHelpFunc HelperId = CORINFO_HELP_OVERFLOW;
  genConditionalThrow(IsFinite, HelperId, "ThrowOverflow");
  return Arg;
}

IRNode *GenIR::unaryOp(ReaderBaseNS::UnaryOpcode Opcode, IRNode *Arg1) {

  if (Opcode == ReaderBaseNS::Neg) {
    if (Arg1->getType()->isFloatingPointTy()) {
      return (IRNode *)LLVMBuilder->CreateFNeg(Arg1);
    } else {
      return (IRNode *)LLVMBuilder->CreateNeg(Arg1);
    }
  }

  if (Opcode == ReaderBaseNS::Not) {
    return (IRNode *)LLVMBuilder->CreateNot(Arg1);
  }

  ASSERTNR(UNREACHED);
  return nullptr;
}

IRNode *GenIR::binaryOp(ReaderBaseNS::BinaryOpcode Opcode, IRNode *Arg1,
                        IRNode *Arg2) {

  struct BinaryTriple {
    union {
      Instruction::BinaryOps Opcode;
      Intrinsic::ID Intrinsic;
    } Op;
    bool IsOverflow;
    bool IsUnsigned;

    // Constructor for operations that map to LLVM opcodes
    BinaryTriple(Instruction::BinaryOps Opcode, bool IsOverflow,
                 bool IsUnsigned)
        : IsOverflow(IsOverflow), IsUnsigned(IsUnsigned) {
      Op.Opcode = Opcode;
    }

    // Constructor for operations that map to LLVM intrinsics
    BinaryTriple(Intrinsic::ID Intrinsic, bool IsOverflow, bool IsUnsigned)
        : IsOverflow(IsOverflow), IsUnsigned(IsUnsigned) {
      Op.Intrinsic = Intrinsic;
    }

    // Default constructor for invalid cases
    BinaryTriple() {}
  };

  static const BinaryTriple IntMap[ReaderBaseNS::LastBinaryOpcode] = {
      {Instruction::BinaryOps::Add, false, false},  // ADD
      {Intrinsic::sadd_with_overflow, true, false}, // ADD_OVF
      {Intrinsic::uadd_with_overflow, true, true},  // ADD_OVF_UN
      {Instruction::BinaryOps::And, false, false},  // AND
      {Instruction::BinaryOps::SDiv, false, false}, // DIV
      {Instruction::BinaryOps::UDiv, false, true},  // DIV_UN
      {Instruction::BinaryOps::Mul, false, false},  // MUL
      {Intrinsic::smul_with_overflow, true, false}, // MUL_OVF
      {Intrinsic::umul_with_overflow, true, true},  // MUL_OVF_UN
      {Instruction::BinaryOps::Or, false, false},   // OR
      {Instruction::BinaryOps::SRem, false, false}, // REM
      {Instruction::BinaryOps::URem, false, true},  // REM_UN
      {Instruction::BinaryOps::Sub, false, false},  // SUB
      {Intrinsic::ssub_with_overflow, true, false}, // SUB_OVF
      {Intrinsic::usub_with_overflow, true, true},  // SUB_OVF_UN
      {Instruction::BinaryOps::Xor, false, false}   // XOR
  };

  static const BinaryTriple FloatMap[ReaderBaseNS::LastBinaryOpcode] = {
      {Instruction::BinaryOps::FAdd, false, false}, // ADD
      {},                                           // ADD_OVF (invalid)
      {},                                           // ADD_OVF_UN (invalid)
      {},                                           // AND (invalid)
      {Instruction::BinaryOps::FDiv, false, false}, // DIV
      {},                                           // DIV_UN (invalid)
      {Instruction::BinaryOps::FMul, false, false}, // MUL
      {},                                           // MUL_OVF (invalid)
      {},                                           // MUL_OVF_UN (invalid)
      {},                                           // OR (invalid)
      {Instruction::BinaryOps::FRem, false, false}, // REM
      {},                                           // REM_UN (invalid)
      {Instruction::BinaryOps::FSub, false, false}, // SUB
      {},                                           // SUB_OVF (invalid)
      {},                                           // SUB_OVF_UN (invalid)
      {},                                           // XOR (invalid)
  };

  Type *Type1 = Arg1->getType();
  Type *Type2 = Arg2->getType();
  Type *ResultType = binaryOpType(Opcode, Type1, Type2);
  Type *ArithType = ResultType;

  // If the result is a pointer, see if we have simple
  // pointer + int op...
  if (ResultType->isPointerTy()) {
    switch (Opcode) {
    case ReaderBaseNS::Add: {
      IRNode *PtrAdd = genPointerAdd(Arg1, Arg2);
      if (PtrAdd != nullptr) {
        return PtrAdd;
      }
      break;
    }

    case ReaderBaseNS::Sub: {
      IRNode *PtrSub = genPointerSub(Arg1, Arg2);
      if (PtrSub != nullptr) {
        return PtrSub;
      }
      break;
    }

    case ReaderBaseNS::AddOvfUn:
    case ReaderBaseNS::SubOvfUn: {
      // Arithmetic with overflow must use an appropriately-sized integer to
      // perform the arithmetic, then convert the result back to the pointer
      // type.
      ArithType =
          Type::getIntNTy(*JitContext->LLVMContext, TargetPointerSizeInBits);
      break;
    }
    default:
      // No fixup required
      break;
    }
  }

  assert(ArithType == ResultType || ResultType->isPointerTy());

  bool IsFloat = ResultType->isFloatingPointTy();
  const BinaryTriple *Triple = IsFloat ? FloatMap : IntMap;

  bool IsOverflow = Triple[Opcode].IsOverflow;
  bool IsUnsigned = Triple[Opcode].IsUnsigned;

  if (Type1 != ArithType) {
    Arg1 = convert(ArithType, Arg1, !IsUnsigned);
  }

  if (Type2 != ArithType) {
    Arg2 = convert(ArithType, Arg2, !IsUnsigned);
  }

  IRNode *Result;
  if (IsFloat && Opcode == ReaderBaseNS::BinaryOpcode::Rem) {
    // FRem must be lowered to a JIT helper call to avoid undefined symbols
    // during emit.
    //
    // TODO: it may be possible to delay this lowering by updating the JIT
    // APIs to allow the definition of a target library (via TargeLibraryInfo).
    CorInfoHelpFunc Helper = CORINFO_HELP_UNDEF;
    if (ResultType->isFloatTy()) {
      Helper = CORINFO_HELP_FLTREM;
    } else if (ResultType->isDoubleTy()) {
      Helper = CORINFO_HELP_DBLREM;
    } else {
      llvm_unreachable("Bad floating point type!");
    }

    const bool MayThrow = false;
    Result = (IRNode *)callHelperImpl(Helper, MayThrow, ResultType, Arg1, Arg2)
                 .getInstruction();
  } else if (IsOverflow) {
    // Call the appropriate intrinsic.  Its result is a pair of the arithmetic
    // result and a bool indicating whether the operation overflows.
    Value *Intrinsic = Intrinsic::getDeclaration(
        JitContext->CurrentModule, Triple[Opcode].Op.Intrinsic, ArithType);
    Value *Args[] = {Arg1, Arg2};
    const bool MayThrow = false;
    Value *Pair = makeCall(Intrinsic, MayThrow, Args).getInstruction();

    // Extract the bool and raise an overflow exception if set.
    Value *OvfBool = LLVMBuilder->CreateExtractValue(Pair, 1, "Ovf");
    genConditionalThrow(OvfBool, CORINFO_HELP_OVERFLOW, "ThrowOverflow");

    // Extract the result.
    Result = (IRNode *)LLVMBuilder->CreateExtractValue(Pair, 0);
  } else {
    // Create a simple binary operation.
    Instruction::BinaryOps Op = Triple[Opcode].Op.Opcode;

    if ((Op == Instruction::BinaryOps::SDiv) ||
        (Op == Instruction::BinaryOps::UDiv) ||
        (Op == Instruction::BinaryOps::SRem) ||
        (Op == Instruction::BinaryOps::URem)) {
      // Integer divide and remainder throw a DivideByZeroException
      // if the divisor is zero
      if (UseExplicitZeroDivideChecks) {
        Value *IsZero = LLVMBuilder->CreateIsNull(Arg2);
        genConditionalThrow(IsZero, CORINFO_HELP_THROWDIVZERO,
                            "ThrowDivideByZero");
      } else {
        // This configuration isn't really supported.  To support it we'd
        // need to annotate the divide we're about to generate as possibly
        // throwing an exception (that would be raised from a machine trap).
      }
    }

    Result = (IRNode *)LLVMBuilder->CreateBinOp(Op, Arg1, Arg2);
  }

  if (ResultType != ArithType) {
    assert(ResultType->isPointerTy());
    assert(ArithType->isIntegerTy());

    Result = (IRNode *)LLVMBuilder->CreateIntToPtr(Result, ResultType);
  }

  return Result;
}

Type *GenIR::binaryOpType(ReaderBaseNS::BinaryOpcode Opcode, Type *Type1,
                          Type *Type2) {
  // Roughly follows ECMA-355, Table III.2.
  // If both types are floats, the result is the larger float type.
  if (Type1->isFloatingPointTy() && Type2->isFloatingPointTy()) {
    uint32_t Size1 = Type1->getPrimitiveSizeInBits();
    uint32_t Size2 = Type2->getPrimitiveSizeInBits();
    return (Size1 >= Size2 ? Type1 : Type2);
  }

  const bool Type1IsInt = Type1->isIntegerTy();
  const bool Type2IsInt = Type2->isIntegerTy();
  const bool Type1IsPtr = Type1->isPointerTy();
  const bool Type2IsPtr = Type2->isPointerTy();

  assert((Type1IsInt || Type1IsPtr) &&
         "unexpected operand type1 for binary op");
  assert((Type2IsInt || Type2IsPtr) &&
         "unexpected operand type2 for binary op");

  const uint32_t Size1 =
      Type1IsPtr ? TargetPointerSizeInBits : Type1->getPrimitiveSizeInBits();
  const uint32_t Size2 =
      Type2IsPtr ? TargetPointerSizeInBits : Type2->getPrimitiveSizeInBits();

  // If both types are integers, sizes must match, or one of the sizes must be
  // native int and the other must be smaller.
  if (Type1IsInt && Type2IsInt) {
    if (Size1 == Size2) {
      return Type1;
    }
    if ((Size1 == TargetPointerSizeInBits) && (Size1 > Size2)) {
      return Type1;
    }
    if ((Size2 == TargetPointerSizeInBits) && (Size2 > Size1)) {
      return Type2;
    }
  } else {
    const bool Type1IsUnmanagedPointer = GcInfo::isUnmanagedPointer(Type1);
    const bool Type2IsUnmanagedPointer = GcInfo::isUnmanagedPointer(Type2);
    const bool IsStrictlyAdd = (Opcode == ReaderBaseNS::Add);
    const bool IsAdd = IsStrictlyAdd || (Opcode == ReaderBaseNS::AddOvf) ||
                       (Opcode == ReaderBaseNS::AddOvfUn);
    const bool IsStrictlySub = (Opcode == ReaderBaseNS::Sub);
    const bool IsSub = IsStrictlySub || (Opcode == ReaderBaseNS::SubOvf) ||
                       (Opcode == ReaderBaseNS::SubOvfUn);
    const bool IsStrictlyAddOrSub = IsStrictlyAdd || IsStrictlySub;
    const bool IsAddOrSub = IsAdd || IsSub;

    // If we see a mixture of int and unmanaged pointer, the result
    // is generally a native int, with a few special cases where we
    // preserve pointer-ness.
    if (Type1IsUnmanagedPointer || Type2IsUnmanagedPointer) {
      // ptr +/- int = ptr
      if (IsAddOrSub && Type1IsUnmanagedPointer && Type2IsInt &&
          (Size1 >= Size2)) {
        return Type1;
      }
      // int + ptr = ptr
      if (IsAdd && Type1IsInt && Type2IsUnmanagedPointer && (Size2 >= Size1)) {
        return Type2;
      }
      // Otherwise type result as native int as long as there's no truncation
      // going on.
      if ((Size1 <= TargetPointerSizeInBits) &&
          (Size2 <= TargetPointerSizeInBits)) {
        return Type::getIntNTy(*JitContext->LLVMContext,
                               TargetPointerSizeInBits);
      }
    } else if (GcInfo::isGcPointer(Type1)) {
      if (IsSub && GcInfo::isGcPointer(Type2)) {
        // The difference of two managed pointers is a native int.
        return Type::getIntNTy(*JitContext->LLVMContext,
                               TargetPointerSizeInBits);
      } else if (IsStrictlyAddOrSub && Type2IsInt && (Size1 >= Size2)) {
        // Special case for just strict add and sub: if Type1 is a managed
        // pointer and Type2 is an integer, the result is Type1. We see the
        // add case in some internal uses in reader base. We see the sub case
        // in some IL stubs.
        return Type1;
      }
    }
  }

  // All other combinations are invalid.
  ASSERT(UNREACHED);
  return nullptr;
}

// Handle simple field access via a structural GEP.
IRNode *GenIR::simpleFieldAddress(IRNode *BaseAddress,
                                  CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                  CORINFO_FIELD_INFO *FieldInfo) {
  Value *Address = nullptr;

  if (!(JitContext->Flags & CORJIT_FLG_READYTORUN)) {
    // Determine field index and referent type.
    CORINFO_FIELD_HANDLE FieldHandle = ResolvedToken->hField;
    Type *BaseAddressTy = BaseAddress->getType();
    ASSERT(BaseAddressTy->isPointerTy());
    Type *BaseObjTy = cast<PointerType>(BaseAddressTy)->getElementType();

    if (BaseObjTy->isStructTy() &&
        (FieldIndexMap->find(FieldHandle) != FieldIndexMap->end())) {

      const uint32_t FieldIndex = (*FieldIndexMap)[FieldHandle];
      StructType *BaseObjStructTy = cast<StructType>(BaseObjTy);

      // Double-check that the field index is sensible. Note
      // in unverifiable IL we may not have proper referent types and
      // so may see what appear to be unrelated field accesses.
      if (BaseObjStructTy->getNumElements() > FieldIndex) {
        ASSERT(JitContext->CurrentModule->getDataLayout()
                   .getStructLayout(BaseObjStructTy)
                   ->getElementOffset(FieldIndex) == FieldInfo->offset);

        Address =
            LLVMBuilder->CreateStructGEP(nullptr, BaseAddress, FieldIndex);
      }
    }
  }

  if (Address == nullptr) {
    // We can't find the struct type or the field index, or the field index
    // doesn't make sense for the referent type we have on hand.
    // It can happen, for example, if we cast native int pointer to
    // IntPtr pointer. Unfortunately we can't get the enclosing type
    // via ICorJitInfo interface so we can't create a struct version of GEP.
    // Also, in ReadyToRun mode we have to get field offset in a special way.

    if ((JitContext->Flags & CORJIT_FLG_READYTORUN) &&
        (FieldInfo->fieldAccessor == CORINFO_FIELD_INSTANCE_WITH_BASE)) {
      void *EmbHandle = FieldInfo->fieldLookup.addr;
      if (EmbHandle != nullptr) {
        void *RealHandle = nullptr;
        bool IsIndirect = (FieldInfo->fieldLookup.accessType == IAT_PVALUE);
        const bool IsReadOnly = true;
        const bool IsRelocatable = true;
        const bool IsCallTarget = false;
        std::string HandleName = "BaseSize";
        IRNode *BaseClassSize =
            handleToIRNode(HandleName, EmbHandle, RealHandle, IsIndirect,
                           IsReadOnly, IsRelocatable, IsCallTarget);
        BaseAddress = binaryOp(ReaderBaseNS::Add, BaseAddress, BaseClassSize);
      }
    }

    if (FieldInfo->offset != 0) {
      IRNode *FieldOffset = loadConstantI(FieldInfo->offset);
      Address = binaryOp(ReaderBaseNS::Add, BaseAddress, FieldOffset);
    } else {
      Address = BaseAddress;
    }
  }

  return (IRNode *)Address;
}

// Handle pointer + int by emitting a flattened LLVM GEP.
IRNode *GenIR::genPointerAdd(IRNode *Arg1, IRNode *Arg2) {
  // Assume 1 is base and 2 is offset
  IRNode *BasePtr = Arg1;
  IRNode *Offset = Arg2;

  // Reconsider based on types.
  bool Arg1IsPointer = Arg1->getType()->isPointerTy();
  bool Arg2IsPointer = Arg2->getType()->isPointerTy();
  ASSERT(Arg1IsPointer || Arg2IsPointer);

  // Bail if both args are already pointer types.
  if (Arg1IsPointer && Arg2IsPointer) {
    return nullptr;
  }

  // Swap base and offset if we got it wrong.
  if (Arg2IsPointer) {
    BasePtr = Arg2;
    Offset = Arg1;
  }

  // Bail if offset is not integral.
  Type *OffsetTy = Offset->getType();
  if (!OffsetTy->isIntegerTy()) {
    return nullptr;
  }

  // Build an LLVM GEP for the resulting address.
  // For now we "flatten" to byte offsets.
  Type *CharPtrTy = Type::getInt8PtrTy(
      *JitContext->LLVMContext, BasePtr->getType()->getPointerAddressSpace());
  Value *BasePtrCast = LLVMBuilder->CreateBitCast(BasePtr, CharPtrTy);
  Value *ResultPtr = LLVMBuilder->CreateInBoundsGEP(BasePtrCast, Offset);
  return (IRNode *)ResultPtr;
}

// Handle pointer - int by emitting a flattened LLVM GEP.
IRNode *GenIR::genPointerSub(IRNode *Arg1, IRNode *Arg2) {

  // Assume 1 is base and 2 is offset
  IRNode *BasePtr = Arg1;
  IRNode *Offset = Arg2;

  // Reconsider based on types.
  bool Arg1IsPointer = Arg1->getType()->isPointerTy();
  bool Arg2IsPointer = Arg2->getType()->isPointerTy();
  ASSERT(Arg1IsPointer);

  // Bail if both args are already pointer types.
  if (Arg1IsPointer && Arg2IsPointer) {
    return nullptr;
  }

  // Bail if offset is not integral.
  Type *OffsetTy = Offset->getType();
  if (!OffsetTy->isIntegerTy()) {
    return nullptr;
  }

  // Build an LLVM GEP for the resulting address.
  // For now we "flatten" to byte offsets.
  Type *CharPtrTy = Type::getInt8PtrTy(
      *JitContext->LLVMContext, BasePtr->getType()->getPointerAddressSpace());
  Value *BasePtrCast = LLVMBuilder->CreateBitCast(BasePtr, CharPtrTy);
  Value *NegOffset = LLVMBuilder->CreateNeg(Offset);
  Value *ResultPtr = LLVMBuilder->CreateGEP(BasePtrCast, NegOffset);
  return (IRNode *)ResultPtr;
}

void GenIR::storeLocal(uint32_t LocalOrdinal, IRNode *Arg1,
                       ReaderAlignType Alignment, bool IsVolatile) {
  uint32_t LocalIndex = LocalOrdinal;
  Value *LocalAddress = LocalVars[LocalIndex];
  Type *LocalTy = LocalAddress->getType()->getPointerElementType();
  IRNode *Value =
      convertFromStackType(Arg1, LocalVarCorTypes[LocalIndex], LocalTy);
  storeAtAddressNoBarrierNonNull((IRNode *)LocalAddress, Value, LocalTy,
                                 IsVolatile);
}

IRNode *GenIR::loadLocal(uint32_t LocalOrdinal) {
  uint32_t LocalIndex = LocalOrdinal;
  Value *LocalAddress = LocalVars[LocalIndex];
  Type *LocalTy = LocalAddress->getType()->getPointerElementType();
  const bool IsVolatile = false;
  return loadAtAddressNonNull((IRNode *)LocalAddress, LocalTy,
                              LocalVarCorTypes[LocalIndex], Reader_AlignNatural,
                              IsVolatile);
}

IRNode *GenIR::loadLocalAddress(uint32_t LocalOrdinal) {
  assert(HasAddressTaken && "need to detect this in first pass");
  uint32_t LocalIndex = LocalOrdinal;
  return loadManagedAddress(LocalVars[LocalIndex]);
}

void GenIR::storeArg(uint32_t ArgOrdinal, IRNode *Arg1,
                     ReaderAlignType Alignment, bool IsVolatile) {
  uint32_t ArgIndex = MethodSignature.getArgIndexForILArg(ArgOrdinal);
  Value *ArgAddress = Arguments[ArgIndex];
  Type *ArgTy = ArgAddress->getType()->getPointerElementType();
  const CallArgType &CallArgType = MethodSignature.getArgumentTypes()[ArgIndex];
  IRNode *Value = convertFromStackType(Arg1, CallArgType.CorType, ArgTy);

  if (ABIMethodSig.getArgumentInfo(ArgIndex).getKind() ==
      ABIArgInfo::Indirect) {
    storeIndirectArg(CallArgType, Value, ArgAddress, IsVolatile);
  } else {
    storeAtAddressNoBarrierNonNull((IRNode *)ArgAddress, Value, ArgTy,
                                   IsVolatile);
  }
}

IRNode *GenIR::loadArg(uint32_t ArgOrdinal, bool IsJmp) {
  uint32_t ArgIndex =
      IsJmp ? ArgOrdinal : MethodSignature.getArgIndexForILArg(ArgOrdinal);
  Value *ArgAddress = Arguments[ArgIndex];

  Type *ArgTy = ArgAddress->getType()->getPointerElementType();
  const bool IsVolatile = false;
  CorInfoType CorType = MethodSignature.getArgumentTypes()[ArgIndex].CorType;
  return loadAtAddressNonNull((IRNode *)ArgAddress, ArgTy, CorType,
                              Reader_AlignNatural, IsVolatile);
}

IRNode *GenIR::loadArgAddress(uint32_t ArgOrdinal) {
  assert(HasAddressTaken && "need to detect this in first pass");
  uint32_t ArgIndex = MethodSignature.getArgIndexForILArg(ArgOrdinal);
  Value *Address = Arguments[ArgIndex];
  return loadManagedAddress(Address);
}

IRNode *GenIR::loadManagedAddress(Value *UnmanagedAddress) {
  Type *ElementType = UnmanagedAddress->getType()->getPointerElementType();
  Type *ManagedPointerType = getManagedPointerType(ElementType);

  // ldloca and ldarga have to return managed pointers. Since we can't influence
  // the address space of the pointer alloca returns we have to add an
  // AddrSpaceCast to ManagedPointerType here. Normally we try to avoid such
  // casts.
  return (IRNode *)LLVMBuilder->CreateAddrSpaceCast(UnmanagedAddress,
                                                    ManagedPointerType);
}

// Load the address of the field described by ResolvedToken
// from the object Obj.
IRNode *GenIR::loadFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                IRNode *Obj) {
  bool ObjIsThis = objIsThis(Obj);
  CORINFO_FIELD_INFO FieldInfo;

  // TODO: optimize 'this' pointer reference for contextful classes

  int32_t AccessFlags = ObjIsThis ? CORINFO_ACCESS_THIS : CORINFO_ACCESS_ANY;
  AccessFlags |= CORINFO_ACCESS_ADDRESS;

  getFieldInfo(ResolvedToken, (CORINFO_ACCESS_FLAGS)AccessFlags, &FieldInfo);

  IRNode *Result = getFieldAddress(ResolvedToken, &FieldInfo, Obj, true);

  return Result;
}

// Get the address of the field described by ResolvedToken
// from the object Obj. Optionally null check.
IRNode *GenIR::getFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               CORINFO_FIELD_INFO *FieldInfo, IRNode *Obj,
                               bool MustNullCheck) {
  // Get the field address.
  Type *AddressTy = Obj->getType();

  // If we have a pointer-sized integer, convert to an unmanaged pointer.
  if (AddressTy->isIntegerTy()) {
    assert((AddressTy->getPrimitiveSizeInBits() == TargetPointerSizeInBits) &&
           "expected pointer-sized int");
    Type *PointerTy = getUnmanagedPointerType(AddressTy);
    Obj = (IRNode *)LLVMBuilder->CreateIntToPtr(Obj, PointerTy);
    AddressTy = PointerTy;
  }

  ASSERT(AddressTy->isPointerTy());
  const bool IsGcPointer = GcInfo::isGcPointer(AddressTy);
  Value *RawAddress = rdrGetFieldAddress(ResolvedToken, FieldInfo, Obj,
                                         IsGcPointer, MustNullCheck);

  // Determine the type of the field element.
  CorInfoType CorInfoType = FieldInfo->fieldType;
  CORINFO_CLASS_HANDLE Class = FieldInfo->structType;
  Type *FieldTy = getType(CorInfoType, Class);

  // Create the appropriately typed pointer to field.
  Type *FieldPtrTy;
  if (IsGcPointer) {
    FieldPtrTy = getManagedPointerType(FieldTy);
  } else {
    FieldPtrTy = getUnmanagedPointerType(FieldTy);
  }

  // Cast field address -- note in many cases this will not add IR
  // as the field address already has the right type.
  Value *Address = LLVMBuilder->CreateBitCast(RawAddress, FieldPtrTy);

  return (IRNode *)Address;
}

IRNode *GenIR::loadField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Obj,
                         ReaderAlignType AlignmentPrefix, bool IsVolatile) {
  // Gather relevant facts about this field access.
  bool ObjIsThis = objIsThis(Obj);
  int32_t AccessFlags = ObjIsThis ? CORINFO_ACCESS_THIS : CORINFO_ACCESS_ANY;
  AccessFlags |= CORINFO_ACCESS_GET;
  CORINFO_FIELD_INFO FieldInfo;

  getFieldInfo(ResolvedToken, (CORINFO_ACCESS_FLAGS)AccessFlags, &FieldInfo);
  // Determine the type of the field element.
  CorInfoType CorInfoType = FieldInfo.fieldType;
  CORINFO_CLASS_HANDLE Class = FieldInfo.structType;
  Type *FieldTy = getType(CorInfoType, Class);

  if (doSimdIntrinsicOpt()) {
    Type *ObjType = Obj->getType();
    auto &Context = LLVMBuilder->getContext();

    if (ObjType->isVectorTy()) {
      if (FieldTy->isStructTy()) {
        std::string FieldName = FieldTy->getStructName();
        if (FieldTy->getStructName() == "System.Numerics.Register") {
          return Obj;
        } else {
          // We haven't information abouth this field. Try to return struct
          // type.
          assert(VectorTypeToStructType.count(ObjType));
          IRNode *Pointer = (IRNode *)createAlloca(Obj->getType(), nullptr);
          LLVMBuilder->CreateStore(Obj, Pointer);
          Obj = (IRNode *)LLVMBuilder->CreateBitCast(
              Pointer,
              llvm::PointerType::get(VectorTypeToStructType[ObjType], 0));
        }
      } else {
        if (ObjType->getVectorElementType() != FieldTy) {
          Type *AssumingVectorType = llvm::VectorType::get(
              FieldTy, ObjType->getScalarSizeInBits() *
                           ObjType->getVectorNumElements() /
                           FieldTy->getScalarSizeInBits());
          Obj = (IRNode *)LLVMBuilder->CreateBitCast(Obj, AssumingVectorType);
          ObjType = Obj->getType();
        }
        unsigned ElementSize =
            ObjType->getVectorElementType()->getScalarSizeInBits();
        assert(ElementSize == FieldTy->getScalarSizeInBits());
        unsigned IndexInVector = FieldInfo.offset * 8 / ElementSize;
        IRNode *Index = (IRNode *)ConstantInt::get(Type::getInt32Ty(Context),
                                                   IndexInVector);
        IRNode *Result =
            (IRNode *)LLVMBuilder->CreateExtractElement(Obj, Index);
        return convertToStackType((IRNode *)Result, CorInfoType);
      }
    }
  }

  // LoadStaticField and GetFieldAddress already generate
  // checkFieldAuthorization calls, so
  // only put them in the paths that terminate other ways.

  // It's legal to use LDFLD on a static field. In that case,
  // we need to make sure that we evaluate the object address for
  // side-effects, but then we treat it like LDSFLD.
  if (FieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) {
    pop(Obj);

    // TODO: check that unaligned load from static field is illegal.
    return loadStaticField(ResolvedToken, IsVolatile);
  }

  // Fields typed as GC pointers are always aligned,
  // so ignore any smaller alignment prefix
  if (FieldTy->isPointerTy() &&
      GcInfo::isGcPointer(cast<PointerType>(FieldTy))) {
    AlignmentPrefix = Reader_AlignNatural;
  }

  // If accessing the field requires a helper, then we need to
  // call the helper routine; we can't just load the address
  // and do a load-indirect off it.
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INSTANCE_HELPER) {
    handleMemberAccess(FieldInfo.accessAllowed, FieldInfo.accessCalloutHelper);

    IRNode *Destination;
    const bool IsLoad = true;
    IRNode *ValueToStore = nullptr;

    if (FieldInfo.helper == CORINFO_HELP_GETFIELDSTRUCT) {
      Destination = (IRNode *)createTemporary(FieldTy);
      setValueRepresentsStruct(Destination);
    } else {
      Destination = (IRNode *)Constant::getNullValue(FieldTy);
    }

    IRNode *Result =
        rdrCallFieldHelper(ResolvedToken, FieldInfo.helper, IsLoad, Destination,
                           Obj, ValueToStore, AlignmentPrefix, IsVolatile);

    return convertToStackType(Result, CorInfoType);
  }

  // The operand on top of the stack may be the address of the
  // valuetype, or it could be an instance of the valuetype. If
  // it's an instance, we need to get its address.
  //
  // Some C# compiler versions used ldfld on floats and doubles.
  // Tolerate this by getting the address instead of the value.
  Type *AddressTy = Obj->getType();

  if (AddressTy->isStructTy() || AddressTy->isFloatingPointTy()) {
    Obj = addressOfValue(Obj);
  }

  if (CorInfoType == CORINFO_TYPE_VALUECLASS ||
      CorInfoType == CORINFO_TYPE_REFANY) {
    AlignmentPrefix = getMinimumClassAlignment(Class, AlignmentPrefix);
  }

  // When using explicit null checks, have getFieldAddress insert a null check
  // on the base object and indicate that it is known not to be null later when
  // generating the load.
  // Otherwise, skip the null check here but indicate that the load may be
  // null so that it can (in theory) be annotated accordingly.
  bool NullCheckBeforeLoad = UseExplicitNullChecks;
  IRNode *Address =
      getFieldAddress(ResolvedToken, &FieldInfo, Obj, NullCheckBeforeLoad);

  return loadAtAddress(Address, FieldTy, CorInfoType, ResolvedToken,
                       AlignmentPrefix, IsVolatile, !NullCheckBeforeLoad);
}

// Generate instructions for loading value of the specified type at the
// specified address.
IRNode *GenIR::loadAtAddress(IRNode *Address, Type *Ty, CorInfoType CorType,
                             CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             ReaderAlignType AlignmentPrefix, bool IsVolatile,
                             bool AddressMayBeNull) {
  if (Ty->isStructTy()) {
    bool IsFieldAccess = ResolvedToken->hField != nullptr;
    return loadObj(ResolvedToken, Address, AlignmentPrefix, IsVolatile,
                   IsFieldAccess, AddressMayBeNull);
  } else {
    LoadInst *LoadInst = makeLoad(Address, IsVolatile, AddressMayBeNull);
    uint32_t Align = convertReaderAlignment(AlignmentPrefix);
    LoadInst->setAlignment(Align);

    IRNode *Result = convertToStackType((IRNode *)LoadInst, CorType);

    return Result;
  }
}

// Generate instructions for loading value of the specified type at the
// specified address.
IRNode *GenIR::loadAtAddress(IRNode *Address, Type *Ty, CorInfoType CorType,
                             ReaderAlignType AlignmentPrefix, bool IsVolatile,
                             bool AddressMayBeNull) {
  StructType *StructTy = dyn_cast<StructType>(Ty);
  if (StructTy != nullptr) {
    return loadNonPrimitiveObj(StructTy, Address, AlignmentPrefix, IsVolatile,
                               AddressMayBeNull);
  } else {
    LoadInst *LoadInst = makeLoad(Address, IsVolatile, AddressMayBeNull);
    uint32_t Align = convertReaderAlignment(AlignmentPrefix);
    LoadInst->setAlignment(Align);

    IRNode *Result = convertToStackType((IRNode *)LoadInst, CorType);

    return Result;
  }
}

// Generate instructions for storing value of the specified type at the
// specified address.
void GenIR::storeAtAddress(IRNode *Address, IRNode *ValueToStore, Type *Ty,
                           CORINFO_RESOLVED_TOKEN *ResolvedToken,
                           ReaderAlignType Alignment, bool IsVolatile,
                           bool IsField, bool AddressMayBeNull) {
  // We do things differently based on whether the field is a value class.
  if (Ty->isStructTy()) {
    storeObj(ResolvedToken, ValueToStore, Address, Alignment, IsVolatile,
             IsField, AddressMayBeNull);
  } else {
    StoreInst *StoreInst =
        makeStore(ValueToStore, Address, IsVolatile, AddressMayBeNull);
    uint32_t Align = convertReaderAlignment(Alignment);
    StoreInst->setAlignment(Align);
  }
}

void GenIR::storeAtAddressNoBarrierNonNull(IRNode *Address,
                                           IRNode *ValueToStore, llvm::Type *Ty,
                                           bool IsVolatile) {
  StructType *StructTy = dyn_cast<StructType>(Ty);
  if (StructTy != nullptr) {
    assert(ValueToStore->getType()->isPointerTy() &&
           (ValueToStore->getType()->getPointerElementType() == Ty));
    assert(doesValueRepresentStruct(ValueToStore));
    copyStructNoBarrier(StructTy, Address, ValueToStore, IsVolatile);
  } else {
    makeStoreNonNull(ValueToStore, Address, IsVolatile);
  }
}

void GenIR::storeField(CORINFO_RESOLVED_TOKEN *FieldToken, IRNode *ValueToStore,
                       IRNode *Object, ReaderAlignType Alignment,
                       bool IsVolatile) {
  // Gather information about the field
  const bool ObjectIsThis = objIsThis(Object);
  int32_t AccessFlags = ObjectIsThis ? CORINFO_ACCESS_THIS : CORINFO_ACCESS_ANY;
  AccessFlags |= CORINFO_ACCESS_SET;
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(FieldToken, (CORINFO_ACCESS_FLAGS)AccessFlags, &FieldInfo);
  CORINFO_FIELD_HANDLE FieldHandle = FieldToken->hField;

  // It's legal to use STFLD to store into a static field. In that case,
  // handle the opcode like STSFLD.
  if (FieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) {
    storeStaticField(FieldToken, ValueToStore, IsVolatile);
    return;
  }

  // Gather information about the type of the field.
  CorInfoType FieldCorType = FieldInfo.fieldType;
  CORINFO_CLASS_HANDLE FieldClassHandle = FieldInfo.structType;
  Type *FieldTy = getType(FieldCorType, FieldClassHandle);
  const bool IsStructTy = FieldTy->isStructTy();

  // Fields typed as GC pointers are always aligned,
  // so ignore any smaller alignment prefix
  if ((FieldCorType == CorInfoType::CORINFO_TYPE_CLASS) ||
      (FieldCorType == CorInfoType::CORINFO_TYPE_BYREF)) {
    Alignment = Reader_AlignNatural;
  }

  // Coerce the type of the value to store, if necessary.
  ValueToStore = convertFromStackType(ValueToStore, FieldCorType, FieldTy);

  // If the EE has asked us to use a helper to store the
  // value, then do so.
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INSTANCE_HELPER) {
    handleMemberAccess(FieldInfo.accessAllowed, FieldInfo.accessCalloutHelper);

    const bool IsLoad = false;
    IRNode *Destination = nullptr;

    rdrCallFieldHelper(FieldToken, FieldInfo.helper, IsLoad, Destination,
                       Object, ValueToStore, Alignment, IsVolatile);
    return;
  }

  // When using explicit null checks, have getFieldAddress insert a null check
  // on the base object and indicate that it is known not to be null later when
  // generating the store.
  // Otherwise, skip the null check here but indicate that the store may be
  // null so that it can (in theory) be annotated accordingly.
  bool NullCheckBeforeStore = UseExplicitNullChecks;
  IRNode *Address =
      getFieldAddress(FieldToken, &FieldInfo, Object, NullCheckBeforeStore);

  // Stores might require write barriers. If so, call the appropriate
  // helper method.
  const bool NeedsWriteBarrier =
      JitContext->JitInfo->isWriteBarrierHelperRequired(FieldHandle);
  if (NeedsWriteBarrier) {
    rdrCallWriteBarrierHelper(Address, ValueToStore, Alignment, IsVolatile,
                              FieldToken, !IsStructTy, false, true, false);
    return;
  }

  bool IsField = true;
  return storeAtAddress(Address, ValueToStore, FieldTy, FieldToken, Alignment,
                        IsVolatile, IsField, !NullCheckBeforeStore);
}

void GenIR::storePrimitiveType(IRNode *Value, IRNode *Addr,
                               CorInfoType CorInfoType,
                               ReaderAlignType Alignment, bool IsVolatile,
                               bool AddressMayBeNull) {
  ASSERTNR(isPrimitiveType(CorInfoType));

  uint32_t Align;
  const CORINFO_CLASS_HANDLE ClassHandle = nullptr;
  IRNode *TypedAddr =
      getTypedAddress(Addr, CorInfoType, ClassHandle, Alignment, &Align);
  Type *ExpectedTy =
      cast<PointerType>(TypedAddr->getType())->getPointerElementType();
  IRNode *ValueToStore = convertFromStackType(Value, CorInfoType, ExpectedTy);
  StoreInst *StoreInst =
      makeStore(ValueToStore, TypedAddr, IsVolatile, AddressMayBeNull);
  StoreInst->setAlignment(Align);
}

void GenIR::storeNonPrimitiveType(IRNode *Value, IRNode *Addr,
                                  CORINFO_CLASS_HANDLE Class,
                                  ReaderAlignType Alignment, bool IsVolatile,
                                  CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                  bool IsField) {
  // Get the minimum Alignment for the class
  Alignment = getMinimumClassAlignment(Class, Alignment);
  bool IsValueIsPointer = !Value->getType()->isVectorTy();
  assert(!IsValueIsPointer || doesValueRepresentStruct(Value));
  if (Value->getType()->isVectorTy()) {
    IsValueIsPointer = false;
  }
  rdrCallWriteBarrierHelper(Addr, Value, Alignment, IsVolatile, ResolvedToken,
                            false, IsValueIsPointer, IsField, false);
}

void GenIR::storeIndirectArg(const CallArgType &ValueArgType,
                             llvm::Value *ValueToStore, llvm::Value *Address,
                             bool IsVolatile) {
  assert(GcInfo::isGcPointer(cast<PointerType>(Address->getType())));

  Type *ValueToStoreType = ValueToStore->getType();
  if (ValueToStoreType->isVectorTy()) {
    assert(!ValueToStoreType->getVectorElementType()->isPointerTy());
    makeStoreNonNull(ValueToStore, Address, IsVolatile);
    return;
  }

  assert(ValueToStoreType->isPointerTy() &&
         ValueToStoreType->getPointerElementType()->isStructTy());
  assert(doesValueRepresentStruct(ValueToStore));

  CORINFO_CLASS_HANDLE ArgClass = ValueArgType.Class;

  // The argument may be on the heap; call the write barrier helper if
  // necessary.
  CORINFO_RESOLVED_TOKEN ResolvedToken;
  memset(&ResolvedToken, 0, sizeof(CORINFO_RESOLVED_TOKEN));
  ResolvedToken.hClass = ArgClass;

  const ReaderAlignType Alignment =
      getMinimumClassAlignment(ArgClass, Reader_AlignNatural);
  const bool IsNotValueClass = !JitContext->JitInfo->isValueClass(ArgClass);
  const bool IsValueIsPointer = true;
  const bool IsFieldToken = false;
  const bool IsUnchecked = false;
  rdrCallWriteBarrierHelper((IRNode *)Address, (IRNode *)ValueToStore,
                            Alignment, IsVolatile, &ResolvedToken,
                            IsNotValueClass, IsValueIsPointer, IsFieldToken,
                            IsUnchecked);
}

// Helper used to wrap CreateStore
StoreInst *GenIR::makeStore(Value *ValueToStore, Value *Address,
                            bool IsVolatile, bool AddressMayBeNull) {
  // TODO: There is a JitConfig setting JitLockWrite which can alter how
  // volatile stores are handled for x86 architectures. When this is set we
  // should emit a (lock) xchg intead of mov. RyuJit doesn't to look at this
  // config setting, so we also ignore it.
  if (AddressMayBeNull) {
    if (UseExplicitNullChecks) {
      Address = genNullCheck((IRNode *)Address);
    } else {
      // If we had support for implicit null checks, this
      // path would need to annotate the store we're about
      // to generate.
    }
  }
  if (ValueToStore->getType()->isVectorTy()) {
    return LLVMBuilder->CreateAlignedStore(ValueToStore, Address, 1,
                                           IsVolatile);
  }
  return LLVMBuilder->CreateStore(ValueToStore, Address, IsVolatile);
}

// Helper used to wrap CreateLoad
LoadInst *GenIR::makeLoad(Value *Address, bool IsVolatile,
                          bool AddressMayBeNull) {
  if (AddressMayBeNull) {
    if (UseExplicitNullChecks) {
      Address = genNullCheck((IRNode *)Address);
    } else {
      // If we had support for implicit null checks, this
      // path would need to annotate the load we're about
      // to generate.
    }
  }
  if (Address->getType()->isPointerTy() &&
      Address->getType()->getPointerElementType()->isVectorTy()) {
    return LLVMBuilder->CreateAlignedLoad(Address, 1, IsVolatile);
  }
  return LLVMBuilder->CreateLoad(Address, IsVolatile);
}

CallSite GenIR::makeCall(Value *Callee, bool MayThrow, ArrayRef<Value *> Args) {
  // First, figure out if we're inside a handler -- if so, we need a bundle
  // indicating which one.
  Value *ParentPad = nullptr;
  for (EHRegion *Region = CurrentRegion; Region != nullptr;
       Region = rgnGetEnclosingAncestor(Region)) {
    auto Kind = Region->Kind;
    if ((Kind == ReaderBaseNS::RegionKind::RGN_Fault) ||
        (Kind == ReaderBaseNS::RegionKind::RGN_Finally)) {
      ParentPad = Region->CleanupPad;
      break;
    }
    if ((Kind == ReaderBaseNS::RegionKind::RGN_MCatch) ||
        (Kind == ReaderBaseNS::RegionKind::RGN_MExcept)) {
      ParentPad = Region->CatchPad;
      break;
    }
    if (Kind == ReaderBaseNS::RegionKind::RGN_Filter) {
      ParentPad = Region->HandlerRegion->CatchPad;
      break;
    }
  }

  SmallVector<OperandBundleDef, 1> Bundles;
  if (ParentPad != nullptr) {
    Bundles.emplace_back("funclet", ParentPad);
  }

  if (MayThrow) {
    Instruction *EHPad =
        (CurrentRegion == nullptr ? nullptr : CurrentRegion->HandlerEHPad);
    if (EHPad == nullptr) {
      // The call may throw but is not in a protected region, so if it does
      // throw it will unwind all the way to this function's caller.  LLVM
      // models this behavior with the `call` operator, not `invoke`.  Fall
      // down to the code that generates a call.
    } else if (SuppressExceptionHandlers) {
      // Per configuration setting, suppress all exception edges so that
      // handlers will be removed as unreachable code.
    } else {
      BasicBlock *ExceptionSuccessor = EHPad->getParent();
      TerminatorInst *Goto;
      BasicBlock *NormalSuccessor = splitCurrentBlock(&Goto);
      InvokeInst *Invoke = InvokeInst::Create(
          Callee, NormalSuccessor, ExceptionSuccessor, Args, Bundles);
      replaceInstruction(Goto, Invoke);

      return Invoke;
    }
  }

  // Generate a simple call.
  return LLVMBuilder->CreateCall(Callee, Args, Bundles);
}

void GenIR::storeStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                             IRNode *ValueToStore, bool IsVolatile) {
  // Gather information about the field
  CORINFO_FIELD_HANDLE FieldHandle = FieldToken->hField;
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(FieldToken, CORINFO_ACCESS_SET, &FieldInfo);

  // Determine the type of the field element.
  CORINFO_CLASS_HANDLE FieldClassHandle;
  CorInfoType FieldCorType = getFieldType(FieldHandle, &FieldClassHandle);
  Type *FieldTy = getType(FieldCorType, FieldClassHandle);
  const bool IsStructTy = FieldTy->isStructTy();

  // Coerce the type of the value to store, if necessary.
  ValueToStore = convertFromStackType(ValueToStore, FieldCorType, FieldTy);

  // Get the address of the field.
  Value *DstAddress = rdrGetStaticFieldAddress(FieldToken, &FieldInfo);

  // If the runtime asks us to use a helper for the store, do so.
  const bool NeedsWriteBarrier =
      JitContext->JitInfo->isWriteBarrierHelperRequired(FieldHandle);
  if (NeedsWriteBarrier) {
    // Statics are always on the heap, so we can use an unchecked write barrier
    rdrCallWriteBarrierHelper((IRNode *)DstAddress, ValueToStore,
                              Reader_AlignNatural, IsVolatile, FieldToken,
                              !IsStructTy, false, true, true);
    return;
  }

  Type *PtrToFieldTy = getUnmanagedPointerType(FieldTy);
  if (DstAddress->getType()->isIntegerTy()) {
    DstAddress = LLVMBuilder->CreateIntToPtr(DstAddress, PtrToFieldTy);
  } else {
    ASSERT(DstAddress->getType()->isPointerTy());
    DstAddress = LLVMBuilder->CreatePointerCast(DstAddress, PtrToFieldTy);
  }

  // Create an assignment which stores the value into the static field.
  storeAtAddressNoBarrierNonNull((IRNode *)DstAddress, ValueToStore, FieldTy,
                                 IsVolatile);
}

IRNode *GenIR::loadStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                               bool IsVolatile) {
  // Gather information about the field.
  CORINFO_FIELD_HANDLE FieldHandle = FieldToken->hField;
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(FieldToken, CORINFO_ACCESS_GET, &FieldInfo);

  // Handle case where field is a constant zero.
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INTRINSIC_ZERO) {
    return loadConstantI(0);
  }

  // Handle case where field is a constant empty string
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INTRINSIC_EMPTY_STRING) {
    void *StringHandle;
    InfoAccessType Iat = JitContext->JitInfo->emptyStringLiteral(&StringHandle);
    return stringLiteral(mdTokenNil, StringHandle, Iat);
  }

  // Gather information about the field type.
  CORINFO_CLASS_HANDLE FieldClassHandle;
  CorInfoType FieldCorType = getFieldType(FieldHandle, &FieldClassHandle);
  Type *FieldTy = getType(FieldCorType, FieldClassHandle);

  // TODO: Replace static read-only fields with constant when possible

  // Get static field address. Convert to pointer.
  Value *Address = rdrGetStaticFieldAddress(FieldToken, &FieldInfo);
  Type *PtrToFieldTy = getUnmanagedPointerType(FieldTy);
  if (Address->getType()->isIntegerTy()) {
    Address = LLVMBuilder->CreateIntToPtr(Address, PtrToFieldTy);
  } else {
    ASSERT(Address->getType()->isPointerTy());
    Address = LLVMBuilder->CreatePointerCast(Address, PtrToFieldTy);
  }

  return loadAtAddressNonNull((IRNode *)Address, FieldTy, FieldCorType,
                              Reader_AlignNatural, IsVolatile);
}

IRNode *GenIR::addressOfValue(IRNode *Leaf) {
  Type *LeafTy = Leaf->getType();

  switch (LeafTy->getTypeID()) {
  case Type::TypeID::IntegerTyID:
  case Type::TypeID::FloatTyID:
  case Type::TypeID::DoubleTyID:
  case Type::TypeID::VectorTyID: {
    Instruction *Alloc = createTemporary(LeafTy);
    LLVMBuilder->CreateStore(Leaf, Alloc);
    return (IRNode *)Alloc;
  }

  case Type::TypeID::PointerTyID: {
    assert(doesValueRepresentStruct(Leaf));

    // Create a new pointer to the struct. The new pointer will point to the
    // same struct as the original pointer but doesValueRepresentStruct will
    // return false for the new pointer.
    Instruction *Alloc = createTemporary(LeafTy);
    LLVMBuilder->CreateStore(Leaf, Alloc);
    return (IRNode *)LLVMBuilder->CreateLoad(Alloc);
  }
  default:
    ASSERTNR(UNREACHED);
    return nullptr;
  }
}

IRNode *GenIR::makeBoxDstOperand(CORINFO_CLASS_HANDLE Class) {
  Type *Ty = getBoxedType(Class);
  Value *Ptr = llvm::Constant::getNullValue(getManagedPointerType(Ty));
  return (IRNode *)Ptr;
}

IRNode *GenIR::loadElem(ReaderBaseNS::LdElemOpcode Opcode,
                        CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Index,
                        IRNode *Array) {
  static const CorInfoType Map[ReaderBaseNS::LastLdelemOpcode] = {
      CorInfoType::CORINFO_TYPE_BYTE,      // LDELEM_I1
      CorInfoType::CORINFO_TYPE_UBYTE,     // LDELEM_U1
      CorInfoType::CORINFO_TYPE_SHORT,     // LDELEM_I2
      CorInfoType::CORINFO_TYPE_USHORT,    // LDELEM_U2
      CorInfoType::CORINFO_TYPE_INT,       // LDELEM_I4
      CorInfoType::CORINFO_TYPE_UINT,      // LDELEM_U4
      CorInfoType::CORINFO_TYPE_LONG,      // LDELEM_I8
      CorInfoType::CORINFO_TYPE_NATIVEINT, // LDELEM_I
      CorInfoType::CORINFO_TYPE_FLOAT,     // LDELEM_R4
      CorInfoType::CORINFO_TYPE_DOUBLE,    // LDELEM_R8
      CorInfoType::CORINFO_TYPE_CLASS,     // LDELEM_REF
      CorInfoType::CORINFO_TYPE_UNDEF      // LDELEM
  };

  ASSERTNR(Opcode >= ReaderBaseNS::LdelemI1 &&
           Opcode < ReaderBaseNS::LastLdelemOpcode);

  CorInfoType CorType = Map[Opcode];
  ReaderAlignType Alignment = Reader_AlignNatural;

  // ResolvedToken is only valid for ldelem.
  if (Opcode != ReaderBaseNS::Ldelem) {
    ResolvedToken = nullptr;
  }
  Type *ElementTy =
      getMSILArrayElementType(Array, ResolvedToken, &CorType, &Alignment);

  IRNode *ElementAddress = genArrayElemAddress(Array, Index, ElementTy);
  bool IsVolatile = false;
  return loadAtAddressNonNull(ElementAddress, ElementTy, CorType, ResolvedToken,
                              Alignment, IsVolatile);
}

IRNode *GenIR::loadElemA(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Index,
                         IRNode *Array, bool IsReadOnly) {
  ASSERTNR(ResolvedToken != nullptr);
  CORINFO_CLASS_HANDLE ClassHandle = ResolvedToken->hClass;
  uint32_t ClassAttribs = getClassAttribs(ClassHandle);
  CorInfoType CorInfoType = JitContext->JitInfo->asCorInfoType(ClassHandle);
  Type *ElementTy = getType(CorInfoType, ClassHandle);

  // Attempt to use a helper call.
  // We can only use the LDELEMA helper if the array elements
  // are not value classes and the access is not readonly.
  if (!IsReadOnly && ((ClassAttribs & CORINFO_FLG_VALUECLASS) == 0)) {
    IRNode *HandleNode = genericTokenToNode(ResolvedToken);
    PointerType *ElementAddressTy = getManagedPointerType(ElementTy);
    const bool MayThrow = true;
    return (IRNode *)callHelperImpl(CORINFO_HELP_LDELEMA_REF, MayThrow,
                                    ElementAddressTy, Array, Index, HandleNode)
        .getInstruction();
  }

  return genArrayElemAddress(Array, Index, ElementTy);
}

void GenIR::storeElem(ReaderBaseNS::StElemOpcode Opcode,
                      CORINFO_RESOLVED_TOKEN *ResolvedToken,
                      IRNode *ValueToStore, IRNode *Index, IRNode *Array) {
  static const CorInfoType Map[ReaderBaseNS::LastStelemOpcode] = {
      CorInfoType::CORINFO_TYPE_NATIVEINT, // STELEM_I
      CorInfoType::CORINFO_TYPE_BYTE,      // STELEM_I1
      CorInfoType::CORINFO_TYPE_SHORT,     // STELEM_I2
      CorInfoType::CORINFO_TYPE_INT,       // STELEM_I4
      CorInfoType::CORINFO_TYPE_LONG,      // STELEM_I8
      CorInfoType::CORINFO_TYPE_FLOAT,     // STELEM_R4
      CorInfoType::CORINFO_TYPE_DOUBLE,    // STELEM_R8
      CorInfoType::CORINFO_TYPE_CLASS,     // STELEM_REF
      CorInfoType::CORINFO_TYPE_UNDEF      // STELEM
  };

  ASSERTNR(Opcode >= ReaderBaseNS::StelemI &&
           Opcode < ReaderBaseNS::LastStelemOpcode);

  CorInfoType CorType = Map[Opcode];
  ReaderAlignType Alignment = Reader_AlignNatural;

  // ResolvedToken is only valid for stelem.
  if (Opcode != ReaderBaseNS::Stelem) {
    ResolvedToken = nullptr;
  }

  Type *ElementTy =
      getMSILArrayElementType(Array, ResolvedToken, &CorType, &Alignment);

  if (CorType == CorInfoType::CORINFO_TYPE_CLASS) {
    if (!isConstantNull(ValueToStore)) {
      // This will call a helper that stores an element of object array with
      // type checking. It will also call a write barrier if necessary. Storing
      // null is always legal, doesn't need a write barrier, and thus does not
      // need a helper call.
      return storeElemRefAny(ValueToStore, Index, Array);
    }
  }

  IRNode *ElementAddress = genArrayElemAddress(Array, Index, ElementTy);
  bool IsVolatile = false;
  bool IsField = false;
  ValueToStore = convertFromStackType(ValueToStore, CorType, ElementTy);
  if (ElementTy->isStructTy()) {
    bool IsNonValueClass = false;
    bool IsValueIsPointer = false;
    bool IsUnchecked = false;
    // Store with a write barrier if the struct has gc pointers.
    rdrCallWriteBarrierHelper(ElementAddress, ValueToStore, Alignment,
                              IsVolatile, ResolvedToken, IsNonValueClass,
                              IsValueIsPointer, IsField, IsUnchecked);
  } else {
    storeAtAddressNonNull(ElementAddress, ValueToStore, ElementTy,
                          ResolvedToken, Alignment, IsVolatile, IsField);
  }
}

// Get array element type from the token and/or CorType.
Type *GenIR::getMSILArrayElementType(IRNode *Array,
                                     CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                     CorInfoType *CorType,
                                     ReaderAlignType *Alignment) {
  ASSERTNR(Alignment != nullptr);
  ASSERTNR(CorType != nullptr);
  CORINFO_CLASS_HANDLE ClassHandle = nullptr;
  if (*CorType == CorInfoType::CORINFO_TYPE_CLASS) {
    // This is the ldelem.ref or stelem.ref case where the element
    // type is supposed to come from the input array rather
    // than being specified explicitly.
    PointerType *Ty = cast<PointerType>(Array->getType());
    if (!isArrayType(Ty, nullptr)) {
      // Likely System.__Canon*. Assume array of object.
      return this->getBuiltInObjectType();
    }
    StructType *ReferentTy = cast<StructType>(Ty->getPointerElementType());
    unsigned int NumElements = ReferentTy->getNumElements();
    ArrayType *ArrayTy =
        cast<ArrayType>(ReferentTy->getElementType(NumElements - 1));
    return ArrayTy->getElementType();
  }

  if (ResolvedToken != nullptr) {
    ClassHandle = ResolvedToken->hClass;
    *CorType = JitContext->JitInfo->asCorInfoType(ClassHandle);
    if ((*CorType == CorInfoType::CORINFO_TYPE_VALUECLASS) ||
        (*CorType == CORINFO_TYPE_REFANY)) {
      *Alignment = getMinimumClassAlignment(ClassHandle, Reader_AlignNatural);
    }
  }
  ASSERTNR(*CorType != CorInfoType::CORINFO_TYPE_UNDEF);
  return getType(*CorType, ClassHandle);
}

// Get address of the array element.
IRNode *GenIR::genArrayElemAddress(IRNode *Array, IRNode *Index,
                                   Type *ElementTy) {

  // Make sure array is properly typed for the element type,
  // otherwise the address arithmetic will be wrong.
  Array = this->ensureIsArray(Array, ElementTy);

  // This call will load the array length which will ensure that the array is
  // not null.
  IRNode *ArrayLength = loadLen(Array);

  genBoundsCheck(ArrayLength, Index);

  PointerType *Ty = cast<PointerType>(Array->getType());
  StructType *ReferentTy = cast<StructType>(Ty->getPointerElementType());
  unsigned int RawArrayStructFieldIndex = ReferentTy->getNumElements() - 1;

#ifndef NDEBUG
  Type *ArrayTy = ReferentTy->getElementType(RawArrayStructFieldIndex);
  assert(ArrayTy->isArrayTy());
  assert(ArrayTy->getArrayElementType() == ElementTy);
#endif

  LLVMContext &Context = *this->JitContext->LLVMContext;

  // Build up gep indices:
  // the first index is for the struct representing the array;
  // the second index is for the raw array (last field of the struct):
  // the third index is for the array element.
  Value *Indices[] = {
      ConstantInt::get(Type::getInt32Ty(Context), 0),
      ConstantInt::get(Type::getInt32Ty(Context), RawArrayStructFieldIndex),
      Index};

  Value *Address = LLVMBuilder->CreateInBoundsGEP(Array, Indices);

  return (IRNode *)Address;
}

bool GenIR::arrayGet(CORINFO_SIG_INFO *Sig, IRNode **RetVal) {
  uint32_t Rank = 0;
  CorInfoType ElemCorType = CORINFO_TYPE_UNDEF;
  Type *ElementTy = nullptr;
  const bool IsStore = false;
  const bool IsLoadAddr = false;

  if (!canExpandMDArrayRef(Sig, IsStore, IsLoadAddr, &Rank, &ElemCorType,
                           &ElementTy)) {
    return false;
  }

  // This call will null-check the array so the load below can assume a
  // non-null pointer.
  IRNode *ElementAddress = mdArrayRefAddr(Rank, ElementTy);

  // Load the value
  assert(!ElementTy->isStructTy());
  const bool IsVolatile = false;
  LoadInst *LoadInst = makeLoadNonNull(ElementAddress, IsVolatile);
  uint32_t Align = convertReaderAlignment(Reader_AlignNatural);
  LoadInst->setAlignment(Align);
  *RetVal = convertToStackType((IRNode *)LoadInst, ElemCorType);

  return true;
}

bool GenIR::arraySet(CORINFO_SIG_INFO *Sig) {
  uint32_t Rank = 0;
  CorInfoType ElemCorType = CORINFO_TYPE_UNDEF;
  Type *ElementTy = nullptr;
  const bool IsStore = true;
  const bool IsLoadAddr = false;

  if (!canExpandMDArrayRef(Sig, IsStore, IsLoadAddr, &Rank, &ElemCorType,
                           &ElementTy)) {
    return false;
  }

  IRNode *Value =
      convertFromStackType(ReaderOperandStack->pop(), ElemCorType, ElementTy);

  // This call will null-check the array so the store below can assume a
  // non-null pointer.
  IRNode *ElementAddress = mdArrayRefAddr(Rank, ElementTy);

  const bool IsVolatile = false;
  // Store the value
  if (GcInfo::isGcPointer(ElementTy)) {
    // Since arrays are always on the heap, writing a GC pointer into an array
    // always requires a write barrier.
    CORINFO_RESOLVED_TOKEN *const ResolvedToken = nullptr;
    const bool IsNonValueClass = true;
    const bool IsValueIsPointer = false;
    const bool IsFieldToken = false;
    const bool IsUnchecked = true;
    rdrCallWriteBarrierHelper(ElementAddress, Value, Reader_AlignNatural,
                              IsVolatile, ResolvedToken, IsNonValueClass,
                              IsValueIsPointer, IsFieldToken, IsUnchecked);
  } else {
    assert(!ElementTy->isStructTy());
    makeStoreNonNull(Value, ElementAddress, IsVolatile);
  }

  return true;
}

bool GenIR::arrayAddress(CORINFO_SIG_INFO *Sig, IRNode **RetVal) {
  uint32_t Rank = 0;
  CorInfoType ElemCorType = CORINFO_TYPE_UNDEF;
  Type *ElementTy = nullptr;
  const bool IsStore = false;
  const bool IsLoadAddr = true;

  if (!canExpandMDArrayRef(Sig, IsStore, IsLoadAddr, &Rank, &ElemCorType,
                           &ElementTy)) {
    return false;
  }

  *RetVal = mdArrayRefAddr(Rank, ElementTy);
  return true;
}

bool GenIR::canExpandMDArrayRef(CORINFO_SIG_INFO *Sig, bool IsStore,
                                bool IsLoadAddr, uint32_t *Rank,
                                CorInfoType *ElemCorType,
                                llvm::Type **ElemType) {
  // TODO: legacy jit limits the number of array intrinsics expanded to 50 to
  // avoid excessive code expansion. Evaluate whether we need such a limit.

  // If it's a store there is an argument for the value
  // (doesn't count toward the rank).
  *Rank = IsStore ? Sig->numArgs - 1 : Sig->numArgs;

  // Early out if we don't want to handle it.
  // NOTE: rank 1 requires special handling (not done here).
  if (*Rank > ArrayIntrinMaxRank || *Rank <= 1) {
    return false;
  }

  // Figure out the element type
  CorInfoType CorType = CORINFO_TYPE_UNDEF;
  CORINFO_CLASS_HANDLE Class = nullptr;
  if (IsStore) {
    CORINFO_ARG_LIST_HANDLE ArgList = Sig->args;
    CORINFO_CLASS_HANDLE ArgType;

    // Skip arguments for each dimension.
    for (uint32_t R = 0; R < *Rank; R++) {
      assert((strip(getArgType(Sig, ArgList, &ArgType)) ==
              CorInfoType::CORINFO_TYPE_INT) &&
             "expected MDArray indicies to be int32s");

      ArgList = getArgNext(ArgList);
    }

    CorType = strip(getArgType(Sig, ArgList, &ArgType));
    ASSERTNR(CorType != CORINFO_TYPE_VAR); // common generics trouble

    if (CorType == CORINFO_TYPE_CLASS || CorType == CORINFO_TYPE_VALUECLASS) {
      Class = getArgClass(Sig, ArgList);
    } else if (CorType == CORINFO_TYPE_REFANY) {
      Class = getBuiltinClass(CLASSID_TYPED_BYREF);
    } else {
      Class = nullptr;
    }
  } else {
    CorType = Sig->retType;
    Class = Sig->retTypeClass;
  }

  // Bail if the element type is a value class.
  if (CorType == CORINFO_TYPE_VALUECLASS) {
    return false;
  }

  if (IsStore || IsLoadAddr) {
    CORINFO_CLASS_HANDLE GCClass = Class;
    CorInfoType GCCorType = CorType;

    // If it is BYREF then we need to find the child type
    // for the next security check.
    if (CorType == CORINFO_TYPE_BYREF) {
      CORINFO_CLASS_HANDLE ClassTmp = nullptr;
      GCCorType = getChildType(Class, &ClassTmp);
      GCClass = ClassTmp;
    }

    // See if the element is an object.
    if (GCCorType == CORINFO_TYPE_STRING || GCCorType == CORINFO_TYPE_CLASS) {
      // If it's not final, we can't expand due to security reasons.
      uint32_t ClassAttribs = getClassAttribs(GCClass);
      if (!(ClassAttribs & CORINFO_FLG_FINAL)) {
        return false;
      }
    }
  }

  *ElemCorType = CorType;
  *ElemType = getType(CorType, Class);

  return true;
}

IRNode *GenIR::mdArrayRefAddr(uint32_t Rank, llvm::Type *ElemType) {
  IRNode *Indices[ArrayIntrinMaxRank];
  for (uint32_t I = Rank; I > 0; --I) {
    Indices[I - 1] = ReaderOperandStack->pop();
  }

  IRNode *Array = ReaderOperandStack->pop();

  // The memory layout of the array is as follows:
  //     PTR          MethodTable for ArrayType
  //     unsigned     length      Total number of elements in the array
  //     unsigned     alignpad  for 64 bit alignment
  //     unsigned     dimLengths[rank]
  //     unsigned     dimBounds[rank]
  //     Data elements follow this.
  //
  // Note that this layout is somewhat described in corinfo.h,
  // CORINFO_Array and CORINFO_RefArray.
  //
  // Valid indexes are
  //     dimBounds[i] <= index[i] < dimBounds[i] + dimLengths[i]

  assert(!ElemType->isStructTy());

  // Check whether the array is null. If UseExplicitNullChecks were false, we'd
  // need to annotate the first load as exception-bearing.
  if (UseExplicitNullChecks) {
    Array = genNullCheck(Array);
  }

  PointerType *Ty = cast<PointerType>(Array->getType());
  StructType *ReferentTy = cast<StructType>(Ty->getPointerElementType());
  unsigned ArrayDataIndex = ReferentTy->getNumElements() - 1;
  unsigned DimensionBoundsIndex = ArrayDataIndex - 1;

  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
  IRNode *ElementOffset = nullptr;
  const bool IsVolatile = false;
  uint32_t NaturalAlignment = convertReaderAlignment(Reader_AlignNatural);
  Type *Int32Ty = Type::getInt32Ty(LLVMContext);
  for (uint32_t I = 0; I < Rank; ++I) {
    IRNode *Index = convertFromStackType(
        Indices[I], CorInfoType::CORINFO_TYPE_INT, Int32Ty);

    // Load the lower bound
    Value *LowerBoundIndices[] = {
        ConstantInt::get(Int32Ty, 0),
        ConstantInt::get(Int32Ty, DimensionBoundsIndex),
        ConstantInt::get(Int32Ty, I)};
    Value *LowerBoundAddress =
        LLVMBuilder->CreateInBoundsGEP(Array, LowerBoundIndices);
    LoadInst *LowerBound = makeLoadNonNull(LowerBoundAddress, IsVolatile);
    LowerBound->setAlignment(NaturalAlignment);

    // Subtract the lower bound
    Index = (IRNode *)LLVMBuilder->CreateSub(Index, LowerBound);

    IRNode *DimensionLength =
        mdArrayGetDimensionLength(Array, ConstantInt::get(Int32Ty, I));

    // Do the range check
    genBoundsCheck(DimensionLength, Index);

    // We'll accumulate the address offset as we go
    if (I == 0) {
      // On the first dimension, we initialize the offset
      ElementOffset = Index;
    } else {
      // On subsequent dimensions, we compute:
      //     ElementOffset = ElementOffset*DimensionLength + Index
      Value *MulResult = LLVMBuilder->CreateMul(ElementOffset, DimensionLength);
      ElementOffset = (IRNode *)LLVMBuilder->CreateAdd(MulResult, Index);
    }
  }

  // Now we are ready to compute the address of the element.
  Value *ElementAddressIndices[] = {ConstantInt::get(Int32Ty, 0),
                                    ConstantInt::get(Int32Ty, ArrayDataIndex),
                                    ElementOffset};
  Value *ElementAddress =
      LLVMBuilder->CreateInBoundsGEP(Array, ElementAddressIndices);

  return (IRNode *)ElementAddress;
}

IRNode *GenIR::arrayGetDimLength(IRNode *Arg1, IRNode *Arg2,
                                 CORINFO_CALL_INFO *CallInfo) {

  IRNode *Dimension = Arg1;
  IRNode *Array = Arg2;

  const int SystemArrayStructNumElements = 1;
  const int VectorStructNumElements = 4;
  const int MDArrayStructNumElements = 6;
  StructType *ArrayStructType =
      cast<StructType>(Array->getType()->getPointerElementType());
  uint64_t ArrayRank = 0;
  unsigned int ArrayStructNumElements = ArrayStructType->getNumElements();
  Type *Int32Ty = Type::getInt32Ty(*JitContext->LLVMContext);

  switch (ArrayStructNumElements) {
  case SystemArrayStructNumElements: {
    // We have an opaque instance of System.Array. Just call the helper.
    return nullptr;
  }

  case VectorStructNumElements: {
    // We have a zero-based one-dimensional array.
    ArrayRank = 1;
    Constant *ArrayRankValue = ConstantInt::get(Int32Ty, ArrayRank);
    genBoundsCheck(ArrayRankValue, Dimension);

    // This call will load the array length which will ensure that the array is
    // not null.
    return loadLen(Array);
  }

  case MDArrayStructNumElements: {
    // We have a multi-dimensional array or a single-dimensional array with a
    // non-zero lower bound.

    // Check whether the array is null. If UseExplicitNullChecks were false,
    // we'd need to annotate the first load as exception-bearing.
    if (UseExplicitNullChecks) {
      Array = genNullCheck(Array);
    }

    const int DimensionLengthsIndex = 3;
    ArrayType *DimLengthsArrayType =
        cast<ArrayType>(ArrayStructType->getElementType(DimensionLengthsIndex));
    ArrayRank = DimLengthsArrayType->getArrayNumElements();
    Constant *ArrayRankValue = ConstantInt::get(Int32Ty, ArrayRank);
    genBoundsCheck(ArrayRankValue, Dimension);

    return mdArrayGetDimensionLength(Array, Dimension);
  }

  default:
    llvm_unreachable("Bad array type!");
  }
}

IRNode *GenIR::mdArrayGetDimensionLength(Value *Array, Value *Dimension) {
  Type *Int32Ty = Type::getInt32Ty(*JitContext->LLVMContext);
  const int DimensionLengthsIndex = 3;

  // Load the dimension length
  const bool IsVolatile = false;
  uint32_t NaturalAlignment = convertReaderAlignment(Reader_AlignNatural);
  Value *DimensionLengthIndices[] = {
      ConstantInt::get(Int32Ty, 0),
      ConstantInt::get(Int32Ty, DimensionLengthsIndex), Dimension};
  Value *DimensionLengthAddress =
      LLVMBuilder->CreateInBoundsGEP(Array, DimensionLengthIndices);
  LoadInst *DimensionLength =
      makeLoadNonNull(DimensionLengthAddress, IsVolatile);
  DimensionLength->setAlignment(NaturalAlignment);
  return (IRNode *)DimensionLength;
}

void GenIR::branch() {
  ASSERT(isa<BranchInst>(LLVMBuilder->GetInsertBlock()->getTerminator()));
}

IRNode *GenIR::call(ReaderBaseNS::CallOpcode Opcode, mdToken Token,
                    mdToken ConstraintTypeRef, mdToken LoadFtnToken,
                    bool HasReadOnlyPrefix, bool HasTailCallPrefix,
                    bool IsUnmarkedTailCall, uint32_t CurrOffset) {
  ReaderCallTargetData *Data =
      (ReaderCallTargetData *)_alloca(sizeof(ReaderCallTargetData));
  if (Opcode == ReaderBaseNS::NewObj) {
    makeReaderCallTargetDataForNewObj(Data, Token, LoadFtnToken);
  } else {
    ASSERTNR(LoadFtnToken == mdTokenNil);
    makeReaderCallTargetDataForCall(Data, Token, ConstraintTypeRef,
                                    HasTailCallPrefix, IsUnmarkedTailCall,
                                    HasReadOnlyPrefix, Opcode, CurrOffset);
  }
  IRNode *CallNode;
  return rdrCall(Data, Opcode, &CallNode);
}

bool isNonVolatileWriteHelperCall(CorInfoHelpFunc HelperId) {
  switch (HelperId) {
  case CORINFO_HELP_ASSIGN_REF:
  case CORINFO_HELP_CHECKED_ASSIGN_REF:
  case CORINFO_HELP_ASSIGN_STRUCT:
  case CORINFO_HELP_SETFIELD8:
  case CORINFO_HELP_SETFIELD16:
  case CORINFO_HELP_SETFIELD32:
  case CORINFO_HELP_SETFIELD64:
  case CORINFO_HELP_SETFIELDOBJ:
  case CORINFO_HELP_SETFIELDSTRUCT:
  case CORINFO_HELP_SETFIELDFLOAT:
  case CORINFO_HELP_SETFIELDDOUBLE:
  case CORINFO_HELP_MEMSET:
  case CORINFO_HELP_MEMCPY:
    return true;
  default:
    return false;
  }
}

// Generate call to helper
IRNode *GenIR::callHelper(CorInfoHelpFunc HelperID, bool MayThrow, IRNode *Dst,
                          IRNode *Arg1, IRNode *Arg2, IRNode *Arg3,
                          IRNode *Arg4, ReaderAlignType Alignment,
                          bool IsVolatile, bool NoCtor, bool CanMoveUp) {
  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
  Type *ReturnType =
      (Dst == nullptr) ? Type::getVoidTy(LLVMContext) : Dst->getType();
  return (IRNode *)callHelperImpl(HelperID, MayThrow, ReturnType, Arg1, Arg2,
                                  Arg3, Arg4, Alignment, IsVolatile, NoCtor,
                                  CanMoveUp)
      .getInstruction();
}

IRNode *GenIR::callHelper(CorInfoHelpFunc HelperID, IRNode *HelperAddress,
                          bool MayThrow, IRNode *Dst, IRNode *Arg1,
                          IRNode *Arg2, IRNode *Arg3, IRNode *Arg4,
                          ReaderAlignType Alignment, bool IsVolatile,
                          bool NoCtor, bool CanMoveUp) {
  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
  Type *ReturnType =
      (Dst == nullptr) ? Type::getVoidTy(LLVMContext) : Dst->getType();
  return (IRNode *)callHelperImpl(HelperID, HelperAddress, MayThrow, ReturnType,
                                  Arg1, Arg2, Arg3, Arg4, Alignment, IsVolatile,
                                  NoCtor, CanMoveUp)
      .getInstruction();
}

IRNode *GenIR::callReadyToRunHelper(CorInfoHelpFunc HelperID, bool MayThrow,
                                    IRNode *Dst,
                                    CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                    IRNode *Arg1, IRNode *Arg2, IRNode *Arg3,
                                    IRNode *Arg4, ReaderAlignType Alignment,
                                    bool IsVolatile, bool NoCtor,
                                    bool CanMoveUp) {
  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
  Type *ReturnType =
      (Dst == nullptr) ? Type::getVoidTy(LLVMContext) : Dst->getType();
  return (IRNode *)callReadyToRunHelperImpl(
             HelperID, MayThrow, ReturnType, ResolvedToken, Arg1, Arg2, Arg3,
             Arg4, Alignment, IsVolatile, NoCtor, CanMoveUp)
      .getInstruction();
}

CallSite GenIR::callHelperImpl(CorInfoHelpFunc HelperID, bool MayThrow,
                               Type *ReturnType, IRNode *Arg1, IRNode *Arg2,
                               IRNode *Arg3, IRNode *Arg4,
                               ReaderAlignType Alignment, bool IsVolatile,
                               bool NoCtor, bool CanMoveUp) {
  assert(HelperID != CORINFO_HELP_UNDEF);

  // TODO: We can turn some of these helper calls into intrinsics.
  // When doing so, make sure the intrinsics are not optimized
  // for the volatile operations.
  IRNode *Address = getHelperCallAddress(HelperID);

  return callHelperImpl(HelperID, Address, MayThrow, ReturnType, Arg1, Arg2,
                        Arg3, Arg4, Alignment, IsVolatile, NoCtor, CanMoveUp);
}

CallSite GenIR::callReadyToRunHelperImpl(
    CorInfoHelpFunc HelperID, bool MayThrow, Type *ReturnType,
    CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1, IRNode *Arg2,
    IRNode *Arg3, IRNode *Arg4, ReaderAlignType Alignment, bool IsVolatile,
    bool NoCtor, bool CanMoveUp) {
  assert(HelperID != CORINFO_HELP_UNDEF);

  IRNode *Address = getReadyToRunHelperCallAddress(HelperID, ResolvedToken);

  return callHelperImpl(HelperID, Address, MayThrow, ReturnType, Arg1, Arg2,
                        Arg3, Arg4, Alignment, IsVolatile, NoCtor, CanMoveUp);
}

CallSite GenIR::callHelperImpl(CorInfoHelpFunc HelperID, IRNode *Address,
                               bool MayThrow, Type *ReturnType, IRNode *Arg1,
                               IRNode *Arg2, IRNode *Arg3, IRNode *Arg4,
                               ReaderAlignType Alignment, bool IsVolatile,
                               bool NoCtor, bool CanMoveUp) {
  // We can't get the signature of the helper from the CLR so we generate
  // FunctionType based on the types of dst and the args passed to this method.
  Value *AllArguments[4];
  Type *AllArgumentTypes[4];
  size_t ArgumentCount = 0;

  if (Arg1) {
    AllArguments[0] = Arg1;
    AllArgumentTypes[0] = Arg1->getType();
    ++ArgumentCount;
    if (Arg2) {
      AllArguments[1] = Arg2;
      AllArgumentTypes[1] = Arg2->getType();
      ++ArgumentCount;
      if (Arg3) {
        AllArguments[2] = Arg3;
        AllArgumentTypes[2] = Arg3->getType();
        ++ArgumentCount;
        if (Arg4) {
          AllArguments[3] = Arg4;
          AllArgumentTypes[3] = Arg4->getType();
          ++ArgumentCount;
        }
      }
    }
  }

  ArrayRef<Value *> Arguments(AllArguments, ArgumentCount);
  ArrayRef<Type *> ArgumentTypes(AllArgumentTypes, ArgumentCount);

  bool IsVarArg = false;
  FunctionType *FunctionType =
      FunctionType::get(ReturnType, ArgumentTypes, IsVarArg);

  Value *Target = LLVMBuilder->CreateIntToPtr(
      Address, getUnmanagedPointerType(FunctionType));

  // This is an intermediate result. Callers must handle
  // transitioning to a valid stack type, if appropriate.
  CallSite Call = makeCall(Target, MayThrow, Arguments);

  if (IsVolatile && isNonVolatileWriteHelperCall(HelperID)) {
    // TODO: this is only needed where CLRConfig::INTERNAL_JitLockWrite is set
    // For now, conservatively we emit barrier regardless.
    memoryBarrier();
  }

  return Call;
}

IRNode *GenIR::getHelperCallAddress(CorInfoHelpFunc HelperId) {
  bool IsIndirect;
  void *Descriptor;

  // Get the address of the helper's function descriptor and build
  // the call target from it.
  Descriptor = getHelperDescr(HelperId, &IsIndirect);

  // TODO: direct PC-Rel calls for Jit helpers

  // TODO: figure out how much of imeta.cpp we need;
  // the token here is really an inlined call to
  // IMetaMakeJitHelperToken(helperId)
  return handleToIRNode((mdToken)(mdtJitHelper | HelperId), Descriptor, 0,
                        IsIndirect, IsIndirect, true, false);
}

IRNode *
GenIR::getReadyToRunHelperCallAddress(CorInfoHelpFunc HelperId,
                                      CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  bool IsIndirect;
  void *Descriptor;

  CORINFO_CONST_LOOKUP Lookup;
  // CORINFO_ACCESS_ATYPICAL_CALLSITE means that we can't guarantee
  // that we'll be able to generate call [rel32] form of the helper call so
  // crossgen shouldn't try to disassemble the call instruction.
  HelperId =
      (CorInfoHelpFunc)(HelperId | CORINFO_HELP_READYTORUN_ATYPICAL_CALLSITE);
  JitContext->JitInfo->getReadyToRunHelper(ResolvedToken, HelperId, &Lookup);
  Descriptor = Lookup.addr;
  assert(Lookup.accessType != InfoAccessType::IAT_PPVALUE);
  IsIndirect = (Lookup.accessType == InfoAccessType::IAT_PVALUE);

  // TODO: figure out how much of imeta.cpp we need;
  // the token here is really an inlined call to
  // IMetaMakeJitHelperToken(helperId)
  bool IsReadOnly = false;
  return handleToIRNode((mdToken)(mdtJitHelper | HelperId), Descriptor, 0,
                        IsIndirect, IsReadOnly, true, false);
}

// Generate special generics helper that might need to insert flow.
IRNode *GenIR::callRuntimeHandleHelper(CorInfoHelpFunc Helper, IRNode *Arg1,
                                       IRNode *Arg2, IRNode *NullCheckArg) {

  Type *ReturnType =
      Type::getIntNTy(*JitContext->LLVMContext, TargetPointerSizeInBits);

  // Call the helper unconditionally if NullCheckArg is null.
  if ((NullCheckArg == nullptr) || isConstantNull(NullCheckArg)) {
    const bool MayThrow = true;
    return (IRNode *)callHelperImpl(Helper, MayThrow, ReturnType, Arg1, Arg2)
        .getInstruction();
  }

  BasicBlock *SaveBlock = LLVMBuilder->GetInsertBlock();

  // Insert the compare against null.
  Value *Compare = LLVMBuilder->CreateIsNull(NullCheckArg, "NullCheck");

  // Generate conditional helper call.
  const bool MayThrow = true;
  const bool CallReturns = true;
  CallSite HelperCall =
      genConditionalHelperCall(Compare, Helper, MayThrow, ReturnType, Arg1,
                               Arg2, CallReturns, "RuntimeHandleHelperCall");

  // The result is a PHI of NullCheckArg and the generated call.
  // The generated code is equivalent to
  // x = NullCheckArg;
  // if (NullCheckArg == nullptr) {
  //   x = callhelper(Arg1, Arg2);
  // }
  // return x;
  BasicBlock *CurrentBlock = LLVMBuilder->GetInsertBlock();
  // Find the join predecessor that corresponds to the call.  It may not be
  // the block containing the call itself because the call may be an invoke
  // and so that block may have been split.  It will always be the predecessor
  // of the join which is not SavBlock.
  BasicBlock *RejoinBlock = nullptr;
  for (BasicBlock *JoinPred : predecessors(CurrentBlock)) {
    if (JoinPred != SaveBlock) {
      RejoinBlock = JoinPred;
      break;
    }
  }
  PHINode *Phi = mergeConditionalResults(CurrentBlock, NullCheckArg, SaveBlock,
                                         HelperCall.getInstruction(),
                                         RejoinBlock, "RuntimeHandle");
  return (IRNode *)Phi;
}

IRNode *GenIR::convertHandle(IRNode *GetTokenNumericNode,
                             CorInfoHelpFunc HelperID,
                             CORINFO_CLASS_HANDLE ClassHandle) {
  CorInfoType CorType = JitContext->JitInfo->asCorInfoType(ClassHandle);
  Type *ResultType = getType(CorType, ClassHandle);

  // We expect RuntimeTypeHandle, or RuntimeMethodHandle, or RuntimeFieldHandle,
  // each of which has a single field.
  assert(ResultType->getStructNumElements() == 1);

  // Create a temporary for the result struct.
  Value *Result = createTemporary(ResultType);
  Value *FieldAddress = LLVMBuilder->CreateStructGEP(nullptr, Result, 0);

  // Get the value that should be assigned to the struct's field, e.g., an
  // instance of RuntimeType.
  Type *HelperResultType = FieldAddress->getType()->getPointerElementType();
  const bool MayThrow = true;
  CallSite HelperResult =
      callHelperImpl(HelperID, MayThrow, HelperResultType, GetTokenNumericNode);

  // Assign the field of the result struct.
  LLVMBuilder->CreateStore(HelperResult.getInstruction(), FieldAddress);

  setValueRepresentsStruct(Result);

  return (IRNode *)Result;
}

IRNode *GenIR::getTypeFromHandle(IRNode *Arg1) {
  // We expect RuntimeTypeHandle that has a single field.
  assert(Arg1->getType()->getPointerElementType()->getStructNumElements() == 1);
  assert(doesValueRepresentStruct(Arg1));

  // Get the address of the struct's only field.
  Value *FieldAddress = LLVMBuilder->CreateStructGEP(nullptr, Arg1, 0);

  // Return the field's value (of type RuntimeType).
  const bool IsVolatile = false;
  return (IRNode *)LLVMBuilder->CreateLoad(FieldAddress, IsVolatile);
}

IRNode *GenIR::getValueFromRuntimeHandle(IRNode *Arg1) {
  // TODO: other JITs either
  // a) do not optimize this path, or
  // b) only optimize here if the incoming argument is the result of lowering
  //    a ldtoken instruction.
  //
  // We don't yet have the ability do detect (b) yet; stick with (a) in the
  // meantime.

  return nullptr;
}

CORINFO_CLASS_HANDLE GenIR::inferThisClass(IRNode *ThisArgument) {
  Type *Ty = ((Value *)ThisArgument)->getType();
  assert(Ty->isPointerTy());

  // Check for a ref class first.
  auto MapElem = ReverseClassTypeMap->find(Ty);
  if (MapElem != ReverseClassTypeMap->end()) {
    return MapElem->second;
  }

  // No hit, check for a value class.
  Ty = Ty->getPointerElementType();
  MapElem = ReverseClassTypeMap->find(Ty);
  if (MapElem != ReverseClassTypeMap->end()) {
    return MapElem->second;
  }

  return nullptr;
}

bool GenIR::canMakeDirectCall(ReaderCallTargetData *CallTargetData) {
  return !CallTargetData->isJmp();
}

GlobalVariable *GenIR::getGlobalVariable(uint64_t LookupHandle,
                                         uint64_t ValueHandle, Type *Ty,
                                         StringRef Name, bool IsConstant) {
  GlobalObject *Object = HandleToGlobalObjectMap[LookupHandle];
  GlobalVariable *GlobalVar = nullptr;
  if (Object != nullptr) {
    // Already have an object, make sure it's the right one.
    //
    // We'd like to assert that the name matches, but the same
    // LookupHandle may go by many names.
    assert(isa<GlobalVariable>(Object) && "expected variable");
    // Note after setting a global var V's type to Ty, V->getType() will
    // return ptr-to-Ty, while V->getValueType() will return Ty.
    assert(Object->getValueType() == Ty && "type mismatch for global");
    assert(NameToHandleMap->count(Object->getName()) == 1 &&
           "missing value for global");
    assert((*NameToHandleMap)[Object->getName()] == ValueHandle &&
           "value mismatch for global");
    GlobalVar = cast<GlobalVariable>(Object);
  } else {
    // Create a new global variable
    Constant *Initializer = nullptr;
    const bool IsExternallyInitialized = false;
    const GlobalValue::LinkageTypes LinkageType =
        GlobalValue::LinkageTypes::ExternalLinkage;
    GlobalVariable *const InsertBefore = nullptr;
    unsigned int AddressSpace = 0;

    GlobalVar = new GlobalVariable(*JitContext->CurrentModule, Ty, IsConstant,
                                   LinkageType, Initializer, Name, InsertBefore,
                                   GlobalValue::NotThreadLocal, AddressSpace,
                                   IsExternallyInitialized);

    // Set up or verify the value mapping.
    StringRef ResultName = GlobalVar->getName();
    if (NameToHandleMap->count(ResultName) == 1) {
      assert((*NameToHandleMap)[ResultName] == ValueHandle);
    } else {
      (*NameToHandleMap)[ResultName] = ValueHandle;
    }

    // Cache for future lookups.
    HandleToGlobalObjectMap[LookupHandle] = GlobalVar;
  }

  return GlobalVar;
}

Function *GenIR::getFunction(uint64_t LookupHandle, uint64_t ValueHandle,
                             FunctionType *Ty, StringRef Name) {
  GlobalObject *Object = HandleToGlobalObjectMap[LookupHandle];
  llvm::Function *F = nullptr;
  if (Object != nullptr) {
    // Already have an object, make sure it's the right one.
    //
    // We'd like to assert that the name matches, but the same
    // LookupHandle may go by many names.
    assert(isa<llvm::Function>(Object) && "expected function");
    assert(NameToHandleMap->count(Object->getName()) == 1 &&
           "missing value for function");
    assert((*NameToHandleMap)[Object->getName()] == ValueHandle &&
           "value mismatch for function");
    F = cast<llvm::Function>(Object);
  } else {
    // Create a new function.
    if (LookupHandle == (uint64_t)JitContext->MethodInfo->ftn) {
      // Self-reference....
      F = this->Function;
    } else {
      const GlobalValue::LinkageTypes LinkageType =
          GlobalValue::LinkageTypes::ExternalLinkage;
      F = Function::Create(Ty, LinkageType, Name, JitContext->CurrentModule);
    }

    // Set up or verify the value mapping.
    StringRef ResultName = F->getName();
    if (NameToHandleMap->count(ResultName) == 1) {
      assert((*NameToHandleMap)[ResultName] == ValueHandle);
    } else {
      (*NameToHandleMap)[ResultName] = ValueHandle;
    }

    // Cache for future lookups.
    HandleToGlobalObjectMap[LookupHandle] = F;
  }

  return F;
}

IRNode *GenIR::makeDirectCallTargetNode(CORINFO_METHOD_HANDLE MethodHandle,
                                        mdToken MethodToken, void *CodeAddr) {
  // Use bogus function type for now. We'll fix it up later on when we
  // know the actual signature.
  llvm::LLVMContext *LLVMContext = JitContext->LLVMContext;
  Type *VoidType = Type::getVoidTy(*LLVMContext);
  FunctionType *FuncTy = FunctionType::get(VoidType, false);
  const char *ModuleName = nullptr;
  const char *MethodName =
      JitContext->JitInfo->getMethodName(MethodHandle, &ModuleName);

  std::string FullName;
  raw_string_ostream OS(FullName);
  OS << format("%s.%s(TK_%x)", ModuleName, MethodName, MethodToken);
  OS.flush();
  llvm::Function *Func =
      getFunction((uint64_t)MethodHandle, (uint64_t)CodeAddr, FuncTy, FullName);

  return (IRNode *)Func;
}

// Helper callback used by rdrCall to emit a call to allocate a new MDArray.
IRNode *GenIR::genNewMDArrayCall(ReaderCallTargetData *CallTargetData,
                                 std::vector<IRNode *> Args,
                                 IRNode **CallNode) {
  // To construct the array we need to call a helper passing it the class handle
  // for the constructor method, the number of arguments to the constructor and
  // the arguments to the constructor.

  const ReaderCallSignature &Signature =
      CallTargetData->getCallTargetSignature();
  const std::vector<CallArgType> SigArgumentTypes =
      Signature.getArgumentTypes();
  const uint32_t ArgCount = Args.size();

  // Construct the new function type.
  Type *ReturnType =
      getType(SigArgumentTypes[0].CorType, SigArgumentTypes[0].Class);

  // The helper is variadic; we only want the types of the two fixed arguments
  // but need the values of all the arguments.
  const uint32_t FixedArgCount = 2;
  const uint32_t VariableArgCount = ArgCount - 1;
  const uint32_t TotalArgCount = FixedArgCount + VariableArgCount;
  Type *ArgumentTypes[FixedArgCount];
  SmallVector<Value *, 16> Arguments(TotalArgCount);
  uint32_t Index = 0;

  // The first argument is the class handle.
  IRNode *ClassHandle = CallTargetData->getClassHandleNode();
  ASSERTNR(ClassHandle);

  ArgumentTypes[Index] = ClassHandle->getType();
  Arguments[Index++] = ClassHandle;

  // The second argument is the number of arguments to follow.
  const uint32_t NumBits = 32;
  const bool IsSigned = true;
  Value *NumArgs = ConstantInt::get(
      *JitContext->LLVMContext,
      APInt(NumBits, CallTargetData->getSigInfo()->numArgs, IsSigned));
  ASSERTNR(NumArgs);

  ArgumentTypes[Index] = NumArgs->getType();
  Arguments[Index++] = NumArgs;

  // The rest of the arguments are the same as in the original newobj call.
  // It's a vararg call so add arguments but not argument types.
  for (unsigned I = 1; I < ArgCount; ++I) {
    IRNode *ArgNode = Args[I];
    CorInfoType CorType = SigArgumentTypes[I].CorType;
    CORINFO_CLASS_HANDLE Class = SigArgumentTypes[I].Class;
    Type *ArgType = this->getType(CorType, Class);

    if (ArgType->isStructTy()) {
      throw NotYetImplementedException("Call has value type args");
    }

    IRNode *Arg = convertFromStackType(ArgNode, CorType, ArgType);
    Arguments[Index++] = Arg;
  }

  const bool IsVarArg = true;
  FunctionType *FunctionType =
      FunctionType::get(ReturnType, ArgumentTypes, IsVarArg);

  // Create a call target with the right type.
  // Get the address of the Helper descr.
  Value *Callee = getHelperCallAddress(CORINFO_HELP_NEW_MDARR);
  Callee = LLVMBuilder->CreateIntToPtr(Callee,
                                       getUnmanagedPointerType(FunctionType));

  // Replace the old call instruction with the new one.
  const bool MayThrow = true;
  *CallNode = (IRNode *)makeCall(Callee, MayThrow, Arguments).getInstruction();
  return *CallNode;
}

IRNode *GenIR::genNewObjThisArg(ReaderCallTargetData *CallTargetData,
                                CorInfoType CorType,
                                CORINFO_CLASS_HANDLE Class) {
  Type *ThisType = this->getType(CorType, Class);

  uint32_t ClassAttribs = CallTargetData->getClassAttribs();
  bool IsVarObjSize = ((ClassAttribs & CORINFO_FLG_VAROBJSIZE) != 0);
  if (IsVarObjSize) {
    // Storage for variably-sized objects is allocated by the callee; simply
    // pass a null pointer.
    return (IRNode *)Constant::getNullValue(ThisType);
  }

  bool IsValueClass = ((ClassAttribs & CORINFO_FLG_VALUECLASS) != 0);
  if (IsValueClass) {
    CorInfoType StructCorType;
    uint32_t MbSize;
    ReaderBase::getClassType(Class, ClassAttribs, &StructCorType, &MbSize);

    Type *StructType = this->getType(StructCorType, Class);

    // We are allocating an instance of a value class on the stack.
    Instruction *AllocaInst = createTemporary(StructType);

    // Initialize the struct to zero.
    zeroInitBlock(AllocaInst, MbSize);

    // Create a managed pointer to the struct instance and pass it as the 'this'
    // argument to the constructor call.
    Type *ManagedPointerType = getManagedPointerType(StructType);
    Value *ManagedPointerToStruct =
        LLVMBuilder->CreateAddrSpaceCast(AllocaInst, ManagedPointerType);
    ManagedPointerToStruct =
        LLVMBuilder->CreatePointerCast(ManagedPointerToStruct, ThisType);

    return (IRNode *)ManagedPointerToStruct;
  }

  // We are allocating a fixed-size class on the heap.
  // Create a call to the newobj helper specific to this class,
  // and use its return value as the
  // 'this' pointer to be passed as the first argument to the constructor.

  // Create the address operand for the newobj helper.
  CallSite TheCallSite;
  const bool MayThrow = true;
  if (JitContext->Flags & CORJIT_FLG_READYTORUN) {
    TheCallSite =
        callReadyToRunHelperImpl(CORINFO_HELP_READYTORUN_NEW, MayThrow,
                                 ThisType, CallTargetData->getResolvedToken());
  } else {
    IRNode *ClassHandleNode = CallTargetData->getClassHandleNode();
    CorInfoHelpFunc HelperId = getNewHelper(CallTargetData->getResolvedToken());
    TheCallSite = callHelperImpl(HelperId, MayThrow, ThisType, ClassHandleNode);
  }
  Value *ThisPointer = TheCallSite.getInstruction();
  return (IRNode *)ThisPointer;
}

IRNode *GenIR::genNewObjReturnNode(ReaderCallTargetData *CallTargetData,
                                   IRNode *ThisArg) {
  uint32_t ClassAttribs = CallTargetData->getClassAttribs();
  bool IsValueClass = ((ClassAttribs & CORINFO_FLG_VALUECLASS) != 0);

  if (IsValueClass) {
    // Dig through the 'this' arg to find the temporary created earlier
    // and dereference it.
    Value *Alloca = nullptr;
    CastInst *PointerCast = cast<CastInst>(ThisArg);
    AddrSpaceCastInst *AddrSpaceCast =
        dyn_cast<AddrSpaceCastInst>(PointerCast->getOperand(0));
    if (AddrSpaceCast == nullptr) {
      Alloca = PointerCast->getOperand(0);
    } else {
      Alloca = AddrSpaceCast->getOperand(0);
    }
    AllocaInst *Temp = cast<AllocaInst>(Alloca);
    if (Temp->getType()->getPointerElementType()->isStructTy()) {
      setValueRepresentsStruct(Temp);
      return (IRNode *)Temp;
    } else {
      // The temp we passed as the this arg needs to be dereferenced.
      return (IRNode *)makeLoadNonNull((Value *)Temp, false);
    }
  }

  // Otherwise, we already have a good value.
  return ThisArg;
}

IRNode *GenIR::genCall(ReaderCallTargetData *CallTargetInfo, bool MayThrow,
                       std::vector<IRNode *> Args, IRNode **CallNode) {
  IRNode *Call = nullptr;
  IRNode *TargetNode = CallTargetInfo->getCallTargetNode();
  if (!isa<llvm::Function>(TargetNode) &&
      TargetNode->getType()->isPointerTy()) {
    // According to the ECMA CLI standard, II.14.5, the preferred
    // representation of method pointers is a native int
    // which is an int the size of a pointer.
    // However the type returned by the CoreClr for a method pointer
    // is a pointer to a struct named "(fnptr)". So convert it
    // to native int.
    LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
    IntegerType *NativeInt =
        Type::getIntNTy(LLVMContext, TargetPointerSizeInBits);
    TargetNode = (IRNode *)LLVMBuilder->CreatePtrToInt(TargetNode, NativeInt);
  }
  const ReaderCallSignature &Signature =
      CallTargetInfo->getCallTargetSignature();

  CorInfoCallConv CC = Signature.getCallingConvention();
  if (CC == CORINFO_CALLCONV_VARARG) {
    throw NotYetImplementedException("Vararg call");
  }

  bool IsUnmanagedCall = CC != CORINFO_CALLCONV_DEFAULT;
  if (!CallTargetInfo->isIndirect() && IsUnmanagedCall) {
    throw NotYetImplementedException("Direct unmanaged call");
  }

  CallArgType ResultType = Signature.getResultType();
  const std::vector<CallArgType> &ArgumentTypes = Signature.getArgumentTypes();
  const uint32_t NumArgs = Args.size();
  assert(NumArgs == ArgumentTypes.size());

  bool IsJmp = CallTargetInfo->isJmp();
  SmallVector<Value *, 16> Arguments(NumArgs);
  for (uint32_t I = 0; I < NumArgs; I++) {
    IRNode *ArgNode = Args[I];
    CorInfoType CorType = ArgumentTypes[I].CorType;
    CORINFO_CLASS_HANDLE Class = ArgumentTypes[I].Class;
    Type *ArgType = this->getType(CorType, Class);

    if (I == 0) {
      if (!CallTargetInfo->isNewObj() && CallTargetInfo->needsNullCheck()) {
        // Insert this Ptr null check if required
        ASSERT(CallTargetInfo->hasThis());
        ArgNode = genNullCheck(ArgNode);
      }
    }
    // We pass indirect args to jmp as the pointers we get from the caller
    // without copying the parameters into the current frame.
    IRNode *Arg = IsJmp && (ABIMethodSig.getArgumentInfo(I).getKind() ==
                            ABIArgInfo::Indirect)
                      ? ArgNode
                      : convertFromStackType(ArgNode, CorType, ArgType);
    Arguments[I] = Arg;
  }

  CorInfoIntrinsics IntrinsicID = CallTargetInfo->getCorInstrinsic();
  if ((0 <= IntrinsicID) && (IntrinsicID < CORINFO_INTRINSIC_Count)) {
    switch (IntrinsicID) {
    // TODO: note that these methods have well-known semantics that the jit can
    // use to optimize in some cases.
    // For now treat these intrinsics as normal calls.
    case CORINFO_INTRINSIC_RTH_GetValueInternal:
    case CORINFO_INTRINSIC_Object_GetType:
    case CORINFO_INTRINSIC_TypeEQ:
    case CORINFO_INTRINSIC_TypeNEQ:
    case CORINFO_INTRINSIC_GetCurrentManagedThread:
    case CORINFO_INTRINSIC_GetManagedThreadId: {
      break;
    }
    default:
      break;
    }
  }

  ABICallSignature ABICallSig(Signature, *this, *JitContext->TheABIInfo);
  Value *ResultNode =
      ABICallSig.emitCall(*this, (Value *)TargetNode, MayThrow, Arguments,
                          (Value *)CallTargetInfo->getIndirectionCellNode(),
                          IsJmp, (Value **)&Call);

  // Add VarArgs cookie to outgoing param list
  if (CC == CORINFO_CALLCONV_VARARG) {
    canonVarargsCall(Call, CallTargetInfo);
  }

  // If this call is eligible for tail calls, mark it now.
  if (!IsJmp) {
    if (CallTargetInfo->isTailCall()) {
      if (isa<CallInst>(Call)) {
        CallInst *C = cast<CallInst>(Call);
        bool canTailCall =
            tailCallChecks(CallTargetInfo->getMethodHandle(),
                           CallTargetInfo->getKnownMethodHandle(),
                           CallTargetInfo->isUnmarkedTailCall(),
                           ABICallSig.hasIndirectResultOrArg());
        C->setTailCall(canTailCall);
      }
    }
  }

  *CallNode = Call;

  if (ResultType.CorType != CORINFO_TYPE_VOID) {
    if (IsJmp) {
      // The IR for jmp is a musttail call immediately followed by a ret.
      // The return types of the caller and the jmp target are guaranteed to
      // match so we shouldn't convert to stack type and then back to return
      // type.
      return (IRNode *)ResultNode;
    } else {
      return convertToStackType((IRNode *)ResultNode, ResultType.CorType);
    }
  } else {
    return nullptr;
  }
}

IRNode *GenIR::convertToBoxHelperArgumentType(IRNode *Opr, uint32_t DestSize) {
  Type *Ty = Opr->getType();
  switch (Ty->getTypeID()) {
  case Type::TypeID::IntegerTyID: {
    // CLR box helper will only accept 4 byte or 8 byte integers. The value on
    // the stack should already have the right size.
    ASSERT((Ty->getIntegerBitWidth() == 32) ||
           (Ty->getIntegerBitWidth() == 64));

    // If the operand size is smaller than DestSize the boxing helper will grab
    // data from outside the smaller operand.
    ASSERT(DestSize <= Ty->getIntegerBitWidth());
    break;
  }
  // If the data type is a float64 and we want to box it to a
  // float32 then also have to do an explict conversion.
  // Otherwise, the boxing helper will grab the wrong bits out of the float64
  // and
  // destroy the value.
  case Type::TypeID::FloatTyID:
  case Type::TypeID::DoubleTyID:
    if (Ty->getPrimitiveSizeInBits() > DestSize) {
      Opr = (IRNode *)LLVMBuilder->CreateFPCast(Opr, Ty);
    }
    break;
  default:
    break;
  }

  return Opr;
}

// Method is called with empty stack.
void GenIR::jmp(ReaderBaseNS::CallOpcode Opcode, mdToken Token) {
  assert(Opcode == ReaderBaseNS::Jmp);
  ReaderCallTargetData Data;
  makeReaderCallTargetDataForJmp(&Data, Token);

  const bool IsJmp = true;
  if (MethodSignature.hasThis()) {
    IRNode *ThisArg = loadArg(MethodSignature.getThisIndex(), IsJmp);
    ReaderOperandStack->push(ThisArg);
  }

  for (uint32_t I = MethodSignature.getNormalParamStart();
       I < MethodSignature.getNormalParamEnd(); ++I) {
    IRNode *NormalArg = nullptr;
    const ABIArgInfo &ArgInfo = ABIMethodSig.getArgumentInfo(I);
    if (ArgInfo.getKind() == ABIArgInfo::Indirect) {
      // We pass indirect arguments without copying them to the current frame.
      NormalArg = (IRNode *)Arguments[I];
    } else {
      NormalArg = loadArg(I, IsJmp);
    }
    ReaderOperandStack->push(NormalArg);
  }

  // This will generate a call marked with musttail.
  IRNode *CallNode = nullptr;
  rdrCall(&Data, Opcode, &CallNode);

#ifndef NDEBUG
  const bool IsSynchronizedMethod =
      ((getCurrentMethodAttribs() & CORINFO_FLG_SYNCH) != 0);
  assert(!IsSynchronizedMethod);
#endif

  // LLVM requires musttail calls to be immediatley followed by a ret.
  if (Function->getReturnType()->isVoidTy()) {
    LLVMBuilder->CreateRetVoid();
  } else {
    LLVMBuilder->CreateRet(CallNode);
  }

  return;
}

Value *GenIR::genConvertOverflowCheck(Value *Source, IntegerType *TargetTy,
                                      bool &SourceIsSigned, bool DestIsSigned) {
  Type *SourceTy = Source->getType();
  unsigned TargetBitWidth = TargetTy->getBitWidth();

  if (SourceTy->isFloatingPointTy()) {
    // Overflow-checking conversions from floating-point call runtime helpers.
    // Find the appropriate helper.  Available helpers convert from double to
    // either signed or unsigned int32 or int64.
    CorInfoHelpFunc Helper;
    Type *HelperResultTy;

    if (TargetBitWidth == 64) {
      // Convert to 64-bit result
      HelperResultTy = Type::getInt64Ty(*JitContext->LLVMContext);
      if (DestIsSigned) {
        Helper = CORINFO_HELP_DBL2LNG_OVF;
      } else {
        Helper = CORINFO_HELP_DBL2ULNG_OVF;
      }
    } else {
      // All other cases, use the helper that converts to 32-bit int.  If the
      // target type is less than 32 bits, the helper call will be followed
      // by an explicit checked integer narrowing.
      HelperResultTy = Type::getInt32Ty(*JitContext->LLVMContext);
      if (DestIsSigned) {
        Helper = CORINFO_HELP_DBL2INT_OVF;
      } else {
        Helper = CORINFO_HELP_DBL2UINT_OVF;
      }
    }

    if (SourceTy->isFloatTy()) {
      // The helper takes a double, so up-convert the float before invoking it.
      Type *DoubleTy = Type::getDoubleTy(*JitContext->LLVMContext);
      Source = LLVMBuilder->CreateFPCast(Source, DoubleTy);
      SourceTy = DoubleTy;
    } else {
      assert(SourceTy->isDoubleTy() && "unexpected floating-point type");
    }

    // Call the helper to convert to int.
    const bool MayThrow = true;
    Source = callHelperImpl(Helper, MayThrow, HelperResultTy, (IRNode *)Source)
                 .getInstruction();
    SourceTy = HelperResultTy;

    // The result of the helper call already has the requested signedness.
    SourceIsSigned = DestIsSigned;

    // It's possible that the integer result of the helper call still needs
    // truncation with overflow checking (converting e.g. from double to i8
    // uses a double->i32 helper followed by i32->i8 truncation), so continue
    // on to the integer conversion handling.
  } else if (SourceTy->isPointerTy()) {
    // Re-interpret pointers as native ints.
    Type *NativeIntTy = getType(CorInfoType::CORINFO_TYPE_NATIVEINT, nullptr);
    Source = LLVMBuilder->CreatePtrToInt(Source, NativeIntTy);
    SourceTy = NativeIntTy;
  }

  assert(Source->getType() == SourceTy);
  assert(SourceTy->isIntegerTy());

  unsigned SourceBitWidth = cast<IntegerType>(SourceTy)->getBitWidth();

  if (TargetBitWidth > SourceBitWidth) {
    // Widening integer conversion.
    if (SourceIsSigned && !DestIsSigned) {
      // Signed -> Unsigned widening conversion overflows iff source is
      // negative.

      Constant *Zero = Constant::getNullValue(SourceTy);
      Value *Ovf = LLVMBuilder->CreateICmpSLT(Source, Zero, "Ovf");
      genConditionalThrow(Ovf, CORINFO_HELP_OVERFLOW, "ThrowOverflow");
    } else {
      // Signed -> Signed widening conversion never overflows
      // Unsigned -> Unsigned widening conversion never overflows
      // Unsigned -> Signed widening conversion never overflows
    }
  } else if (TargetBitWidth == SourceBitWidth) {
    // Same-size integer conversion
    if (SourceIsSigned != DestIsSigned) {
      // Signed -> Unsigned same-size conversion overflows iff source is
      // negative.
      // Unsigned -> Signed same-size conversion overflows iff source is
      // signed-less-than zero.
      // The code for these cases is identical.

      Constant *Zero = Constant::getNullValue(SourceTy);
      Value *Ovf = LLVMBuilder->CreateICmpSLT(Source, Zero, "Ovf");
      genConditionalThrow(Ovf, CORINFO_HELP_OVERFLOW, "ThrowOverflow");
    } else {
      // Identity conversion never overflows
    }
  } else {
    // Narrowing integer conversion
    if (DestIsSigned) {
      if (SourceIsSigned) {
        // Signed -> Signed narrowing conversion overflows iff
        //   (source is less than sext(signedMinValue(TargetBitWidth)) or
        //    source is greater than ext(signedMaxValue(TargetBitWidth)))
        // Use two comparisons (rather than a subtract and single compare) to
        // make the IR more straightforward for downstream analysis (range
        // propagation won't have to rewind through the subtract).  With throw
        // block sharing, subsequent optimization should be able to rewrite
        // the two-compare sequence to subtract-compare.

        APInt MinSmallInt =
            APInt::getSignedMinValue(TargetBitWidth).sext(SourceBitWidth);
        ConstantInt *MinConstant =
            ConstantInt::get(*JitContext->LLVMContext, MinSmallInt);
        Value *TooSmall =
            LLVMBuilder->CreateICmpSLT(Source, MinConstant, "Ovf");

        genConditionalThrow(TooSmall, CORINFO_HELP_OVERFLOW, "ThrowOverflow");

        APInt MaxSmallInt =
            APInt::getSignedMaxValue(TargetBitWidth).sext(SourceBitWidth);
        ConstantInt *MaxConstant =
            ConstantInt::get(*JitContext->LLVMContext, MaxSmallInt);
        Value *TooBig = LLVMBuilder->CreateICmpSGT(Source, MaxConstant, "Ovf");

        genConditionalThrow(TooBig, CORINFO_HELP_OVERFLOW, "ThrowOverflow");
      } else {
        // Unsigned -> Signed narrowing conversion overflows iff source is
        // unsigned-greater-than zext(signedMaxValue(TargetBitWidth))
        // This catches cases where truncation would discard set bits and cases
        // where truncation would produce a negative number when interpreted
        // as signed.

        APInt MaxSmallInt =
            APInt::getSignedMaxValue(TargetBitWidth).zext(SourceBitWidth);
        ConstantInt *MaxConstant =
            ConstantInt::get(*JitContext->LLVMContext, MaxSmallInt);
        Value *Ovf = LLVMBuilder->CreateICmpUGT(Source, MaxConstant, "Ovf");

        genConditionalThrow(Ovf, CORINFO_HELP_OVERFLOW, "ThrowOverflow");
      }
    } else {
      // Signed -> Unsigned narrowing conversion or
      // Unsigned -> Unsigned narrowing conversion: both overflow iff source
      // is unsigned-greater-than zext(unsignedMaxValue(TargetBitWidth))

      APInt MaxSmallInt =
          APInt::getMaxValue(TargetBitWidth).zext(SourceBitWidth);
      ConstantInt *MaxConstant =
          ConstantInt::get(*JitContext->LLVMContext, MaxSmallInt);
      Value *Ovf = LLVMBuilder->CreateICmpUGT(Source, MaxConstant, "Ovf");

      genConditionalThrow(Ovf, CORINFO_HELP_OVERFLOW, "ThrowOverflow");
    }
  }

  return Source;
}

IRNode *GenIR::conv(ReaderBaseNS::ConvOpcode Opcode, IRNode *Source) {

  struct ConvertInfo {
    CorInfoType CorType;
    bool CheckForOverflow;
    bool SourceIsUnsigned;
  };

  static const ConvertInfo Map[ReaderBaseNS::LastConvOpcode] = {
      {CorInfoType::CORINFO_TYPE_BYTE, false, false},       // CONV_I1
      {CorInfoType::CORINFO_TYPE_SHORT, false, false},      // CONV_I2
      {CorInfoType::CORINFO_TYPE_INT, false, false},        // CONV_I4
      {CorInfoType::CORINFO_TYPE_LONG, false, false},       // CONV_I8
      {CorInfoType::CORINFO_TYPE_FLOAT, false, false},      // CONV_R4
      {CorInfoType::CORINFO_TYPE_DOUBLE, false, false},     // CONV_R8
      {CorInfoType::CORINFO_TYPE_UBYTE, false, false},      // CONV_U1
      {CorInfoType::CORINFO_TYPE_USHORT, false, false},     // CONV_U2
      {CorInfoType::CORINFO_TYPE_UINT, false, false},       // CONV_U4
      {CorInfoType::CORINFO_TYPE_ULONG, false, false},      // CONV_U8
      {CorInfoType::CORINFO_TYPE_NATIVEINT, false, false},  // CONV_I
      {CorInfoType::CORINFO_TYPE_NATIVEUINT, false, false}, // CONV_U
      {CorInfoType::CORINFO_TYPE_BYTE, true, false},        // CONV_OVF_I1
      {CorInfoType::CORINFO_TYPE_SHORT, true, false},       // CONV_OVF_I2
      {CorInfoType::CORINFO_TYPE_INT, true, false},         // CONV_OVF_I4
      {CorInfoType::CORINFO_TYPE_LONG, true, false},        // CONV_OVF_I8
      {CorInfoType::CORINFO_TYPE_UBYTE, true, false},       // CONV_OVF_U1
      {CorInfoType::CORINFO_TYPE_USHORT, true, false},      // CONV_OVF_U2
      {CorInfoType::CORINFO_TYPE_UINT, true, false},        // CONV_OVF_U4
      {CorInfoType::CORINFO_TYPE_ULONG, true, false},       // CONV_OVF_U8
      {CorInfoType::CORINFO_TYPE_NATIVEINT, true, false},   // CONV_OVF_I
      {CorInfoType::CORINFO_TYPE_NATIVEUINT, true, false},  // CONV_OVF_U
      {CorInfoType::CORINFO_TYPE_BYTE, true, true},         // CONV_OVF_I1_UN
      {CorInfoType::CORINFO_TYPE_SHORT, true, true},        // CONV_OVF_I2_UN
      {CorInfoType::CORINFO_TYPE_INT, true, true},          // CONV_OVF_I4_UN
      {CorInfoType::CORINFO_TYPE_LONG, true, true},         // CONV_OVF_I8_UN
      {CorInfoType::CORINFO_TYPE_UBYTE, true, true},        // CONV_OVF_U1_UN
      {CorInfoType::CORINFO_TYPE_USHORT, true, true},       // CONV_OVF_U2_UN
      {CorInfoType::CORINFO_TYPE_UINT, true, true},         // CONV_OVF_U4_UN
      {CorInfoType::CORINFO_TYPE_ULONG, true, true},        // CONV_OVF_U8_UN
      {CorInfoType::CORINFO_TYPE_NATIVEINT, true, true},    // CONV_OVF_I_UN
      {CorInfoType::CORINFO_TYPE_NATIVEUINT, true, true},   // CONV_OVF_U_UN
      {CorInfoType::CORINFO_TYPE_DOUBLE, false, true}       // CONV_R_UN
  };

  ConvertInfo Info = Map[Opcode];

  Type *TargetTy = getType(Info.CorType, nullptr);
  bool SourceIsSigned = !Info.SourceIsUnsigned;
  const bool DestIsSigned = TargetTy->isIntegerTy() && isSigned(Info.CorType);
  Value *Conversion = nullptr;

  if (Info.CheckForOverflow) {
    assert(TargetTy->isIntegerTy() &&
           "No conv.ovf forms target floating-point");

    Source = (IRNode *)genConvertOverflowCheck(
        Source, cast<IntegerType>(TargetTy), SourceIsSigned, DestIsSigned);
  }

  Type *SourceTy = Source->getType();

  if (SourceTy == TargetTy) {
    Conversion = Source;
  } else if (SourceTy->isIntegerTy() && TargetTy->isIntegerTy()) {
    Conversion = LLVMBuilder->CreateIntCast(Source, TargetTy, DestIsSigned);
  } else if (SourceTy->isPointerTy() && TargetTy->isIntegerTy()) {
    Conversion = LLVMBuilder->CreatePtrToInt(Source, TargetTy);
  } else if (SourceTy->isIntegerTy() && TargetTy->isFloatingPointTy()) {
    Conversion = SourceIsSigned ? LLVMBuilder->CreateSIToFP(Source, TargetTy)
                                : LLVMBuilder->CreateUIToFP(Source, TargetTy);
  } else if (SourceTy->isFloatingPointTy() && TargetTy->isIntegerTy()) {
    // The ECMA-335 spec says that for unchecked floating point to
    // integer conversion, if there is overflow then the result
    // is unspecified. However for compatibility with other jits
    // we want to first convert to i64 and then truncate.
    IntegerType *TargetIntegerType = cast<IntegerType>(TargetTy);
    int TargetBitWidth = TargetIntegerType->getBitWidth();
    if (TargetBitWidth < 64) {
      IntegerType *Int64Ty = Type::getInt64Ty(*JitContext->LLVMContext);
      Conversion = DestIsSigned ? LLVMBuilder->CreateFPToSI(Source, Int64Ty)
                                : LLVMBuilder->CreateFPToUI(Source, Int64Ty);
      // Now do the final conversion to the target type.
      Conversion =
          LLVMBuilder->CreateIntCast(Conversion, TargetTy, DestIsSigned);
    } else {
      Conversion = DestIsSigned ? LLVMBuilder->CreateFPToSI(Source, TargetTy)
                                : LLVMBuilder->CreateFPToUI(Source, TargetTy);
    }
  } else if (SourceTy->isFloatingPointTy() && TargetTy->isFloatingPointTy()) {
    Conversion = LLVMBuilder->CreateFPCast(Source, TargetTy);
  } else {
    ASSERT(UNREACHED);
  }

  IRNode *Result = convertToStackType((IRNode *)Conversion, Info.CorType);
  return Result;
}

// This method only handles basic arithmetic conversions for use in
// binary operations.
IRNode *GenIR::convert(Type *Ty, Value *Node, bool SourceIsSigned) {
  Type *SourceTy = Node->getType();
  Value *Result = nullptr;

  if (Ty == SourceTy) {
    Result = Node;
  } else if (SourceTy->isIntegerTy() && Ty->isIntegerTy()) {
    Result = LLVMBuilder->CreateIntCast(Node, Ty, SourceIsSigned);
  } else if (SourceTy->isFloatingPointTy() && Ty->isFloatingPointTy()) {
    Result = LLVMBuilder->CreateFPCast(Node, Ty);
  } else if (SourceTy->isPointerTy() && Ty->isIntegerTy()) {
    Result = LLVMBuilder->CreatePtrToInt(Node, Ty);
  } else {
    ASSERT(UNREACHED);
  }

  return (IRNode *)Result;
}

// Common to both recursive and non-recursive tail call considerations.
// The debug messages are only wanted when checking the general case
// and not for special recursive checks.
bool GenIR::tailCallChecks(CORINFO_METHOD_HANDLE DeclaredMethod,
                           CORINFO_METHOD_HANDLE ExactMethod,
                           bool IsUnmarkedTailCall,
                           bool HasIndirectResultOrArgument) {
  const char *Reason = nullptr;
  bool SuppressReport = false;
  uint32_t MethodCompFlags = getCurrentMethodAttribs();

  if (MethodCompFlags & CORINFO_FLG_SYNCH) {
    Reason = "synchronized";
  } else if (MethodCompFlags & CORINFO_FLG_SECURITYCHECK) {
    Reason = "caller's declarative security";
  } else if (IsUnmarkedTailCall && !JitContext->Options->EnableOptimization) {
    Reason = "not optimizing";
  } else if (IsUnmarkedTailCall && !JitContext->Options->DoTailCallOpt) {
    Reason = "tail call opt disabled";
  } else if (IsUnmarkedTailCall && HasLocAlloc) {
    Reason = "localloc";
  } else if (IsUnmarkedTailCall && HasAddressTaken) {
    Reason = "address taken local or argument";
  } else if (NeedsStackSecurityCheck) {
    Reason = "GS";
  } else if (!canTailCall(DeclaredMethod, ExactMethod, !IsUnmarkedTailCall)) {
    Reason = "canTailCall declined";
    SuppressReport = true;
  } else if (getCurrentMethodHandle() == ExactMethod) {
    // For tail recursion, also run the canInline check.
    CorInfoInline inlineCheck =
        canInline(getCurrentMethodHandle(), ExactMethod, nullptr);
    if (inlineCheck != CorInfoInline::INLINE_PASS) {
      Reason = "tail recursion and can't inline";
    }
  } else if (HasIndirectResultOrArgument) {
    Reason = "pass by reference result or argument";
  }

  // Did we find any reason not to tail call? If not, we're good.
  if (Reason == nullptr) {
    return true;
  }

  // Else report the failure to the EE, if needed
  if (!SuppressReport) {
    JitContext->JitInfo->reportTailCallDecision(
        getCurrentMethodHandle(), ExactMethod, !IsUnmarkedTailCall,
        TAILCALL_FAIL, Reason);
  }

  return false;
}

void GenIR::returnOpcode(IRNode *Opr, bool IsSynchronizedMethod) {
  const ABIArgInfo &ResultInfo = ABIMethodSig.getResultInfo();
  const CallArgType &ResultArgType = MethodSignature.getResultType();
  CorInfoType ResultCorType = ResultArgType.CorType;
  Value *ResultValue = nullptr;
  bool IsVoidReturn = ResultCorType == CORINFO_TYPE_VOID;

  BasicBlock *CurrentBlock = LLVMBuilder->GetInsertBlock();
  TerminatorInst *Terminator = CurrentBlock->getTerminator();
  assert(isa<UnreachableInst>(Terminator));
  Terminator->removeFromParent();
  LLVMBuilder->SetInsertPoint(CurrentBlock);

  if (IsVoidReturn) {
    assert(Opr == nullptr);
    assert(ResultInfo.getType()->isVoidTy());
  } else {
    assert(Opr != nullptr);

    if (ResultInfo.getKind() == ABIArgInfo::Indirect) {
      Type *ResultTy = ResultInfo.getType();
      ResultValue = convertFromStackType(Opr, ResultCorType, ResultTy);

      CORINFO_CLASS_HANDLE ResultClass = ResultArgType.Class;
      if (JitContext->JitInfo->isStructRequiringStackAllocRetBuf(ResultClass) ==
          TRUE) {
        // The return buffer must be on the stack; a simple store will suffice.
        LLVMBuilder->CreateStore(ResultValue, IndirectResult);
      } else {
        const bool IsVolatile = false;
        storeIndirectArg(ResultArgType, ResultValue, IndirectResult,
                         IsVolatile);
      }

      ResultValue = IndirectResult;
    } else if (ResultInfo.getKind() == ABIArgInfo::Expand) {
      assert(doesValueRepresentStruct((Value *)Opr) ||
             (((Value *)Opr)->getType()->isVectorTy()));

      const bool IsResult = true;
      ABISignature::expand(*this, ResultInfo.getExpansions(), (Value *)Opr,
                           MutableArrayRef<Value *>(&ResultValue, 1),
                           MutableArrayRef<Type *>(), IsResult);
    } else {
      Type *ResultTy = getType(ResultCorType, ResultArgType.Class);
      ResultValue = convertFromStackType(Opr, ResultCorType, ResultTy);
      ResultValue =
          ABISignature::coerce(*this, ResultInfo.getType(), ResultValue);
    }
  }

  // If the method is synchronized, then we must insert a call to MONITOR_EXIT
  // before returning. The call to MONITOR_EXIT must occur after the return
  // value has been calculated.
  if (IsSynchronizedMethod) {
    const bool IsEnter = false;
    callMonitorHelper(IsEnter);
  }

  if (IsVoidReturn) {
    LLVMBuilder->CreateRetVoid();
  } else {
    LLVMBuilder->CreateRet(ResultValue);
  }
}

bool GenIR::needSequencePoints() { return false; }

void GenIR::setEHInfo(EHRegion *EhRegionTree, EHRegionList *EhRegionList) {
  // TODO: anything we need here?
}

void GenIR::methodNeedsToKeepAliveGenericsContext(bool NewValue) {
  if (NewValue) {
    KeepGenericContextAlive = true;
  }
}

void GenIR::nop() {
  // Preserve Nops in debug builds since they may carry unique source positions.
  if (JitContext->Options->EnableOptimization) {
    return;
  }

  // LLVM has no high-level NOP instruction. Put in a placeholder for now.
  // This will survive lowering, but we may want to do something else that
  // is cleaner
  // TODO: Look into creating a high-level NOP that doesn't get removed and
  // gets lowered to the right platform-specific encoding

  bool IsVariadic = false;
  llvm::FunctionType *FTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(*(JitContext->LLVMContext)), IsVariadic);

  llvm::InlineAsm *AsmCode = llvm::InlineAsm::get(FTy, "nop", "", true, false,
                                                  llvm::InlineAsm::AD_Intel);
  const bool MayThrow = false;
  ArrayRef<Value *> Args;
  makeCall(AsmCode, MayThrow, Args);
}

IRNode *GenIR::unbox(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Object,
                     bool AndLoad, ReaderAlignType Alignment, bool IsVolatile) {
  // Ensure that this method can access the the unbox type.
  CORINFO_HELPER_DESC ThrowHelper;
  CorInfoIsAccessAllowedResult ClassAccessAllowed =
      canAccessClass(ResolvedToken, getCurrentMethodHandle(), &ThrowHelper);
  handleMemberAccess(ClassAccessAllowed, ThrowHelper);

  // Unboxing uses a helper call. Figure out which one.
  CORINFO_CLASS_HANDLE ClassHandle = ResolvedToken->hClass;
  CorInfoHelpFunc HelperId = getUnBoxHelper(ClassHandle);

  // The first helper argument is the class handle for the type to unbox to.
  IRNode *ClassHandleArgument = genericTokenToNode(ResolvedToken);

  // The normal Unboxing helper returns a pointer into the boxed object
  // but the unbox nullable helper takes a pointer to write into and
  // returns void, so we need to shift the arguments and create a temp
  // to write the output into.
  if (HelperId == CORINFO_HELP_UNBOX_NULLABLE) {
    CorInfoType CorType = ReaderBase::getClassType(ClassHandle);
    Type *NullableType = getType(CorType, ClassHandle);
    assert(NullableType->isStructTy());
    IRNode *UnboxedNullable =
        (IRNode *)createTemporary(NullableType, "UnboxedNullable");
    const bool MayThrow = true;
    callHelper(HelperId, MayThrow, nullptr, UnboxedNullable,
               ClassHandleArgument, Object);
    if (AndLoad) {
      setValueRepresentsStruct(UnboxedNullable);
    }
    return UnboxedNullable;
  }

  ASSERTNR(HelperId == CORINFO_HELP_UNBOX);

  // Call helper to do the type check and get the address of the unbox payload.
  Type *PtrTy = getType(CorInfoType::CORINFO_TYPE_BYREF, ClassHandle);
  const bool MayThrow = true;
  IRNode *Result = (IRNode *)callHelperImpl(HelperId, MayThrow, PtrTy,
                                            ClassHandleArgument, Object)
                       .getInstruction();

  // If requested, load the object onto the evaluation stack.
  if (AndLoad) {
    // Result was null checked by the helper, so is non-null here.
    Result =
        loadObjNonNull(ResolvedToken, Result, Alignment, IsVolatile, false);
  }

  return Result;
}

void GenIR::pop(IRNode *Opr) {
  // No actions needed.
}

void GenIR::dup(IRNode *Opr, IRNode **Result1, IRNode **Result2) {
  *Result1 = Opr;
  *Result2 = Opr;
}

bool GenIR::interlockedCmpXchg(IRNode *Destination, IRNode *Exchange,
                               IRNode *Comparand, IRNode **Result,
                               CorInfoIntrinsics IntrinsicID) {
  if (Exchange->getType()->isPointerTy()) {
    Exchange =
        (IRNode *)LLVMBuilder->CreatePtrToInt(Exchange, Comparand->getType());
  }

  ASSERT(Exchange->getType() == Comparand->getType());
  switch (IntrinsicID) {
  case CORINFO_INTRINSIC_InterlockedCmpXchg32:
    ASSERT(Exchange->getType() == Type::getInt32Ty(*JitContext->LLVMContext));
    break;
  case CORINFO_INTRINSIC_InterlockedCmpXchg64:
    ASSERT(Exchange->getType() == Type::getInt64Ty(*JitContext->LLVMContext));
    break;
  default:
    throw NotYetImplementedException("interlockedCmpXchg");
  }

  Type *ComparandTy = Comparand->getType();
  Type *DestinationTy = Destination->getType();

  if (DestinationTy->isIntegerTy()) {
    Type *CastTy = getManagedPointerType(ComparandTy);
    Destination = (IRNode *)LLVMBuilder->CreateIntToPtr(Destination, CastTy);
  } else {
    ASSERT(DestinationTy->isPointerTy());
    Type *CastTy = GcInfo::isGcPointer(DestinationTy)
                       ? getManagedPointerType(ComparandTy)
                       : getUnmanagedPointerType(ComparandTy);
    Destination = (IRNode *)LLVMBuilder->CreatePointerCast(Destination, CastTy);
  }

  Value *Pair = LLVMBuilder->CreateAtomicCmpXchg(
      Destination, Comparand, Exchange, llvm::SequentiallyConsistent,
      llvm::SequentiallyConsistent);
  *Result =
      (IRNode *)LLVMBuilder->CreateExtractValue(Pair, 0, "cmpxchg_result");

  return true;
}

bool GenIR::interlockedIntrinsicBinOp(IRNode *Arg1, IRNode *Arg2,
                                      IRNode **RetVal,
                                      CorInfoIntrinsics IntrinsicID) {
  AtomicRMWInst::BinOp Op = AtomicRMWInst::BinOp::BAD_BINOP;

  switch (IntrinsicID) {
  case CORINFO_INTRINSIC_InterlockedXAdd32:
  case CORINFO_INTRINSIC_InterlockedXAdd64:
    Op = AtomicRMWInst::BinOp::Add;
    break;
  case CORINFO_INTRINSIC_InterlockedXchg32:
  case CORINFO_INTRINSIC_InterlockedXchg64:
    Op = AtomicRMWInst::BinOp::Xchg;
    break;
  default:
    // Leave Op unchanged
    break;
  }

  if (Op != AtomicRMWInst::BinOp::BAD_BINOP) {
    assert(Arg1->getType()->isPointerTy());
    Type *CastTy = GcInfo::isGcPointer(Arg1->getType())
                       ? getManagedPointerType(Arg2->getType())
                       : getUnmanagedPointerType(Arg2->getType());
    Arg1 = (IRNode *)LLVMBuilder->CreatePointerCast(Arg1, CastTy);

    Value *Result = LLVMBuilder->CreateAtomicRMW(
        Op, Arg1, Arg2, AtomicOrdering::SequentiallyConsistent);
    *RetVal = (IRNode *)Result;
    return true;
  } else {
    *RetVal = nullptr;
    return false;
  }
}

bool GenIR::memoryBarrier() {
  // TODO: Here we emit mfence which is stronger than sfence
  // that CLR needs.
  // We could improve this further by using
  // lock or byte ptr [rsp], 0
  // which is faster than sfence.
  LLVMBuilder->CreateFence(SequentiallyConsistent);
  return true;
}

void GenIR::switchOpcode(IRNode *Opr) {
  // We split the block right after the switch during the flow-graph build.
  // The terminator is switch instruction itself.
  // Now condition operand is updated.
  BasicBlock *CurrBlock = LLVMBuilder->GetInsertBlock();
  TerminatorInst *TermInst = CurrBlock->getTerminator();
  SwitchInst *SwitchInstruction = cast<SwitchInst>(TermInst);
  SwitchInstruction->setCondition(Opr);

  Type *OprType = Opr->getType();
  IntegerType *OprIntType = cast<IntegerType>(OprType);
  unsigned OprBitWidth = OprIntType->getBitWidth();

  // LLVM requires that for a switch instruction the type of the selector and
  // the type of the case values all be of the same integer type.
  // The purpose of the following code is to adjust the type of the
  // case values to match the type of the selector if they are different.

  // If a small selector type was used, e.g. i8, the type might not
  // be big enough to hold all the case values. But the value for the
  // selector is popped off the evaluation stack, so it should be at
  // least 32 bits. So check that.
  assert((OprBitWidth >= 32) && "Selector bit width is less then 32");

  // If we have to adjust the types of the operands we use a
  // uint64_t to transfer the value, so we cannot handle larger selector.
  assert((OprBitWidth <= 64) && "Selector bit width is greater than 64");

  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  for (SwitchInst::CaseIt Case : SwitchInstruction->cases()) {
    ConstantInt *OldValue = Case.getCaseValue();
    unsigned OldBitWidth = OldValue->getBitWidth();
    if (OldBitWidth != OprBitWidth) {
      // Need to adjust type.
      assert((OldBitWidth <= 64) && "Old value bit width is greater than 64");
      uint64_t CaseValue = OldValue->getZExtValue();
      ConstantInt *Value =
          ConstantInt::get(LLVMContext, APInt(OprBitWidth, CaseValue, false));
      Case.setValue(Value);
    }
  }
}

void GenIR::throwOpcode(IRNode *Arg1) {
  Type *Void = Type::getVoidTy(*JitContext->LLVMContext);
  const bool MayThrow = true;
  CallSite ThrowCall = callHelperImpl(CORINFO_HELP_THROW, MayThrow, Void, Arg1);

  // Annotate the helper
  ThrowCall.setDoesNotReturn();
}

void GenIR::rethrow() {
  Type *Void = Type::getVoidTy(*JitContext->LLVMContext);
  const bool MayThrow = true;
  CallSite ThrowCall = callHelperImpl(CORINFO_HELP_RETHROW, MayThrow, Void);

  // Annotate the helper
  ThrowCall.setDoesNotReturn();
}

void GenIR::endFilter(IRNode *Arg1) {
  Value *ThrowCondition = LLVMBuilder->CreateIsNull(Arg1, "Rethrow");
  genConditionalThrow(ThrowCondition, CORINFO_HELP_RETHROW, "FilterFalse");
  ReaderOperandStack->push((IRNode *)CurrentRegion->HandlerRegion->Exception);
}

CallSite GenIR::genConditionalHelperCall(
    Value *Condition, CorInfoHelpFunc HelperId, bool MayThrow, Type *ReturnType,
    IRNode *Arg1, IRNode *Arg2, bool CallReturns, const Twine &CallBlockName) {
  // Create the call block and fill it in.
  BasicBlock *CallBlock = createPointBlock(CallBlockName);
  IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
  LLVMBuilder->SetInsertPoint(CallBlock);
  CallSite HelperCall =
      callHelperImpl(HelperId, MayThrow, ReturnType, Arg1, Arg2);

  if (!CallReturns) {
    HelperCall.setDoesNotReturn();
    LLVMBuilder->CreateUnreachable();
  }
  LLVMBuilder->restoreIP(SavedInsertPoint);

  // Splice it into the flow.
  insertConditionalPointBlock(Condition, CallBlock, CallReturns);

  // Return the the call.
  return HelperCall;
}

// Generate a call to the throw helper if the condition is met.
void GenIR::genConditionalThrow(Value *Condition, CorInfoHelpFunc HelperId,
                                const Twine &ThrowBlockName) {
  IRNode *Arg1 = nullptr, *Arg2 = nullptr;
  Type *ReturnType = Type::getVoidTy(*JitContext->LLVMContext);
  const bool MayThrow = true;
  const bool CallReturns = false;
  genConditionalHelperCall(Condition, HelperId, MayThrow, ReturnType, Arg1,
                           Arg2, CallReturns, ThrowBlockName);
}

IRNode *GenIR::genNullCheck(IRNode *Node) {
  // Insert the compare against null.
  Value *Compare = LLVMBuilder->CreateIsNull(Node, "NullCheck");

  // Insert the conditional throw
  CorInfoHelpFunc HelperId = CORINFO_HELP_THROWNULLREF;
  genConditionalThrow(Compare, HelperId, "ThrowNullRef");

  return Node;
}

void GenIR::genBoundsCheck(Value *ArrayLength, Value *Index) {
  CorInfoHelpFunc HelperId = CORINFO_HELP_RNGCHKFAIL;

  // Insert the bound compare.
  // The unsigned conversion allows us to also catch negative indices in the
  // compare.
  Type *ArrayLengthType = ArrayLength->getType();
  ASSERTNR(Index->getType()->getPrimitiveSizeInBits() <=
           ArrayLengthType->getPrimitiveSizeInBits());
  bool IsSigned = false;
  Value *ConvertedIndex =
      LLVMBuilder->CreateIntCast(Index, ArrayLengthType, IsSigned);
  Value *UpperBoundCompare =
      LLVMBuilder->CreateICmpUGE(ConvertedIndex, ArrayLength, "BoundsCheck");
  genConditionalThrow(UpperBoundCompare, HelperId, "ThrowIndexOutOfRange");
}

/// \brief Get the immediate target (innermost exited finally) for this leave.
///
/// Also create any IR and reader state needed to pass the appropriate
/// continuation for this leave to the finallies being exited, and for the
/// finallies to respect the passed continuations.
///
/// \param LeaveOffset  MSIL offset of the leave instruction
/// \param NextOffset   MSIL offset immediately after the leave instruction
/// \param LeaveBlock   Block containing the leave instruction
/// \param TargetOffset Ultimate target of the leave instruction
/// \returns Immediate target of the leave instruction: start of innermost
//           exited finally if any exists, \p TargetOffset otherwise
uint32_t GenIR::updateLeaveOffset(uint32_t LeaveOffset, uint32_t NextOffset,
                                  FlowGraphNode *LeaveBlock,
                                  uint32_t TargetOffset) {
  EHRegion *RootRegion = EhRegionTree;
  if (RootRegion == nullptr) {
    // Leave outside of a protected region is treated like a goto.
    return TargetOffset;
  }
  bool IsInHandler = false;
  return updateLeaveOffset(RootRegion, LeaveOffset, NextOffset, LeaveBlock,
                           TargetOffset, IsInHandler);
}

/// \brief Get the immediate target (innermost exited finally) for this leave.
///
/// Also create any necessary selector variables, finally-exiting switch
/// instructions, and selector variable stores.  Selector variable stores are
/// inserted at the leave location (and \p ContinuationStoreMap is updated so
/// subsequent reader passes will know where to insert IR).  Switch insertion
/// is deferred until the first endfinally for the affected finally is
/// processed.
///
/// \param Region               Current region to process, which contains the
///                             leave instruction.  Inner regions are processed
///                             recursively.
/// \param LeaveOffset          MSIL offset of the leave instruction
/// \param NextOffset           MSIL offset immediately after the leave
///                             instruction
/// \param LeaveBlock           Block containing the leave instruction
/// \param TargetOffset         Ultimate target of the leave instruction
/// \param IsInHandler [in/out] Support for dynamic exceptions is NYI; if this
///                             method finds that the leave is in a handler
///                             which can only be entered by an exception,
///                             IsInHandler is set to true and processing is
///                             aborted (no IR is inserted and the original
///                             \p TargetOffset is returned).  Initial caller
///                             must pass false.
/// \returns Immediate target of the leave instruction: start of innermost
//           exited finally if any exists, \p TargetOffset otherwise
uint32_t GenIR::updateLeaveOffset(EHRegion *Region, uint32_t LeaveOffset,
                                  uint32_t NextOffset,
                                  FlowGraphNode *LeaveBlock,
                                  uint32_t TargetOffset, bool &IsInHandler) {
  ReaderBaseNS::RegionKind RegionKind = rgnGetRegionType(Region);

  if (SuppressExceptionHandlers &&
      ((RegionKind == ReaderBaseNS::RegionKind::RGN_MCatch) ||
       (RegionKind == ReaderBaseNS::RegionKind::RGN_Fault) ||
       (RegionKind == ReaderBaseNS::RegionKind::RGN_Filter) ||
       (RegionKind == ReaderBaseNS::RegionKind::RGN_MExcept))) {
    // This leave is in an exception handler, but handlers are to be
    // suppressed; skip the update for this leave to avoid making code look
    // reachable which is only reachable from handlers.

    IsInHandler = true;
    return TargetOffset;
  }

  EHRegion *FinallyRegion = nullptr;
  uint32_t ChildTargetOffset = TargetOffset;

  if ((RegionKind == ReaderBaseNS::RegionKind::RGN_Try) &&
      ((TargetOffset < rgnGetStartMSILOffset(Region)) ||
       (TargetOffset >= rgnGetEndMSILOffset(Region)))) {
    // We are leaving this try.  See if there is a finally to invoke.
    FinallyRegion = getFinallyRegion(Region);
    if (FinallyRegion) {
      // There is a finally.  Update ChildTargetOffset so that recursive
      // processing for inner regions will know to target this finally.
      ChildTargetOffset = rgnGetStartMSILOffset(FinallyRegion);
    }
  }

  uint32_t InnermostTargetOffset = ChildTargetOffset;

  // Check if this leave exits any nested regions.
  if (EHRegion *ChildRegion = getInnerEnclosingRegion(Region, LeaveOffset)) {
    InnermostTargetOffset =
        updateLeaveOffset(ChildRegion, LeaveOffset, NextOffset, LeaveBlock,
                          ChildTargetOffset, IsInHandler);

    if (IsInHandler) {
      // Skip processing for this leave
      return TargetOffset;
    }
  }

  if (FinallyRegion != nullptr) {
    // Generate the code to set the continuation for the finally we are leaving
    // First, get a pointer to the continuation block.
    FlowGraphNode *TargetNode = nullptr;
    fgAddNodeMSILOffset(&TargetNode, TargetOffset);
    BasicBlock *TargetBlock = (BasicBlock *)TargetNode;

    // Get or create the switch that terminates the finally.
    SwitchInst *Switch = FinallyRegion->EndFinallySwitch;

    if (Switch == nullptr) {
      Switch = createFinallyDispatch(FinallyRegion);
    }

    LoadInst *Load = (LoadInst *)Switch->getCondition();
    Value *SelectorAddr = Load->getPointerOperand();
    IntegerType *SelectorType = (IntegerType *)Load->getType();
    ConstantInt *SelectorValue = Switch->findCaseDest(TargetBlock);

    if (SelectorValue == nullptr) {
      // The switch doesn't have a case for this target continuation yet;
      // add one.
      SelectorValue =
          ConstantInt::get(SelectorType, Switch->getNumCases() + 1U);
      Switch->addCase(SelectorValue, TargetBlock);
    }

    // Create the store instruction to set this continuation selector for
    // this leave across this finally.
    LLVMBuilder->SetInsertPoint(LeaveBlock);
    StoreInst *Store = LLVMBuilder->CreateStore(SelectorValue, SelectorAddr);

    if (InnermostTargetOffset == ChildTargetOffset) {
      // This is the innermost finally being exited (no child region updated
      // InnermostTargetOffset).
      // Record the first continuation selector store in this block so that
      // the 2nd pass will know to insert code before them rather than after
      // them.
      ContinuationStoreMap.insert(std::make_pair(NextOffset, Store));

      // Update InnermostTargetOffset.
      InnermostTargetOffset = FinallyRegion->StartMsilOffset;
    }
  }

  return InnermostTargetOffset;
}

static EHRegion *getCommonAncestor(EHRegion *Region1, EHRegion *Region2) {
  if ((Region1 == nullptr) || (Region2 == nullptr)) {
    return nullptr;
  }
  int Depth1 = 0;
  for (EHRegion *Ancestor1 = Region1->Parent; Ancestor1 != nullptr;
       Ancestor1 = Ancestor1->Parent) {
    ++Depth1;
  }
  int Depth2 = 0;
  for (EHRegion *Ancestor2 = Region2->Parent; Ancestor2 != nullptr;
       Ancestor2 = Ancestor2->Parent) {
    ++Depth2;
  }
  while (Depth1 > Depth2) {
    Region1 = Region1->Parent;
    --Depth1;
  }
  while (Depth2 > Depth1) {
    Region2 = Region2->Parent;
    --Depth2;
  }
  while (Region1 != Region2) {
    Region1 = Region1->Parent;
    Region2 = Region2->Parent;
  }
  return Region1;
}

void GenIR::leave(uint32_t TargetOffset) {
  // Check if this leave exits any catchpads (catches or filter handlers)
  BasicBlock *CurrentBlock = LLVMBuilder->GetInsertBlock();
  BasicBlock *TargetBlock = CurrentBlock->getUniqueSuccessor();
  EHRegion *TargetRegion = fgNodeGetRegion((FlowGraphNode *)TargetBlock);
  EHRegion *ExitToRegion = getCommonAncestor(CurrentRegion, TargetRegion);
  SmallVector<EHRegion *, 4> Handlers;
  for (EHRegion *Leaving = CurrentRegion; Leaving != ExitToRegion;
       Leaving = Leaving->Parent) {
    if ((Leaving->Kind == ReaderBaseNS::RegionKind::RGN_MCatch) ||
        (Leaving->Kind == ReaderBaseNS::RegionKind::RGN_MExcept)) {
      Handlers.push_back(Leaving);
    }
  }
  if (!Handlers.empty()) {
    IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
    BasicBlock *NextTarget = TargetBlock;
    for (auto I = Handlers.rbegin(), E = Handlers.rend(); I != E; ++I) {
      EHRegion *Handler = *I;
      BasicBlock *ExitBlock = createPointBlock(
          Handler->EndMsilOffset,
          (Handler->Kind == ReaderBaseNS::RegionKind::RGN_MCatch
               ? "ExitCatch"
               : "ExitHandler"));
      LLVMBuilder->SetInsertPoint(ExitBlock);
      LLVMBuilder->CreateCatchRet(Handler->CatchPad, NextTarget);
      NextTarget = ExitBlock;
      ExitBlock->moveAfter(CurrentBlock);
    }
    cast<BranchInst>(CurrentBlock->getTerminator())
        ->setSuccessor(0, NextTarget);
    LLVMBuilder->restoreIP(SavedInsertPoint);
  }
}

IRNode *GenIR::loadStr(mdToken Token) {
  // TODO: Special handling for cold blocks
  void *StringHandle;
  InfoAccessType Iat = constructStringLiteral(Token, &StringHandle);
  ASSERTNR(StringHandle != nullptr);

  return stringLiteral(Token, StringHandle, Iat);
}

IRNode *GenIR::stringLiteral(mdToken Token, void *StringHandle,
                             InfoAccessType Iat) {
  IRNode *StringPtrNode = nullptr;
  switch (Iat) {
#if defined(FEATURE_BASICFREEZE)
  case IAT_VALUE:
    StringPtrNode =
        handleToIRNode(Token, StringHandle, 0, false, false, true, false, true);
    break;
#endif
  case IAT_PVALUE:
  case IAT_PPVALUE: {
    // Get the raw address of the pointer to reference to string.
    IRNode *RawAddress = handleToIRNode(
        Token, StringHandle, 0, (Iat == IAT_PPVALUE), true, true, false);
    // Cast it to the right address type.
    Type *StringRefTy = getBuiltInStringType();
    Type *AddressTy = getUnmanagedPointerType(StringRefTy);
    IRNode *TypedAddress =
        (IRNode *)LLVMBuilder->CreateIntToPtr(RawAddress, AddressTy);
    // Fetch the string reference.
    StringPtrNode = loadIndirNonNull(ReaderBaseNS::LdindRef, TypedAddress,
                                     Reader_AlignNatural, false, false);
    break;
  }
  default:
    ASSERTNR(UNREACHED);
  }
  return StringPtrNode;
}

// Function encodes a handle pointer in IRNode. If IsIndirect is true
// then embHandle is a pointer to the actual handle, and IR must
// be emitted to load the actual handle before encoding it in IR.
IRNode *GenIR::handleToIRNode(mdToken Token, void *EmbHandle, void *RealHandle,
                              bool IsIndirect, bool IsReadOnly,
                              bool IsRelocatable, bool IsCallTarget,
                              bool IsFrozenObject /* default = false */
                              ) {
  std::string HandleName;
  if (IsRelocatable) {
    uint64_t LookupHandle =
        RealHandle ? (uint64_t)RealHandle : (uint64_t)EmbHandle;
    HandleName = getNameForToken(Token, (CORINFO_GENERIC_HANDLE)LookupHandle,
                                 getCurrentContext(), getCurrentModuleHandle());
  }
  return handleToIRNode(HandleName, EmbHandle, RealHandle, IsIndirect,
                        IsReadOnly, IsRelocatable, IsCallTarget,
                        IsFrozenObject);
}

IRNode *GenIR::handleToIRNode(const std::string &HandleName, void *EmbHandle,
                              void *RealHandle, bool IsIndirect,
                              bool IsReadOnly, bool IsRelocatable,
                              bool IsCallTarget,
                              bool IsFrozenObject /* default = false */
                              ) {
  LLVMContext &LLVMContext = *JitContext->LLVMContext;

  if (IsFrozenObject) {
    throw NotYetImplementedException("frozen object");
  }

  uint64_t LookupHandle =
      RealHandle ? (uint64_t)RealHandle : (uint64_t)EmbHandle;
  uint64_t ValueHandle = (uint64_t)EmbHandle;

  Value *HandleValue = nullptr;
  Type *HandleTy = Type::getIntNTy(LLVMContext, TargetPointerSizeInBits);

  if (IsRelocatable) {
    GlobalVariable *GlobalVar = getGlobalVariable(
        LookupHandle, ValueHandle, HandleTy, HandleName, IsReadOnly);
    HandleValue = LLVMBuilder->CreatePtrToInt(GlobalVar, HandleTy);
  } else {
    uint32_t NumBits = TargetPointerSizeInBits;
    bool IsSigned = false;

    HandleValue = ConstantInt::get(
        LLVMContext, APInt(NumBits, (uint64_t)EmbHandle, IsSigned));
  }

  if (IsIndirect) {
    Type *HandlePtrTy = getUnmanagedPointerType(HandleTy);
    Value *HandlePtr = LLVMBuilder->CreateIntToPtr(HandleValue, HandlePtrTy);
    HandleValue = LLVMBuilder->CreateLoad(HandlePtr);
  }

  return (IRNode *)HandleValue;
}

std::string GenIR::getNameForToken(mdToken Token, CORINFO_GENERIC_HANDLE Handle,
                                   CORINFO_CONTEXT_HANDLE Context,
                                   CORINFO_MODULE_HANDLE Scope) {
  std::string Storage;
  raw_string_ostream OS(Storage);

  // DEBUG ONLY: Find the real name based on the token/context/scope
  switch (TypeFromToken(Token)) {
  case mdtJitHelper:
    OS << JitContext->JitInfo->getHelperName(
              (CorInfoHelpFunc)RidFromToken(Token))
       << "::JitHelper";
    break;
  case mdtVarArgsHandle:
    OS << getNameForToken(TokenFromRid(RidFromToken(Token), mdtMemberRef),
                          Handle, Context, Scope)
       << "::VarArgsHandle";
    break;
  case mdtVarArgsMDHandle:
    OS << getNameForToken(TokenFromRid(RidFromToken(Token), mdtMethodDef),
                          Handle, Context, Scope)
       << "::VarArgsHandle";
    break;
  case mdtVarArgsMSHandle:
    OS << getNameForToken(TokenFromRid(RidFromToken(Token), mdtMethodSpec),
                          Handle, Context, Scope)
       << "::VarArgsHandle";
    break;
  case mdtVarArgsSigHandle:
    OS << getNameForToken(TokenFromRid(RidFromToken(Token), mdtSignature),
                          Handle, Context, Scope)
       << "::VarArgsHandle";
    break;
  case mdtInterfaceOffset:
    OS << "InterfaceOffset";
    break;
  case mdtCodeOffset:
    OS << "CodeOffset";
    break;
  case mdtPInvokeCalliHandle:
    OS << "PinvokeCalliHandle";
    break;
  case mdtIBCProfHandle:
    OS << "IBCProfHandle";
    break;
  case mdtMBReturnHandle:
    OS << "MBReturnHandle";
    break;
  case mdtSyncHandle:
    OS << "SyncHandle";
    break;
  case mdtGSCookie:
    OS << "GSCookieAddr";
    break;
  case mdtJMCHandle:
    OS << "JMCHandle";
    break;
  case mdtCaptureThreadGlobal:
    OS << "CaptureThreadGlobal";
    break;
  case mdtSignature:
    // findNameOfToken doesn't do anythign interesting and getMemberParent
    // asserts so special case it here.
    OS << format("Signature(TK_%x)", Token);
    break;
  case mdtString:
    OS << format("String(TK_%x)", Token);
    break;
  case mdtModule:
    // Can't get the parent of a module, because it's the 'root'
    assert(Token == mdtModule);
    OS << "ModuleHandle";
    break;
  case mdtModuleID:
    OS << format("EmbeddedModuleID(%x)", Token);
    break;
  case mdtMethodHandle: {
    const char *MethodName;
    const char *ModuleName = NULL;
    MethodName = JitContext->JitInfo->getMethodName(
        (CORINFO_METHOD_HANDLE)Handle, &ModuleName);
    OS << format("TypeContext(%s.%s)", ModuleName, MethodName);
  } break;
  case mdtClassHandle: {
    char *ClassName = getClassNameWithNamespace((CORINFO_CLASS_HANDLE)Handle);
    if (ClassName != nullptr) {
      OS << format("TypeContext(%s)", ClassName);
      delete[] ClassName;
    }
  } break;
  default:
    char Buffer[MAX_CLASSNAME_LENGTH];
    JitContext->JitInfo->findNameOfToken(Scope, Token, Buffer, COUNTOF(Buffer));
    OS << format("%s", &Buffer[0]);
    break;
  }

  OS.flush();

  if (Storage.empty()) {
    assert(Handle != 0);
    if (RidFromToken(Token) == 0) {
      OS << format("!handle_%Ix", Handle);
    } else {
      OS << format("!TK_%x_handle_%Ix", Token, Handle);
    }
    OS.flush();
  }

  return Storage;
}

IRNode *GenIR::makeRefAnyDstOperand(CORINFO_CLASS_HANDLE Class) {
  CorInfoType CorType = ReaderBase::getClassType(Class);
  Type *ElementTy = getType(CorType, Class);
  Type *Ty = getManagedPointerType(ElementTy);
  return (IRNode *)Constant::getNullValue(Ty);
}

// TODO: currently PtrType telling base or interior pointer is ignored.
// So for now, deliberately we keep this API to retain the call site.
IRNode *GenIR::makePtrNode(ReaderPtrType PtrType) { return loadNull(); }

// Load a pointer-sized value from the indicated address.
// Used when navigating through runtime data structures.
// This should not be used for accessing user data types.
IRNode *GenIR::derefAddress(IRNode *Address, bool DstIsGCPtr, bool IsConst,
                            bool AddressMayBeNull) {

  // We don't know the true referent type so just use a pointer sized
  // integer or GC pointer to i8 for the result.

  Type *ReferentTy = DstIsGCPtr
                         ? (Type *)getManagedPointerType(
                               Type::getInt8Ty(*JitContext->LLVMContext))
                         : (Type *)Type::getIntNTy(*JitContext->LLVMContext,
                                                   TargetPointerSizeInBits);

  // Address is a pointer, but since it may come from dereferencing into
  // runtime data structures with unknown field types, we may need a cast here
  // to make it so.
  Type *AddressTy = Address->getType();
  PointerType *AddressPointerTy = dyn_cast<PointerType>(AddressTy);
  if (AddressPointerTy == nullptr) {
    // Cast the integer to an appropriate unmanaged pointer
    Type *CastTy = getUnmanagedPointerType(ReferentTy);
    Address = (IRNode *)LLVMBuilder->CreateIntToPtr(Address, CastTy);
  } else if (AddressPointerTy->getElementType() != ReferentTy) {
    // Cast to the appropriate referent type
    Type *CastTy = GcInfo::isGcPointer(AddressPointerTy)
                       ? getManagedPointerType(ReferentTy)
                       : getUnmanagedPointerType(ReferentTy);
    Address = (IRNode *)LLVMBuilder->CreatePointerCast(Address, CastTy);
  }

  LoadInst *Result = makeLoad(Address, false, AddressMayBeNull);

  if (IsConst) {
    MDNode *EmptyNode =
        MDNode::get(*JitContext->LLVMContext, ArrayRef<Metadata *>());

    Result->setMetadata(LLVMContext::MD_invariant_load, EmptyNode);
  }

  return (IRNode *)Result;
}

// Create an empty block to hold IR for some conditional instructions at a
// particular point in the MSIL (conditional LLVM instructions that are part
// of the expansion of a single MSIL instruction)
BasicBlock *GenIR::createPointBlock(uint32_t PointOffset,
                                    const Twine &BlockName) {
  BasicBlock *Block =
      BasicBlock::Create(*JitContext->LLVMContext, BlockName, Function);

  // Give the point block equal start and end offsets so subsequent processing
  // won't try to translate MSIL into it.
  FlowGraphNode *PointFlowGraphNode = (FlowGraphNode *)Block;
  fgNodeSetStartMSILOffset(PointFlowGraphNode, PointOffset);
  fgNodeSetEndMSILOffset(PointFlowGraphNode, PointOffset);

  // Make the point block part of the current region
  fgNodeSetRegion(PointFlowGraphNode, CurrentRegion);

  // Point blocks don't need an operand stack: they don't have any MSIL and
  // any successor block will get the stack propagated from the other
  // predecessor.
  fgNodeSetPropagatesOperandStack(PointFlowGraphNode, false);

  if (!DoneBuildingFlowGraph) {
    // Position this block in the list so that it will get moved to its point
    // at the end of flow-graph construction.
    if (PointOffset == MethodInfo->ILCodeSize) {
      // The point is the end of the function, which is already where this
      // block is.
    } else {
      // Request a split at PointOffset, and move this block before the temp
      // target so it will get moved after the split is created (in
      // movePointBlocks).
      FlowGraphNode *Next = nullptr;
      fgAddNodeMSILOffset(&Next, PointOffset);
      Block->moveBefore(Next);
    }
  }

  return Block;
}

// Split the current block, inserting a conditional branch to the PointBlock
// based on Condition, and branch back from the PointBlock to the continuation
// if Rejoin is true. Return the continuation.
BasicBlock *GenIR::insertConditionalPointBlock(Value *Condition,
                                               BasicBlock *PointBlock,
                                               bool Rejoin) {
  // Split the current block.  This creates a goto connecting the blocks that
  // we'll replace with the conditional branch.
  TerminatorInst *Goto;
  BasicBlock *ContinueBlock = splitCurrentBlock(&Goto);
  BranchInst *Branch = BranchInst::Create(PointBlock, ContinueBlock, Condition);
  replaceInstruction(Goto, Branch);

  if (Rejoin) {
    BasicBlock *RejoinFromBlock = PointBlock;
    // Allow that the point block may have been split to insert invoke
    // instructions.
    TerminatorInst *Terminator;
    while ((Terminator = RejoinFromBlock->getTerminator()) != nullptr) {
      assert(isa<InvokeInst>(Terminator));
      RejoinFromBlock = cast<InvokeInst>(Terminator)->getNormalDest();
    }
    IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
    LLVMBuilder->SetInsertPoint(RejoinFromBlock);
    LLVMBuilder->CreateBr(ContinueBlock);
    LLVMBuilder->restoreIP(SavedInsertPoint);
  }

  return ContinueBlock;
}

BasicBlock *GenIR::splitCurrentBlock(TerminatorInst **Goto) {
  BasicBlock *CurrentBlock = LLVMBuilder->GetInsertBlock();
  BasicBlock::iterator InsertPoint = LLVMBuilder->GetInsertPoint();
  Instruction *NextInstruction =
      (InsertPoint == CurrentBlock->end() ? nullptr
                                          : (Instruction *)InsertPoint);
  uint32_t CurrentEndOffset =
      fgNodeGetEndMSILOffset((FlowGraphNode *)CurrentBlock);
  uint32_t SplitOffset;

  if (CurrentEndOffset >= NextInstrOffset) {
    // Split at offset NextInstrOffset rather than CurrInstrOffset.  We're
    // already generating the IR for the instr at CurrInstrOffset, and using
    // NextInstrOffset here ensures that we won't redundantly try to add this
    // instruction again when processing moves to NewBlock.

    SplitOffset = NextInstrOffset;
  } else {
    // It may be the case that we're splitting a point block, whose point is
    // CurrInstrOffset rather than NextInstrOffset.  In that case, give the new
    // point block the same point as the old one, to ensure that the "split"
    // operation never produces a block whose IL offset range isn't contained
    // in the original block's range.

    assert(CurrentEndOffset == CurrInstrOffset);
    SplitOffset = CurrentEndOffset;
  }
  BasicBlock *NewBlock = ReaderBase::fgSplitBlock(
      (FlowGraphNode *)CurrentBlock, SplitOffset, (IRNode *)NextInstruction);

  if (Goto != nullptr) {
    // Report the created goto to the caller
    *Goto = CurrentBlock->getTerminator();
  }

  // Move the insertion point to the first instruction in the new block
  if (NextInstruction == nullptr) {
    LLVMBuilder->SetInsertPoint(NewBlock);
  } else {
    LLVMBuilder->SetInsertPoint(NewBlock, NextInstruction->getIterator());
  }
  return NewBlock;
}

void GenIR::replaceInstruction(Instruction *OldInstruction,
                               Instruction *NewInstruction) {
  // Record where we were
  IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();

  // Insert the new instruction in the proper place.
  LLVMBuilder->SetInsertPoint(OldInstruction);
  LLVMBuilder->Insert(NewInstruction);

  // Remove the old instruction.  Make sure it has no uses first.
  assert(OldInstruction->use_empty());
  OldInstruction->eraseFromParent();

  // Move the insertion point back.
  LLVMBuilder->restoreIP(SavedInsertPoint);
}

// Add a PHI at the start of the JoinBlock to merge the two results.
PHINode *GenIR::mergeConditionalResults(BasicBlock *JoinBlock, Value *Arg1,
                                        BasicBlock *Block1, Value *Arg2,
                                        BasicBlock *Block2,
                                        const Twine &NameStr) {
  PHINode *Phi = createPHINode(JoinBlock, Arg1->getType(), 2, NameStr);
  Phi->addIncoming(Arg1, Block1);
  Phi->addIncoming(Arg2, Block2);
  if (doesValueRepresentStruct(Arg1)) {
    assert(doesValueRepresentStruct(Arg2));
    setValueRepresentsStruct(Phi);
  }
  return Phi;
}

// Handle case of an indirection from CORINFO_RUNTIME_LOOKUP where
// testForFixup was true.
//
// If lowest bit of Address is set, clear it and dereference to obtain the
// result. If not, just set the result to Address.
IRNode *GenIR::conditionalDerefAddress(IRNode *Address) {
  // Build up the initial bit test
  BasicBlock *TestBlock = LLVMBuilder->GetInsertBlock();
  Type *AddressTy = Address->getType();
  Type *IntPtrTy = getType(CorInfoType::CORINFO_TYPE_NATIVEINT, nullptr);
  Value *AddressAsInt = LLVMBuilder->CreatePtrToInt(Address, IntPtrTy);
  Value *One = loadConstantI(1);
  Value *TestValue = LLVMBuilder->CreateAnd(AddressAsInt, One);
  Value *TestPredicate = LLVMBuilder->CreateICmpEQ(TestValue, One);

  // Allocate the indirection block and fill it in.
  BasicBlock *IndirectionBlock = createPointBlock("CondDerefAddr");
  IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
  LLVMBuilder->SetInsertPoint(IndirectionBlock);
  Value *Mask = LLVMBuilder->CreateNot(One);
  Value *ConditionalAddressAsInt = LLVMBuilder->CreateAnd(AddressAsInt, Mask);
  Value *ConditionalAddress = LLVMBuilder->CreateIntToPtr(
      ConditionalAddressAsInt, AddressTy->getPointerTo());
  Value *UpdatedAddress = LLVMBuilder->CreateLoad(ConditionalAddress);
  LLVMBuilder->restoreIP(SavedInsertPoint);

  // Splice the indirection block in.
  BasicBlock *ContinueBlock =
      insertConditionalPointBlock(TestPredicate, IndirectionBlock, true);

  // Merge the two addresses and return the result.
  PHINode *Result = mergeConditionalResults(ContinueBlock, Address, TestBlock,
                                            UpdatedAddress, IndirectionBlock);

  return (IRNode *)Result;
}

IRNode *GenIR::loadVirtFunc(IRNode *Arg1, CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            CORINFO_CALL_INFO *CallInfo) {
  if (JitContext->Flags & CORJIT_FLG_READYTORUN) {
    return getReadyToRunVirtFuncPtr(Arg1, ResolvedToken, CallInfo);
  } else {
    Type *Ty = Type::getIntNTy(*this->JitContext->LLVMContext,
                               TargetPointerSizeInBits);
    const bool MayThrow = true;
    IRNode *TypeToken = genericTokenToNode(ResolvedToken, true);
    IRNode *MethodToken = genericTokenToNode(ResolvedToken);

    return (IRNode *)callHelperImpl(CORINFO_HELP_VIRTUAL_FUNC_PTR, MayThrow, Ty,
                                    Arg1, TypeToken, MethodToken)
        .getInstruction();
  }
}

IRNode *GenIR::getReadyToRunVirtFuncPtr(IRNode *Arg1,
                                        CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                        CORINFO_CALL_INFO *CallInfo) {
  if (CallInfo->kind != CORINFO_VIRTUALCALL_LDVIRTFTN) {
    return rdrMakeLdFtnTargetNode(ResolvedToken, CallInfo);
  } else {
    Type *Ty = Type::getIntNTy(*this->JitContext->LLVMContext,
                               TargetPointerSizeInBits);
    const bool MayThrow = true;
    InfoAccessType AccessType =
        CallInfo->codePointerLookup.constLookup.accessType;
    void *Address = CallInfo->codePointerLookup.constLookup.addr;
    assert(Address != nullptr);
    assert(AccessType != IAT_PPVALUE);
    bool IsIndirect = AccessType != IAT_VALUE;
    void *RealHandle = nullptr;
    const bool IsRelocatable = true;
    const bool IsCallTarget = true;
    IRNode *HelperAddress =
        handleToIRNode(ResolvedToken->token, Address, RealHandle, IsIndirect,
                       IsIndirect, IsRelocatable, IsCallTarget);
    return (IRNode *)callHelperImpl(CORINFO_HELP_READYTORUN_VIRTUAL_FUNC_PTR,
                                    HelperAddress, MayThrow, Ty, Arg1)
        .getInstruction();
  }
}

IRNode *GenIR::getTypedAddress(IRNode *Addr, CorInfoType CorInfoType,
                               CORINFO_CLASS_HANDLE ClassHandle,
                               ReaderAlignType Alignment, uint32_t *Align) {
  // Get type of the result.
  Type *AddressTy = Addr->getType();
  IRNode *TypedAddr = Addr;

  // For the 'REFANY' case, verify the address carries
  // reasonable typing. Address producer must ensure this.
  if (CorInfoType == CORINFO_TYPE_REFANY) {
    PointerType *PointerTy = dyn_cast<PointerType>(AddressTy);
    if (PointerTy != nullptr) {
      Type *ReferentTy = PointerTy->getPointerElementType();

      // The result of the load is an object reference or a typed reference.
      if (ReferentTy->isStructTy()) {
#ifndef NDEBUG
        // This is the typed reference case. We shouldn't need a cast here.
        Type *ExpectedTy = this->getType(CorInfoType, ClassHandle);
        assert(ReferentTy == ExpectedTy);
#endif
      } else {
        // This is the object reference case so addr should be ptr to managed
        // ptr to struct.
        if (!ReferentTy->isPointerTy()) {
          // If we hit this we should fix the address producer, not
          // coerce the type here.
          throw NotYetImplementedException(
              "unexpected type in load/store primitive");
        }
        assert(GcInfo::isGcPointer(ReferentTy));
        assert(cast<PointerType>(ReferentTy)
                   ->getPointerElementType()
                   ->isStructTy());
      }
    } else {
      // This must be a nativeint, in which case we cast the address
      // to an address of Object.
      assert(AddressTy == Type::getIntNTy(*JitContext->LLVMContext,
                                          TargetPointerSizeInBits));

      Type *ObjectType = getBuiltInObjectType();
      TypedAddr = (IRNode *)LLVMBuilder->CreateIntToPtr(
          Addr, getUnmanagedPointerType(ObjectType));
    }
    // GC pointers are always naturally aligned
    Alignment = Reader_AlignNatural;
  } else {
    // For other cases we may need to cast the address.
    Type *ExpectedTy = this->getType(CorInfoType, ClassHandle);
    PointerType *PointerTy = dyn_cast<PointerType>(AddressTy);
    if (PointerTy != nullptr) {
      Type *ReferentTy = PointerTy->getPointerElementType();
      if (ReferentTy != ExpectedTy) {
        Type *PtrToExpectedTy = GcInfo::isGcPointer(PointerTy)
                                    ? getManagedPointerType(ExpectedTy)
                                    : getUnmanagedPointerType(ExpectedTy);
        TypedAddr =
            (IRNode *)LLVMBuilder->CreatePointerCast(Addr, PtrToExpectedTy);
      }
    } else {
      assert(AddressTy->isIntegerTy());
      Type *PtrToExpectedTy = getUnmanagedPointerType(ExpectedTy);
      TypedAddr = (IRNode *)LLVMBuilder->CreateIntToPtr(Addr, PtrToExpectedTy);
    }
  }

  *Align = convertReaderAlignment(Alignment);
  return TypedAddr;
}

IRNode *GenIR::loadPrimitiveType(IRNode *Addr, CorInfoType CorInfoType,
                                 ReaderAlignType Alignment, bool IsVolatile,
                                 bool IsInterfReadOnly, bool AddressMayBeNull) {
  uint32_t Align;
  CORINFO_CLASS_HANDLE ClassHandle = nullptr;
  IRNode *TypedAddr = nullptr;
  // For refany, ensure that Addr is ptr to managed ptr, and if not, arrange to
  // coerce the type to ptr to ptr to object.
  if (CorInfoType == CORINFO_TYPE_REFANY) {
    bool NeedsCoercion = false;
    Type *AddressTy = Addr->getType();
    PointerType *PointerTy = dyn_cast<PointerType>(AddressTy);
    if (PointerTy != nullptr) {
      Type *ReferentTy = PointerTy->getPointerElementType();
      NeedsCoercion = !GcInfo::isGcPointer(ReferentTy);
    } else {
      NeedsCoercion = true;
    }
    if (NeedsCoercion) {
      // Prepare to coerce to proper type
      ClassHandle = getBuiltinClass(CorInfoClassId::CLASSID_SYSTEM_OBJECT);
      CorInfoType = CORINFO_TYPE_CLASS;
    }
  }
  // Coerce address to expected type, if needed.
  TypedAddr =
      getTypedAddress(Addr, CorInfoType, ClassHandle, Alignment, &Align);
  LoadInst *LoadInst = makeLoad(TypedAddr, IsVolatile, AddressMayBeNull);
  LoadInst->setAlignment(Align);

  return convertToStackType((IRNode *)LoadInst, CorInfoType);
}

IRNode *GenIR::loadNonPrimitiveObj(IRNode *Addr,
                                   CORINFO_CLASS_HANDLE ClassHandle,
                                   ReaderAlignType Alignment, bool IsVolatile,
                                   bool AddressMayBeNull) {
  uint32_t Align;
  CorInfoType CorType = JitContext->JitInfo->asCorInfoType(ClassHandle);
  IRNode *TypedAddr =
      getTypedAddress(Addr, CorType, ClassHandle, Alignment, &Align);
  Type *Type = getType(CorType, ClassHandle);
  if (Type->isVectorTy()) {
    return (IRNode *)makeLoad(Addr, IsVolatile, AddressMayBeNull);
  }
  StructType *StructTy = cast<StructType>(Type);
  return loadNonPrimitiveObj(StructTy, TypedAddr, Alignment, IsVolatile,
                             AddressMayBeNull);
}

IRNode *GenIR::loadNonPrimitiveObj(StructType *StructTy, IRNode *Address,
                                   ReaderAlignType Alignment, bool IsVolatile,
                                   bool AddressMayBeNull) {
  if (AddressMayBeNull) {
    if (UseExplicitNullChecks) {
      Address = genNullCheck(Address);
    } else {
      // If we had support for implicit null checks, this
      // path would need to annotate the load we're about
      // to generate.
    }
  }

  IRNode *Copy = (IRNode *)createTemporary(StructTy);
  copyStructNoBarrier(cast<StructType>(StructTy), Copy, Address, IsVolatile,
                      Alignment);
  setValueRepresentsStruct(Copy);
  return Copy;
}

void GenIR::classifyCmpType(Type *Ty, uint32_t &Size, bool &IsPointer,
                            bool &IsFloat) {
  switch (Ty->getTypeID()) {
  case Type::TypeID::IntegerTyID:
    Size = Ty->getIntegerBitWidth();
    break;
  case Type::TypeID::PointerTyID:
    Size = TargetPointerSizeInBits;
    IsPointer = true;
    break;
  case Type::TypeID::FloatTyID:
    IsFloat = true;
    Size = 32;
    break;
  case Type::TypeID::DoubleTyID:
    IsFloat = true;
    Size = 64;
    break;
  default:
    ASSERTM(false, "Unexpected type in cmp");
    break;
  }
}

bool GenIR::prepareArgsForCompare(IRNode *&Arg1, IRNode *&Arg2) {
  // Grab the types to be compared.
  Type *Ty1 = Arg1->getType();
  Type *Ty2 = Arg2->getType();

  // Types can only be int32, int64, float, double, or pointer.
  // They must match in bit size.
  uint32_t Size1 = 0;
  uint32_t Size2 = 0;
  bool IsFloat1 = false;
  bool IsFloat2 = false;
  bool IsPointer1 = false;
  bool IsPointer2 = false;

  classifyCmpType(Ty1, Size1, IsPointer1, IsFloat1);
  classifyCmpType(Ty2, Size2, IsPointer2, IsFloat2);

  ASSERT((Size1 == 32) || (Size1 == 64));
  ASSERT((Size2 == 32) || (Size2 == 64));
  ASSERT(IsFloat1 == IsFloat2);

  if (Size1 != Size2) {
    // int32 can be compared with nativeint and float can be compared
    // with double
    ASSERT(!IsPointer1 && !IsPointer2);
    bool IsSigned = true;
    if (Size1 == 32) {
      if (IsFloat1) {
        Arg1 = (IRNode *)LLVMBuilder->CreateFPExt(Arg1, Ty2);
      } else {
        Arg1 = (IRNode *)LLVMBuilder->CreateIntCast(Arg1, Ty2, IsSigned);
      }
    } else {
      if (IsFloat2) {
        Arg2 = (IRNode *)LLVMBuilder->CreateFPExt(Arg2, Ty1);
      } else {
        Arg2 = (IRNode *)LLVMBuilder->CreateIntCast(Arg2, Ty1, IsSigned);
      }
    }
  } else if (Ty1 != Ty2) {
    // Make types agree without perturbing bit patterns.
    // Must be ptr-ptr or int-ptr case.
    ASSERT(IsPointer1 || IsPointer2);

    // For the ptr-ptr case we pointer cast arg2 to match arg1.
    // For the ptr-int cases we cast the pointer to int.
    if (IsPointer1) {
      if (IsPointer2) {
        // PointerCast Arg2 to match Arg1
        Arg2 = (IRNode *)LLVMBuilder->CreatePointerCast(Arg2, Ty1);
      } else {
        // Cast ptr Arg1 to match int Arg2
        Arg1 = (IRNode *)LLVMBuilder->CreatePointerCast(Arg1, Ty2);
      }
    } else {
      // Cast ptr Arg2 to match int Arg1
      Arg2 = (IRNode *)LLVMBuilder->CreatePointerCast(Arg2, Ty1);
    }
  }

  // Types should now match up.
  ASSERT(Arg1->getType() == Arg2->getType());

  return IsFloat1;
}

IRNode *GenIR::cmp(ReaderBaseNS::CmpOpcode Opcode, IRNode *Arg1, IRNode *Arg2) {

  bool IsFloat = prepareArgsForCompare(Arg1, Arg2);

  static const CmpInst::Predicate IntCmpMap[ReaderBaseNS::LastCmpOpcode] = {
      CmpInst::Predicate::ICMP_EQ,  // CEQ,
      CmpInst::Predicate::ICMP_SGT, // CGT,
      CmpInst::Predicate::ICMP_UGT, // CGT_UN,
      CmpInst::Predicate::ICMP_SLT, // CLT,
      CmpInst::Predicate::ICMP_ULT  // CLT_UN,
  };

  static const CmpInst::Predicate FloatCmpMap[ReaderBaseNS::LastCmpOpcode] = {
      CmpInst::Predicate::FCMP_OEQ, // CEQ,
      CmpInst::Predicate::FCMP_OGT, // CGT,
      CmpInst::Predicate::FCMP_UGT, // CGT_UN,
      CmpInst::Predicate::FCMP_OLT, // CLT,
      CmpInst::Predicate::FCMP_ULT  // CLT_UN,
  };

  Value *Cmp;

  if (IsFloat) {
    Cmp = LLVMBuilder->CreateFCmp(FloatCmpMap[Opcode], Arg1, Arg2);
  } else {
    Cmp = LLVMBuilder->CreateICmp(IntCmpMap[Opcode], Arg1, Arg2);
  }

  IRNode *Result = convertToStackType((IRNode *)Cmp, CORINFO_TYPE_UINT);

  return Result;
}

void GenIR::boolBranch(ReaderBaseNS::BoolBranchOpcode Opcode, IRNode *Arg1) {
  static const CmpInst::Predicate
      BranchMap[ReaderBaseNS::LastBoolBranchOpcode] = {
          CmpInst::Predicate::ICMP_EQ, // BR_FALSE = 0,
          CmpInst::Predicate::ICMP_EQ, // BR_FALSE_S,
          CmpInst::Predicate::ICMP_NE, // BR_TRUE,
          CmpInst::Predicate::ICMP_NE  // BR_TRUE_S,
      };

  CmpInst::Predicate Predicate = BranchMap[Opcode];
  Constant *ZeroConst = Constant::getNullValue(Arg1->getType());
  Value *Condition = LLVMBuilder->CreateICmp(Predicate, Arg1, ZeroConst);

  // Patch up the branch instruction
  TerminatorInst *TermInst = LLVMBuilder->GetInsertBlock()->getTerminator();
  ASSERT(TermInst != nullptr);
  BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
  ASSERT(BranchInstruction != nullptr);
  BranchInstruction->setCondition(Condition);
}

void GenIR::condBranch(ReaderBaseNS::CondBranchOpcode Opcode, IRNode *Arg1,
                       IRNode *Arg2) {

  bool IsFloat = prepareArgsForCompare(Arg1, Arg2);

  static const CmpInst::Predicate
      IntBranchMap[ReaderBaseNS::LastCondBranchOpcode] = {
          CmpInst::Predicate::ICMP_EQ,  // BEQ,
          CmpInst::Predicate::ICMP_EQ,  // BEQ_S,
          CmpInst::Predicate::ICMP_SGE, // BGE,
          CmpInst::Predicate::ICMP_SGE, // BGE_S,
          CmpInst::Predicate::ICMP_UGE, // BGE_UN,
          CmpInst::Predicate::ICMP_UGE, // BGE_UN_S,
          CmpInst::Predicate::ICMP_SGT, // BGT,
          CmpInst::Predicate::ICMP_SGT, // BGT_S,
          CmpInst::Predicate::ICMP_UGT, // BGT_UN,
          CmpInst::Predicate::ICMP_UGT, // BGT_UN_S,
          CmpInst::Predicate::ICMP_SLE, // BLE,
          CmpInst::Predicate::ICMP_SLE, // BLE_S,
          CmpInst::Predicate::ICMP_ULE, // BLE_UN,
          CmpInst::Predicate::ICMP_ULE, // BLE_UN_S,
          CmpInst::Predicate::ICMP_SLT, // BLT,
          CmpInst::Predicate::ICMP_SLT, // BLT_S,
          CmpInst::Predicate::ICMP_ULT, // BLT_UN,
          CmpInst::Predicate::ICMP_ULT, // BLT_UN_S,
          CmpInst::Predicate::ICMP_NE,  // BNE_UN,
          CmpInst::Predicate::ICMP_NE   // BNE_UN_S,
      };

  static const CmpInst::Predicate
      FloatBranchMap[ReaderBaseNS::LastCondBranchOpcode] = {
          CmpInst::Predicate::FCMP_OEQ, // BEQ,
          CmpInst::Predicate::FCMP_OEQ, // BEQ_S,
          CmpInst::Predicate::FCMP_OGE, // BGE,
          CmpInst::Predicate::FCMP_OGE, // BGE_S,
          CmpInst::Predicate::FCMP_UGE, // BGE_UN,
          CmpInst::Predicate::FCMP_UGE, // BGE_UN_S,
          CmpInst::Predicate::FCMP_OGT, // BGT,
          CmpInst::Predicate::FCMP_OGT, // BGT_S,
          CmpInst::Predicate::FCMP_UGT, // BGT_UN,
          CmpInst::Predicate::FCMP_UGT, // BGT_UN_S,
          CmpInst::Predicate::FCMP_OLE, // BLE,
          CmpInst::Predicate::FCMP_OLE, // BLE_S,
          CmpInst::Predicate::FCMP_ULE, // BLE_UN,
          CmpInst::Predicate::FCMP_ULE, // BLE_UN_S,
          CmpInst::Predicate::FCMP_OLT, // BLT,
          CmpInst::Predicate::FCMP_OLT, // BLT_S,
          CmpInst::Predicate::FCMP_ULT, // BLT_UN,
          CmpInst::Predicate::FCMP_ULT, // BLT_UN_S,
          CmpInst::Predicate::FCMP_UNE, // BNE_UN,
          CmpInst::Predicate::FCMP_UNE  // BNE_UN_S,
      };

  Value *Condition =
      IsFloat ? LLVMBuilder->CreateFCmp(FloatBranchMap[Opcode], Arg1, Arg2)
              : LLVMBuilder->CreateICmp(IntBranchMap[Opcode], Arg1, Arg2);

  // Patch up the branch instruction
  TerminatorInst *TermInst = LLVMBuilder->GetInsertBlock()->getTerminator();
  ASSERT(TermInst != nullptr);
  BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
  ASSERT(BranchInstruction != nullptr);
  BranchInstruction->setCondition(Condition);
}

IRNode *GenIR::getStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(ResolvedToken, CORINFO_ACCESS_ADDRESS, &FieldInfo);
  IRNode *Address = rdrGetStaticFieldAddress(ResolvedToken, &FieldInfo);
  uint32_t Align;
  return getTypedAddress(Address, FieldInfo.fieldType, FieldInfo.structType,
                         Reader_AlignNatural, &Align);
}

IRNode *GenIR::shift(ReaderBaseNS::ShiftOpcode Opcode, IRNode *ShiftAmount,
                     IRNode *ShiftOperand) {
  Type *OperandType = ShiftOperand->getType();
  Type *AmountType = ShiftAmount->getType();

  // Unlike MSIL in which a shift operand may have different type and size than
  // that of a shift amount, LLVM requires that in a shift operation the shift
  // operand and the shift amount have to have the same type. Since negative
  // shift amounts have undefined behavior we can unilaterally treat the
  // amount as unsigned here.
  if (OperandType != AmountType) {
    ShiftAmount =
        (IRNode *)LLVMBuilder->CreateIntCast(ShiftAmount, OperandType, false);
  }

  // MSIL ReaderBaseNS::SHL lowered to LLVM Instruction::BinaryOps::Shl
  if (Opcode == ReaderBaseNS::Shl) {
    return (IRNode *)LLVMBuilder->CreateShl(ShiftOperand, ShiftAmount);
  }

  // MSIL ReaderBaseNS::SHR lowered to LLVM Instruction::BinaryOps::AShr
  if (Opcode == ReaderBaseNS::Shr) {
    return (IRNode *)LLVMBuilder->CreateAShr(ShiftOperand, ShiftAmount);
  }

  // MSIL ReaderBaseNS::SHR_UN lowered to LLVM Instruction::BinaryOps::LShr
  return (IRNode *)LLVMBuilder->CreateLShr(ShiftOperand, ShiftAmount);
}

/// Generate IR for MSIL Sizeof instruction.
IRNode *GenIR::sizeofOpcode(CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  uint32_t ClassSize = getClassSize(ResolvedToken->hClass);
  uint32_t NumBits = 32;
  bool IsSigned = false;
  IRNode *Value = (IRNode *)ConstantInt::get(
      *JitContext->LLVMContext, APInt(NumBits, ClassSize, IsSigned));
  IRNode *Result = convertToStackType(Value, CorInfoType::CORINFO_TYPE_UINT);

  return Result;
}

IRNode *GenIR::newObj(mdToken Token, mdToken LoadFtnToken,
                      uint32_t CurrOffset) {
  // Generate the constructor call
  // rdrCall and GenCall process newobj
  //  so there's nothing else to do.
  bool ReadOnlyPrefix = false;
  bool TailCallPrefix = false;
  bool IsUnmarkedTailCall = false;
  IRNode *Result =
      call(ReaderBaseNS::NewObj, Token, mdTokenNil, LoadFtnToken,
           ReadOnlyPrefix, TailCallPrefix, IsUnmarkedTailCall, CurrOffset);
  return Result;
}

IRNode *GenIR::newArr(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1) {
  CORINFO_CLASS_HANDLE ElementType;

  // The second argument to the helper is the number of elements in the array.
  // Create the second argument, the number of elements in the array.
  // This needs to be of type native int.
  IRNode *NumOfElements =
      convertToStackType(Arg1, CorInfoType::CORINFO_TYPE_NATIVEINT);

  Type *ArrayType =
      getType(CorInfoType::CORINFO_TYPE_CLASS, ResolvedToken->hClass);
  Value *Destination = Constant::getNullValue(ArrayType);

  const bool MayThrow = true;
  if (JitContext->Flags & CORJIT_FLG_READYTORUN) {
    return callReadyToRunHelper(CORINFO_HELP_READYTORUN_NEWARR_1, MayThrow,
                                (IRNode *)Destination, ResolvedToken,
                                NumOfElements);
  } else {
    // Or token with CORINFO_ANNOT_ARRAY so that we get back an array-type
    // handle.
    bool EmbedParent = false;
    bool MustRestoreHandle = true;
    IRNode *Token =
        genericTokenToNode(ResolvedToken, EmbedParent, MustRestoreHandle,
                           (CORINFO_GENERIC_HANDLE *)&ElementType, nullptr);

    return callHelper(getNewArrHelper(ElementType), MayThrow,
                      (IRNode *)Destination, Token, NumOfElements);
  }
}

// CastOp - Generates code for castclass or isinst.
IRNode *GenIR::castOp(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *ObjRefNode,
                      CorInfoHelpFunc HelperId) {
  CORINFO_CLASS_HANDLE Class = ResolvedToken->hClass;
  Type *ResultType = nullptr;
  if (JitContext->JitInfo->isValueClass(Class)) {
    ResultType = getBoxedType(Class);
  } else {
    ResultType = getType(CORINFO_TYPE_CLASS, Class);
  }

  const bool MayThrow = true;
  if (JitContext->Flags & CORJIT_FLG_READYTORUN) {
    return (IRNode *)callReadyToRunHelperImpl(HelperId, MayThrow, ResultType,
                                              ResolvedToken, ObjRefNode)
        .getInstruction();
  }

  CORINFO_GENERIC_HANDLE HandleType = nullptr;

  // Create the type node
  bool EmbedParent = false;
  bool MustRestoreHandle = false;

  IRNode *ClassHandleNode = genericTokenToNode(
      ResolvedToken, EmbedParent, MustRestoreHandle, &HandleType, nullptr);
  bool Optimize = false;
  if (!disableCastClassOptimization()) {
    switch (HelperId) {
    case CORINFO_HELP_CHKCASTCLASS:
      // TODO: It's not clear why calling this helper is safe. It assumes
      // that trivial checks are inlined. Check this again if we decide to
      // enable cast class optimization.
      HelperId = CORINFO_HELP_CHKCASTCLASS_SPECIAL;

    //
    // FALL-THROUGH
    //

    case CORINFO_HELP_ISINSTANCEOFCLASS: {
      uint32_t Flags = getClassAttribs((CORINFO_CLASS_HANDLE)HandleType);
      if ((Flags & CORINFO_FLG_FINAL) &&
          !(Flags & (CORINFO_FLG_MARSHAL_BYREF | CORINFO_FLG_CONTEXTFUL |
                     CORINFO_FLG_SHAREDINST))) {
        Optimize = true;
      }
    } break;

    default:
      break;
    }
  }

  // Generate the helper call or intrinsic
  const bool IsVolatile = false;
  const bool DoesNotInvokeStaticCtor = Optimize;
  return (IRNode *)callHelperImpl(HelperId, MayThrow, ResultType,
                                  ClassHandleNode, ObjRefNode, nullptr, nullptr,
                                  Reader_AlignUnknown, IsVolatile,
                                  DoesNotInvokeStaticCtor)
      .getInstruction();
}

// Override the cast class optimization
bool GenIR::disableCastClassOptimization() {
  // TODO: We may want to enable cast class optimization unless it's disabled
  // by some flags or we are generating debug code or it's causing problems
  // downstream.

  return true;
}

/// Optionally generate inline code for the \p abs opcode
///
/// \param Argument      input value for abs
/// \param Result [out]  resulting absolute value, if we decided to expand
/// \returns             true if Result represents the absolute value.
bool GenIR::abs(IRNode *Argument, IRNode **Result) {
  Type *Ty = Argument->getType();

  // Only the floating point cases of System.Math.Abs are implemented via
  // 'internallcall'.
  if (Ty->isFloatingPointTy()) {
    Type *Types[] = {Ty};
    Value *FAbs = Intrinsic::getDeclaration(JitContext->CurrentModule,
                                            Intrinsic::fabs, Types);
    bool MayThrow = false;
    Value *Abs = makeCall(FAbs, MayThrow, Argument).getInstruction();
    *Result = (IRNode *)Abs;
    return true;
  }

  return false;
}

bool GenIR::sqrt(IRNode *Argument, IRNode **Result) {
  Type *Ty = Argument->getType();

  if (Ty->isFloatingPointTy()) {
    Type *Types[] = {Ty};
    Value *FSqrt = Intrinsic::getDeclaration(JitContext->CurrentModule,
                                             Intrinsic::sqrt, Types);
    bool MayThrow = false;
    Value *Sqrt = makeCall(FSqrt, MayThrow, Argument).getInstruction();
    *Result = (IRNode *)Sqrt;
    return true;
  }

  return false;
}

IRNode *GenIR::localAlloc(IRNode *Arg, bool ZeroInit) {
  // We should have noticed this during the first pass.
  assert(HasLocAlloc && "need to detect localloc early");

  // Arg is the number of bytes to allocate. Result must be pointer-aligned.
  const unsigned int Alignment = TargetPointerSizeInBits / 8;
  LLVMContext &Context = *JitContext->LLVMContext;
  Type *Ty = Type::getInt8Ty(Context);
  AllocaInst *LocAlloc = createAlloca(Ty, Arg, "LocAlloc");
  LocAlloc->setAlignment(Alignment);

  // Zero the allocated region if so requested.
  if (ZeroInit) {
    zeroInitBlock(LocAlloc, Arg);
  }

  return (IRNode *)LocAlloc;
}

IRNode *GenIR::loadAndBox(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Addr,
                          ReaderAlignType Alignment) {
  // Get the type of the value being loaded
  CORINFO_CLASS_HANDLE ClassHandle = ResolvedToken->hClass;
  CorInfoType CorInfoType = ReaderBase::getClassType(ClassHandle);
  IRNode *Value = nullptr;
  const bool IsVolatile = false;
  const bool IsReadOnly = false;

  // Handle the various cases
  if (isPrimitiveType(CorInfoType)) {
    Value = GenIR::loadPrimitiveType(Addr, CorInfoType, Alignment, IsVolatile,
                                     IsReadOnly);
  } else if ((getClassAttribs(ClassHandle) & CORINFO_FLG_VALUECLASS)) {
    uint32_t Align;
    IRNode *UpdatedAddress =
        getTypedAddress(Addr, CorInfoType, ClassHandle, Alignment, &Align);
    Value = loadObj(ResolvedToken, UpdatedAddress, Alignment, IsVolatile,
                    IsReadOnly);
  } else {
    Value = GenIR::loadIndir(ReaderBaseNS::LdindRef, Addr, Alignment,
                             IsVolatile, IsReadOnly);
  }
  // TODO: Instead of loading the value above, just pass its address through
  // to the box helper (for primitives and value classes) and let it do the
  // load. Otherwise we end up with a redundant store/load in the path.
  return box(ResolvedToken, Value);
}

IRNode *GenIR::makeRefAny(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                          IRNode *Object) {
  // Create a new temporary of the right type.
  CORINFO_CLASS_HANDLE RefAnyHandle = getBuiltinClass(CLASSID_TYPED_BYREF);
  Type *RefAnyTy = getType(CORINFO_TYPE_VALUECLASS, RefAnyHandle);
  StructType *RefAnyStructTy = cast<StructType>(RefAnyTy);
  Value *RefAny = createTemporary(RefAnyTy, "RefAny");
  const bool IsVolatile = false;

  // Store the object in the value field. Object should either be an object
  // reference or the address of a local or param, both of which we type as
  // managed pointers.
  const unsigned ValueIndex = 0;
  const unsigned TypeIndex = 1;

  // Ecma-335 is clear that the object passed to mkrefany must be a managed
  // pointer type (or native int in unverifiable code). But traditionally .Net
  // jits have allowed arbitrary integers here too. So, tolerate this.
  Type *ExpectedObjectTy = RefAnyStructTy->getContainedType(ValueIndex);
  assert(GcInfo::isGcPointer(ExpectedObjectTy));
  Type *ActualObjectTy = Object->getType();
  Value *CastObject;
  if (GcInfo::isGcPointer(ActualObjectTy)) {
    CastObject = LLVMBuilder->CreatePointerCast(Object, ExpectedObjectTy);
  } else if (GcInfo::isUnmanagedPointer(ActualObjectTy)) {
    CastObject = LLVMBuilder->CreateAddrSpaceCast(Object, ExpectedObjectTy);
  } else {
    assert(Object->getType()->isIntegerTy());
    // Not clear what should happen on a size mismatch, so we'll just let
    // LLVM do what it thinks is reasonable.
    CastObject = LLVMBuilder->CreateIntToPtr(Object, ExpectedObjectTy);
  }
  Value *ValueFieldAddress =
      LLVMBuilder->CreateStructGEP(RefAnyTy, RefAny, ValueIndex);
  makeStoreNonNull(CastObject, ValueFieldAddress, IsVolatile);

  // Store the type handle in the type field.
  Value *TypeFieldAddress =
      LLVMBuilder->CreateStructGEP(RefAnyTy, RefAny, TypeIndex);
  Value *TypeHandle = genericTokenToNode(ResolvedToken);
  assert(
      (TypeHandle->getType() == RefAnyStructTy->getContainedType(TypeIndex)) &&
      "wrong type for refany type");
  makeStoreNonNull(TypeHandle, TypeFieldAddress, IsVolatile);

  // Load the refany as the result.
  setValueRepresentsStruct(RefAny);

  return (IRNode *)RefAny;
}

IRNode *GenIR::refAnyType(IRNode *RefAny) {
  CORINFO_CLASS_HANDLE RefAnyHandle = getBuiltinClass(CLASSID_TYPED_BYREF);
  Type *RefAnyTy = getType(CORINFO_TYPE_VALUECLASS, RefAnyHandle);

  assert(RefAny->getType()->isPointerTy());
  assert(RefAny->getType()->getPointerElementType() == RefAnyTy &&
         "refAnyType expects a RefAny as an argument");

  // Load the second field of the RefAny.
  const unsigned TypeIndex = 1;
  Value *TypeFieldAddress =
      LLVMBuilder->CreateStructGEP(RefAnyTy, RefAny, TypeIndex);
  const bool IsVolatile = false;
  Value *TypeValue = makeLoadNonNull(TypeFieldAddress, IsVolatile);

  // Convert the native TypeHandle to a RuntimeTypeHandle
  CORINFO_CLASS_HANDLE RuntimeTypeHandle =
      getBuiltinClass(CorInfoClassId::CLASSID_TYPE_HANDLE);
  return convertHandle((IRNode *)TypeValue,
                       CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE_MAYBENULL,
                       RuntimeTypeHandle);
}

#pragma endregion

#pragma region STACK MAINTENANCE

//===----------------------------------------------------------------------===//
//
// MSIL READER Stack maintenance operations
//
//===----------------------------------------------------------------------===//

// Since we never place references to aliasable values onto
// the operand stack the two RemoveStackInterference* methods don't
// need to do anything.
void GenIR::removeStackInterference() { return; }

void GenIR::removeStackInterferenceForLocalStore(uint32_t Opcode,
                                                 uint32_t Ordinal) {
  return;
}

void GenIR::maintainOperandStack(FlowGraphNode *CurrentBlock) {

  if (ReaderOperandStack->size() == 0) {
    return;
  }

  FlowGraphEdgeIterator SuccessorIterator =
      fgNodeGetSuccessorsActual(CurrentBlock);
  bool Done = SuccessorIterator.isEnd();

  if (Done) {
    clearStack();
    return;
  }

  while (!Done) {
    FlowGraphNode *SuccessorBlock = fgEdgeIteratorGetSink(SuccessorIterator);

    if (!fgNodeHasMultiplePredsPropagatingStack(SuccessorBlock)) {
      // We need to create a stack for the Successor and copy the items from the
      // current stack.
      if (!fgNodePropagatesOperandStack(SuccessorBlock)) {
        // This successor block doesn't need a stack. This is a common case for
        // implicit exception throw blocks or conditional helper calls.
      } else {
        // The current node is the only relevant predecessor of this Successor.
        if (fgNodePropagatesOperandStack(CurrentBlock)) {
          fgNodeSetOperandStack(SuccessorBlock, ReaderOperandStack->copy());
        } else {
          // The successor block starts with empty stack.
          assert(fgNodeHasNoPredsPropagatingStack(SuccessorBlock));
          fgNodeSetOperandStack(SuccessorBlock, createStack());
        }
      }
    } else {
      ReaderStack *SuccessorStack = fgNodeGetOperandStack(SuccessorBlock);
      bool CreatePHIs = false;
      if (SuccessorStack == nullptr) {
        // We need to create a new stack for the Successor and populate it
        // with PHI instructions corresponding to the values on the current
        // stack.
        SuccessorStack = createStack();
        fgNodeSetOperandStack(SuccessorBlock, SuccessorStack);
        CreatePHIs = true;
      }

// We need to be very careful about reasoning about or iterating through
// instructions in empty blocks or blocks with no terminators.
#ifndef NDEBUG
      Instruction *TermInst = SuccessorBlock->getTerminator();
      const bool SuccessorDegenerate = (TermInst == nullptr);
#endif
      Instruction *CurrentInst =
          SuccessorBlock->empty() ? nullptr : &*SuccessorBlock->begin();
      PHINode *Phi = nullptr;
      for (IRNode *Current : *ReaderOperandStack) {
        Value *CurrentValue = (Value *)Current;
        if (CreatePHIs) {
          // The Successor has at least 2 predecessors so we use 2 as the
          // hint for the number of PHI sources.
          // TODO: Could be nice to have actual pred. count here instead, but
          // there's no simple way of fetching that, AFAICT.
          Phi = createPHINode(SuccessorBlock, CurrentValue->getType(), 2, "");
          if (doesValueRepresentStruct(CurrentValue)) {
            setValueRepresentsStruct(Phi);
          }

          // Preemptively add all predecessors to the PHI node to ensure
          // that we don't forget any once we're done.
          FlowGraphEdgeIterator PredecessorIterator =
              fgNodeGetPredecessorsActual(SuccessorBlock);
          while (!PredecessorIterator.isEnd()) {
            Phi->addIncoming(UndefValue::get(CurrentValue->getType()),
                             fgEdgeIteratorGetSource(PredecessorIterator));
            fgEdgeIteratorMoveNextPredecessorActual(PredecessorIterator);
          }
        } else {
          // PHI instructions should have been inserted already.
          assert(CurrentInst != nullptr);
          assert(isa<PHINode>(CurrentInst));
          Phi = cast<PHINode>(CurrentInst);
          CurrentInst = CurrentInst->getNextNode();
        }
        addPHIOperand(Phi, CurrentValue, (BasicBlock *)CurrentBlock);
        if (CreatePHIs) {
          SuccessorStack->push((IRNode *)Phi);
        }
      }

      // The number of PHI instructions should match the number of values on the
      // stack, so if we're not creating PHIs, try and verify that the next
      // instruction is not a PHI.
      //
      // Note when SuccessorBlock is degenerate we can't be sure CurrentInst is
      // valid, so we can't do this check.
      assert(CreatePHIs || SuccessorDegenerate || !isa<PHINode>(CurrentInst));
    }

    fgEdgeIteratorMoveNextSuccessorActual(SuccessorIterator);
    Done = fgEdgeIteratorIsEnd(SuccessorIterator);
  }

  clearStack();
}

void GenIR::addPHIOperand(PHINode *Phi, Value *NewOperand,
                          BasicBlock *NewBlock) {
  Type *PHITy = Phi->getType();
  Type *NewOperandTy = NewOperand->getType();

  if (PHITy != NewOperandTy) {
    bool IsStructPHITy = doesValueRepresentStruct(Phi);
    bool IsStructNewOperandTy = doesValueRepresentStruct(NewOperand);
    Type *NewPHITy = getStackMergeType(PHITy, NewOperandTy, IsStructPHITy,
                                       IsStructNewOperandTy);
    IRBuilder<>::InsertPoint SavedInsertPoint = LLVMBuilder->saveIP();
    if (NewPHITy != PHITy) {
      // Change the type of the PHI instruction and the types of all of its
      // operands.
      Phi->mutateType(NewPHITy);
      for (unsigned I = 0; I < Phi->getNumOperands(); ++I) {
        Value *Operand = Phi->getIncomingValue(I);
        if (!isa<UndefValue>(Operand)) {
          BasicBlock *OperandBlock = Phi->getIncomingBlock(I);
          Operand = changePHIOperandType(Operand, OperandBlock, NewPHITy);
          Phi->setIncomingValue(I, Operand);
        }
      }
    }
    if (NewPHITy != NewOperandTy) {
      // Change the type of the new PHI operand.
      NewOperand = changePHIOperandType(NewOperand, NewBlock, NewPHITy);
    }
    LLVMBuilder->restoreIP(SavedInsertPoint);
  }

  bool FoundBlockOperand = false;
  for (unsigned I = 0, N = Phi->getNumOperands(); I != N; ++I) {
    if (Phi->block_begin()[I] == NewBlock) {
      Value *CurrentOperand = Phi->getIncomingValue(I);
      if (isa<UndefValue>(CurrentOperand)) {
        Phi->setIncomingValue(I, NewOperand);
        FoundBlockOperand = true;
        break;
      } else {
        assert(CurrentOperand == NewOperand);
      }
    }
  }
  assert(FoundBlockOperand);
}

Value *GenIR::changePHIOperandType(Value *Operand, BasicBlock *OperandBlock,
                                   Type *NewTy) {
  LLVMBuilder->SetInsertPoint(OperandBlock->getTerminator());
  if (NewTy->isIntegerTy()) {
    Type *OperandTy = Operand->getType();
    if (OperandTy->isIntegerTy()) {
      bool IsSigned = true;
      return LLVMBuilder->CreateIntCast(Operand, NewTy, IsSigned);
    } else {
      assert(GcInfo::isUnmanagedPointer(OperandTy));
      return LLVMBuilder->CreatePtrToInt(Operand, NewTy);
    }
  } else if (NewTy->isFloatingPointTy()) {
    return LLVMBuilder->CreateFPCast(Operand, NewTy);
  } else {
    return LLVMBuilder->CreatePointerCast(Operand, NewTy);
  }
}

Type *GenIR::getStackMergeType(Type *Ty1, Type *Ty2, bool IsStruct1,
                               bool IsStruct2) {
  if (Ty1 == Ty2) {
    return Ty1;
  }

  assert(IsStruct1 == IsStruct2);

  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;

  // If we have nativeint and int32 the result is the first type.
  Type *NativeIntTy = Type::getIntNTy(LLVMContext, TargetPointerSizeInBits);
  Type *Int32Ty = Type::getInt32Ty(LLVMContext);
  if (((Ty1 == NativeIntTy) && (Ty2 == Int32Ty)) ||
      ((Ty2 == NativeIntTy) && (Ty1 == Int32Ty))) {
    return Ty1;
  }

  // If we have float and double the result is double.
  Type *FloatTy = Type::getFloatTy(LLVMContext);
  Type *DoubleTy = Type::getDoubleTy(LLVMContext);
  if (((Ty1 == FloatTy) && (Ty2 == DoubleTy)) ||
      ((Ty2 == FloatTy) && (Ty1 == DoubleTy))) {
    return DoubleTy;
  }

  // If we have unmanaged pointer and nativeint the result is nativeint.
  if ((GcInfo::isUnmanagedPointer(Ty1) && (Ty2 == NativeIntTy)) ||
      (GcInfo::isUnmanagedPointer(Ty2) && (Ty1 == NativeIntTy))) {
    return NativeIntTy;
  }

  // If we have GC pointers, the result is the closest common supertype.
  PointerType *PointerTy1 = dyn_cast<PointerType>(Ty1);
  PointerType *PointerTy2 = dyn_cast<PointerType>(Ty2);
  if ((PointerTy1 != nullptr) && (PointerTy2 != nullptr) &&
      (GcInfo::isGcPointer(PointerTy1)) && (GcInfo::isGcPointer(PointerTy2))) {

    CORINFO_CLASS_HANDLE Class1 = nullptr;
    auto MapElement1 = ReverseClassTypeMap->find(PointerTy1);
    if (MapElement1 != ReverseClassTypeMap->end()) {
      Class1 = MapElement1->second;
    }

    CORINFO_CLASS_HANDLE Class2 = nullptr;
    auto MapElement2 = ReverseClassTypeMap->find(PointerTy2);
    if (MapElement2 != ReverseClassTypeMap->end()) {
      Class2 = MapElement2->second;
    }

    CORINFO_CLASS_HANDLE MergedClass = nullptr;
    if ((Class1 != nullptr) && (Class2 != nullptr)) {
      MergedClass = JitContext->JitInfo->mergeClasses(Class1, Class2);
      ASSERT(!JitContext->JitInfo->isValueClass(MergedClass));
    } else {
      // We can get here if one of the types is an array or a boxed type.
      // We can't map arrays back to its handles because an array can be
      // identified by one of two handles: the actual array handle and the
      // handle for its MethodTable. mergeClasses will only work correctly with
      // the former.
      // Use System.Object as the result for these cases.
      MergedClass = getBuiltinClass(CorInfoClassId::CLASSID_SYSTEM_OBJECT);
    }
    return getType(CORINFO_TYPE_CLASS, MergedClass);
  }

  if (IsStruct1 && IsStruct2) {
    // We can have mismatching struct types due to generic sharing.
    // Verify that the struct layouts match.
    StructType *StructTy1 = cast<StructType>(Ty1->getPointerElementType());
    StructType *StructTy2 = cast<StructType>(Ty2->getPointerElementType());
    if (StructTy1->isLayoutIdentical(StructTy2)) {
      // Arbitrarily pick Ty1 as the resulting type.
      return Ty1;
    }
  }

  // If we have (pointers to) two function types, if one or the other is not the
  // default `void()` placeholder, use it as the resulting type.
  Type *ReferentType1 = PointerTy1->getPointerElementType();
  Type *ReferentType2 = PointerTy2->getPointerElementType();
  if (isa<FunctionType>(ReferentType1) && isa<FunctionType>(ReferentType2)) {
    Type *VoidType = Type::getVoidTy(LLVMContext);
    FunctionType *PlaceholderType = FunctionType::get(VoidType, false);
    if (ReferentType1 != PlaceholderType) {
      return Ty1;
    } else {
      return Ty2;
    }
  }

  ASSERT(UNREACHED);
  return nullptr;
}

// Create a PHI node in a block that may or may not have a terminator.
PHINode *GenIR::createPHINode(BasicBlock *Block, Type *Ty,
                              unsigned int NumReservedValues,
                              const Twine &NameStr) {
  // Put this new PHI after any existing PHIs but before anything else.
  BasicBlock::iterator I = Block->begin();
  BasicBlock::iterator IE = Block->end();
  while ((I != IE) && isa<PHINode>(I)) {
    ++I;
  }

  PHINode *Result;
  if (I == IE) {
    Result = PHINode::Create(Ty, NumReservedValues, NameStr, Block);
  } else {
    Result = PHINode::Create(Ty, NumReservedValues, NameStr, &*I);
  }

  return Result;
}

// Check whether the node is constant null.
bool GenIR::isConstantNull(IRNode *Node) {
  Constant *ConstantValue = dyn_cast<Constant>(Node);
  return (ConstantValue != nullptr) && ConstantValue->isNullValue();
}

#pragma endregion

#pragma region VERIFIER

//===----------------------------------------------------------------------===//
//
// MSIL READER Verifier Support
//
//===----------------------------------------------------------------------===//

// In the watch window of the debugger, type tiVarName.ToStaticString()
// to view a string representation of this instance.

void TypeInfo::dump() const {
  TITypes TiType = getType();
  if (TiType == TI_Ref || TiType == TI_Struct) {
    dbgs() << "< " << toStaticString() << " m:" << Method
           << " isbyref:" << isByRef() << " isreadonly:" << isReadonlyByRef()
           << " isthis:" << isThisPtr()
           << " isvar:" << ((Flags & TI_FLAG_GENERIC_TYPE_VAR) != 0) << " >";
  } else {
    dbgs() << "< " << toStaticString() << " >";
  }
}

std::string TypeInfo::toStaticString() const {
  assertx(TI_Count <= TI_FLAG_DATA_MASK);

  if (isMethod()) {
    return "method";
  }

  std::string Storage;
  raw_string_ostream OS(Storage);

  if (isByRef())
    OS << "&";

  if (isNullObjRef())
    OS << "nullref";

  if (isUninitialisedObjRef())
    OS << "<uninit>";

  if (isPermanentHomeByRef())
    OS << "<permanent home>";

  if (isThisPtr())
    OS << "<this>";

  if (Flags & TI_FLAG_GENERIC_TYPE_VAR)
    OS << "<generic>";

  TITypes TiType = getRawType();

#if 0
  if (hasByRefLocalInfo())
    OS << format("(local %d)", getByRefLocalInfo());

  if (hasByRefFieldInfo())
    OS << format("(field %d)", getByRefFieldInfo());
#endif

  OS << " ";
  switch (TiType) {
  default:
    OS << "<<internal error>>";
    break;

  case TI_Byte:
    OS << "byte";
    break;

  case TI_Short:
    OS << "short";
    break;

  case TI_Int:
    OS << "int";
    break;

  case TI_Long:
    OS << "long";
    break;

  case TI_I:
    OS << "native int";
    break;

  case TI_Float:
    OS << "float";
    break;

  case TI_Double:
    OS << "double";
    break;

  case TI_Ref:
    OS << "ref";
    break;

  case TI_Struct:
    OS << "struct";
    break;

  case TI_Ptr:
    OS << "pointer";
    break;

  case TI_Error:
    OS << "error";
    break;
  }

  return Storage;
}

#ifdef DEBUG
void VerificationState::print() {
  int32_t I;
  dbgs() << "--verification stack---\n";
  for (I = Vsp - 1; I >= 0; I--) {
    dbgs() << I << ": ";
    Vstack[I].dump();
    dbgs() << "\n";
  }
}
#endif

#pragma endregion

#pragma region SIMD_INTRISNICS

//===----------------------------------------------------------------------===//
//
// SIMD Intrinsics
//
//===----------------------------------------------------------------------===//

// BinOperations

IRNode *GenIR::vectorAdd(IRNode *Vector1, IRNode *Vector2) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    return (IRNode *)LLVMBuilder->CreateFAdd(Vector1, Vector2);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateAdd(Vector1, Vector2);
  } else {
    return 0;
  }
}

IRNode *GenIR::vectorSub(IRNode *Vector1, IRNode *Vector2) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    return (IRNode *)LLVMBuilder->CreateFSub(Vector1, Vector2);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateSub(Vector1, Vector2);
  } else {
    return 0;
  }
}

IRNode *GenIR::vectorMul(IRNode *Vector1, IRNode *Vector2) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    return (IRNode *)LLVMBuilder->CreateFMul(Vector1, Vector2);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateMul(Vector1, Vector2);
  } else {
    return 0;
  }
}

IRNode *GenIR::vectorDiv(IRNode *Vector1, IRNode *Vector2, bool IsSigned) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    return (IRNode *)LLVMBuilder->CreateFDiv(Vector1, Vector2);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    if (IsSigned) {
      return (IRNode *)LLVMBuilder->CreateSDiv(Vector1, Vector2);
    } else {
      return (IRNode *)LLVMBuilder->CreateUDiv(Vector1, Vector2);
    }
  } else {
    return 0;
  }
}

IRNode *GenIR::vectorEqual(IRNode *Vector1, IRNode *Vector2) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    return (IRNode *)LLVMBuilder->CreateFCmpOEQ(Vector1, Vector2);
  }
  if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateICmpEQ(Vector1, Vector2);
  }
  return 0;
}

IRNode *GenIR::vectorNotEqual(IRNode *Vector1, IRNode *Vector2) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    return (IRNode *)LLVMBuilder->CreateFCmpONE(Vector1, Vector2);
  }
  if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateICmpNE(Vector1, Vector2);
  }
  return 0;
}

IRNode *GenIR::vectorMax(IRNode *Vector1, IRNode *Vector2, bool IsSigned) {
  assert(Vector2->getType() == Vector1->getType());
  IRNode *CompareRes = 0;
  Type *VectorElementType = Vector1->getType()->getVectorElementType();
  if (VectorElementType->isFloatingPointTy()) {
    CompareRes = (IRNode *)LLVMBuilder->CreateFCmpOGE(Vector1, Vector2);
  } else if (VectorElementType->isIntegerTy()) {
    if (IsSigned) {
      CompareRes = (IRNode *)LLVMBuilder->CreateICmpSGE(Vector1, Vector2);
    } else {
      CompareRes = (IRNode *)LLVMBuilder->CreateICmpUGE(Vector1, Vector2);
    }
  }
  return (IRNode *)LLVMBuilder->CreateSelect(CompareRes, Vector1, Vector2);
}

IRNode *GenIR::vectorMin(IRNode *Vector1, IRNode *Vector2, bool IsSigned) {
  assert(Vector2->getType() == Vector1->getType());
  IRNode *CompareRes = 0;
  Type *VectorElementType = Vector1->getType()->getVectorElementType();
  if (VectorElementType->isFloatingPointTy()) {
    CompareRes = (IRNode *)LLVMBuilder->CreateFCmpOGE(Vector1, Vector2);
  } else if (VectorElementType->isIntegerTy()) {
    if (IsSigned) {
      CompareRes = (IRNode *)LLVMBuilder->CreateICmpSGE(Vector1, Vector2);
    } else {
      CompareRes = (IRNode *)LLVMBuilder->CreateICmpUGE(Vector1, Vector2);
    }
  }
  return (IRNode *)LLVMBuilder->CreateSelect(CompareRes, Vector2, Vector1);
}

Type *GenIR::getVectorIntType(unsigned VectorByteSize) {
  LLVMContext &Context = LLVMBuilder->getContext();
  Type *IntType = llvm::Type::getInt32Ty(Context);
  return llvm::VectorType::get(IntType, VectorByteSize * 8 /
                                            IntType->getPrimitiveSizeInBits());
}

IRNode *GenIR::vectorBitOr(IRNode *Vector1, IRNode *Vector2,
                           unsigned VectorByteSize) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    Type *ResultType = Vector1->getType();
    Type *VectorIntType = getVectorIntType(VectorByteSize);
    Vector1 = (IRNode *)LLVMBuilder->CreateBitCast(Vector1, VectorIntType);
    Vector2 = (IRNode *)LLVMBuilder->CreateBitCast(Vector2, VectorIntType);
    IRNode *IntResult = (IRNode *)LLVMBuilder->CreateOr(Vector1, Vector2);
    return (IRNode *)LLVMBuilder->CreateBitCast(IntResult, ResultType);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateOr(Vector1, Vector2);
  }
  return 0;
}

IRNode *GenIR::vectorBitAnd(IRNode *Vector1, IRNode *Vector2,
                            unsigned VectorByteSize) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    Type *ResultType = Vector1->getType();
    Type *VectorIntType = getVectorIntType(VectorByteSize);
    Vector1 = (IRNode *)LLVMBuilder->CreateBitCast(Vector1, VectorIntType);
    Vector2 = (IRNode *)LLVMBuilder->CreateBitCast(Vector2, VectorIntType);
    IRNode *IntResult = (IRNode *)LLVMBuilder->CreateAnd(Vector1, Vector2);
    return (IRNode *)LLVMBuilder->CreateBitCast(IntResult, ResultType);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateAnd(Vector1, Vector2);
  }
  return 0;
}

IRNode *GenIR::vectorBitExOr(IRNode *Vector1, IRNode *Vector2,
                             unsigned VectorByteSize) {
  assert(Vector2->getType() == Vector1->getType());
  if (Vector1->getType()->getVectorElementType()->isFloatingPointTy()) {
    Type *ResultType = Vector1->getType();
    Type *VectorIntType = getVectorIntType(VectorByteSize);
    Vector1 = (IRNode *)LLVMBuilder->CreateBitCast(Vector1, VectorIntType);
    Vector2 = (IRNode *)LLVMBuilder->CreateBitCast(Vector2, VectorIntType);
    IRNode *IntResult = (IRNode *)LLVMBuilder->CreateXor(Vector1, Vector2);
    return (IRNode *)LLVMBuilder->CreateBitCast(IntResult, ResultType);
  } else if (Vector1->getType()->getVectorElementType()->isIntegerTy()) {
    return (IRNode *)LLVMBuilder->CreateXor(Vector1, Vector2);
  }
  return 0;
}

IRNode *GenIR::vectorAbs(IRNode *Vector) {
  if (Vector->getType()->getVectorElementType()->isFloatingPointTy()) {
    std::vector<Type *> Types;
    Types.push_back(Vector->getType());
    llvm::Function *Func = 0;
    Func = Intrinsic::getDeclaration(JitContext->CurrentModule, Intrinsic::fabs,
                                     Types);
    return (IRNode *)LLVMBuilder->CreateCall(Func, Vector);
  }
  return 0;
}

IRNode *GenIR::vectorSqrt(IRNode *Vector) {
  if (Vector->getType()->getVectorElementType()->isFloatingPointTy()) {
    std::vector<Type *> Types;
    Types.push_back(Vector->getType());
    llvm::Function *Func = 0;
    Func = Intrinsic::getDeclaration(JitContext->CurrentModule, Intrinsic::sqrt,
                                     Types);
    return (IRNode *)LLVMBuilder->CreateCall(Func, Vector);
  }
  return 0;
}

IRNode *GenIR::generateIsHardwareAccelerated(CORINFO_CLASS_HANDLE Class) {
  return (IRNode *)ConstantInt::get(Type::getInt32Ty(LLVMBuilder->getContext()),
                                    1);
}

bool GenIR::checkVectorSignature(std::vector<IRNode *> Args,
                                 std::vector<Type *> Types) {
  assert(Args.size() == Types.size());
  for (unsigned int Counter = 0; Counter < Args.size(); ++Counter) {
    assert(Args[Counter]);
    if (Args[Counter]->getType() != Types[Counter]) {
      assert(UNREACHED);
      return 0;
    }
  }
  return 1;
}

IRNode *GenIR::vectorFixType(IRNode *Arg, Type *DstType) {
  Type *SrcType = Arg->getType();
  if (SrcType->isIntegerTy()) {
    Arg = (IRNode *)LLVMBuilder->CreateZExtOrTrunc(Arg, DstType);
  } else if (SrcType->isFloatingPointTy()) {
    Arg = (IRNode *)LLVMBuilder->CreateFPCast(Arg, DstType);
  }
  return 0;
}

IRNode *GenIR::vectorCtorFromOne(int VectorSize, IRNode *Vector,
                                 std::vector<IRNode *> Args) {
  assert(Args.size() == 1);
  for (int Counter = 0; Counter < VectorSize; ++Counter) {
    IRNode *Arg = Args[0];
    if (Arg->getType() != Vector->getType()->getVectorElementType()) {
      Arg = vectorFixType(Arg, Vector->getType()->getVectorElementType());
      if (!Arg) {
        return 0;
      }
    }
    Vector = (IRNode *)LLVMBuilder->CreateInsertElement(Vector, Arg, Counter);
  }
  return Vector;
}

IRNode *GenIR::vectorCtorFromFloats(int VectorSize, IRNode *Vector,
                                    std::vector<IRNode *> Args) {
  assert((int)Args.size() == VectorSize);
  std::vector<Type *> Types;
  llvm::LLVMContext &LLVMContext = *JitContext->LLVMContext;
  Type *FloatTy = Type::getFloatTy(LLVMContext);
  for (int Counter = 0; Counter < VectorSize; ++Counter) {
    Types.push_back(FloatTy);
  }
  if (checkVectorSignature(Args, Types)) {
    for (int Counter = 0; Counter < VectorSize; ++Counter) {
      Vector = (IRNode *)LLVMBuilder->CreateInsertElement(Vector, Args[Counter],
                                                          Counter);
    }
    return Vector;
  }
  return 0;
}

IRNode *GenIR::vectorCtorFromArray(int VectorSize, IRNode *Vector,
                                   IRNode *Array, IRNode *Index) {
  llvm::LLVMContext &Context = *JitContext->LLVMContext;
  Type *IntTy = Type::getInt32Ty(Context);
  for (int Counter = 0; Counter < VectorSize; ++Counter) {
    IRNode *ExtractedArg =
        (IRNode *)LLVMBuilder->CreateLoad(genArrayElemAddress(
            Array, Index, Vector->getType()->getVectorElementType()));
    Vector = (IRNode *)LLVMBuilder->CreateInsertElement(Vector, ExtractedArg,
                                                        Counter);
    Index = (IRNode *)LLVMBuilder->CreateAdd(Index, ConstantInt::get(IntTy, 1));
  }
  return Vector;
}

IRNode *GenIR::vectorCtorFromPointer(int VectorSize, IRNode *Vector,
                                     IRNode *Pointer, IRNode *Index) {
  llvm::LLVMContext &Context = *JitContext->LLVMContext;
  Type *IntTy = Type::getInt32Ty(Context);
  Type *VectorElType = Vector->getType()->getVectorElementType();
  Pointer = (IRNode *)LLVMBuilder->CreatePointerCast(
      Pointer, llvm::PointerType::get(VectorElType, 0));
  for (int Counter = 0; Counter < VectorSize; ++Counter) {
    IRNode *ExtractedArg = (IRNode *)LLVMBuilder->CreateGEP(Pointer, Index);
    ExtractedArg = (IRNode *)LLVMBuilder->CreateLoad(ExtractedArg);
    Vector = (IRNode *)LLVMBuilder->CreateInsertElement(Vector, ExtractedArg,
                                                        Counter);
    Index = (IRNode *)LLVMBuilder->CreateAdd(Index, ConstantInt::get(IntTy, 1));
  }
  return Vector;
}

IRNode *GenIR::vectorCtor(CORINFO_CLASS_HANDLE Class, IRNode *This,
                          std::vector<IRNode *> Args) {
  int VectorSize = 0;
  bool IsGeneric = false;
  bool IsSigned = false;
  Type *ElementType =
      getBaseTypeAndSizeOfSIMDType(Class, VectorSize, IsGeneric, IsSigned);
  if (VectorSize == 0) { // For example Vector<bool>.
    return 0;
  }
  Type *VectorType = llvm::VectorType::get(ElementType, VectorSize);
  IRNode *Vector = (IRNode *)UndefValue::get(VectorType);
  llvm::LLVMContext &LLVMContext = *JitContext->LLVMContext;
  Type *FloatTy = Type::getFloatTy(LLVMContext);
  IRNode *Return = 0;
  if (!IsGeneric) {
    if (Args.size() == 1) {
      if (Args[0]->getType() == ElementType) {
        Return = vectorCtorFromOne(VectorSize, Vector, Args);
      } else {
        return 0;
      }
    } else if ((int)Args.size() == VectorSize) {
      Return = vectorCtorFromFloats(VectorSize, Vector, Args);
    } else {
      std::vector<Type *> Types;
      std::vector<IRNode *> ExtractedArgs;
      llvm::LLVMContext &Context = *JitContext->LLVMContext;
      Type *IntTy = Type::getInt32Ty(Context);
      IRNode *Index0 = (IRNode *)ConstantInt::get(IntTy, 0);
      IRNode *Index1 = (IRNode *)ConstantInt::get(IntTy, 1);
      IRNode *Index2 = (IRNode *)ConstantInt::get(IntTy, 2);
#pragma region non - generic ctors
      switch (VectorSize) {
      case 3:
        assert(Args.size() == 2);
        Types.push_back(VectorType::get(FloatTy, 2));
        Types.push_back(FloatTy);
        if (checkVectorSignature(Args, Types)) {
          IRNode *Vector2 = Args[0];
          ExtractedArgs.push_back(
              (IRNode *)LLVMBuilder->CreateExtractElement(Vector2, Index0));
          ExtractedArgs.push_back(
              (IRNode *)LLVMBuilder->CreateExtractElement(Vector2, Index1));
          ExtractedArgs.push_back(Args[1]);
        }
        break;

      case 4:
        if (Args.size() == 2) {
          Types.push_back(VectorType::get(FloatTy, 3));
          Types.push_back(FloatTy);
          if (checkVectorSignature(Args, Types)) {
            IRNode *Vector3 = Args[0];
            ExtractedArgs.push_back(
                (IRNode *)LLVMBuilder->CreateExtractElement(Vector3, Index0));
            ExtractedArgs.push_back(
                (IRNode *)LLVMBuilder->CreateExtractElement(Vector3, Index1));
            ExtractedArgs.push_back(
                (IRNode *)LLVMBuilder->CreateExtractElement(Vector3, Index2));
            ExtractedArgs.push_back(Args[1]);
          }
        } else {
          assert(Args.size() == 3);
          Types.push_back(VectorType::get(FloatTy, 2));
          Types.push_back(FloatTy);
          Types.push_back(FloatTy);
          if (checkVectorSignature(Args, Types)) {
            IRNode *Vector2 = Args[0];
            ExtractedArgs.push_back(
                (IRNode *)LLVMBuilder->CreateExtractElement(Vector2, Index0));
            ExtractedArgs.push_back(
                (IRNode *)LLVMBuilder->CreateExtractElement(Vector2, Index1));
            ExtractedArgs.push_back(Args[1]);
            ExtractedArgs.push_back(Args[2]);
          }
        }
        break;

      default:
        assert(UNREACHED);
      }
      if (ExtractedArgs.size()) {
        for (int Counter = 0; Counter < VectorSize; ++Counter) {
          Vector = (IRNode *)LLVMBuilder->CreateInsertElement(
              Vector, ExtractedArgs[Counter], Counter);
        }
        Return = Vector;
      }
    }
#pragma endregion
  } else {
    if (Args.size() == 1) {
      if (!Args[0]->getType()->isPointerTy()) {
        Return = vectorCtorFromOne(VectorSize, Vector, Args);
      } else {
        return 0;
      }
    } else if (Args.size() == 2) {
      Type *FirstArgType = Args[0]->getType();
      if (FirstArgType->isPointerTy()) {
        if (FirstArgType->getPointerElementType()->isAggregateType()) {
          Return = vectorCtorFromArray(VectorSize, Vector, Args[0], Args[1]);
        } else {
          Return = vectorCtorFromPointer(VectorSize, Vector, Args[0], Args[1]);
        }
      }
    }
  }
  if (Return) {
    if (This) {
      return (IRNode *)LLVMBuilder->CreateStore(Return, This);
    } else {
      return Return;
    }
  }
  return 0;
}

bool GenIR::isVectorType(IRNode *Arg) {
  assert(Arg);
  return Arg->getType()->isVectorTy();
}

unsigned GenIR::getMaxIntrinsicSIMDVectorLength(CORINFO_CLASS_HANDLE Class) {
  unsigned Result = JitContext->Options->PreferredIntrinsicSIMDVectorLength;
  if (!Result) {
    Result = getClassSize(Class);
  }
  return Result;
}

llvm::Type *GenIR::getBaseTypeAndSizeOfSIMDType(CORINFO_CLASS_HANDLE Class,
                                                int &VectorLength,
                                                bool &IsGeneric,
                                                bool &IsSigned) {
  // TODO t-seand : issue #720, check thread safety.
  static CORINFO_CLASS_HANDLE SIMDFloatHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDDoubleHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDIntHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDUShortHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDUByteHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDShortHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDByteHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDLongHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDUIntHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDULongHandle = 0;
  static CORINFO_CLASS_HANDLE SIMDVector2Handle = 0;
  static CORINFO_CLASS_HANDLE SIMDVector3Handle = 0;
  static CORINFO_CLASS_HANDLE SIMDVector4Handle = 0;

  LLVMContext &Context = *JitContext->LLVMContext;

  VectorLength = 0;
  IsGeneric = false;
  IsSigned = true;
  Type *Result = 0;

  if (Class == SIMDFloatHandle) {
    Result = Type::getFloatTy(Context);
  } else if (Class == SIMDDoubleHandle) {
    Result = Type::getDoubleTy(Context);
  } else if (Class == SIMDIntHandle) {
    Result = Type::getInt32Ty(Context);
  } else if (Class == SIMDUShortHandle) {
    IsSigned = false;
    Result = Type::getInt16Ty(Context);
  } else if (Class == SIMDUByteHandle) {
    IsSigned = false;
    Result = Type::getInt8Ty(Context);
  } else if (Class == SIMDShortHandle) {
    Result = Type::getInt16Ty(Context);
  } else if (Class == SIMDByteHandle) {
    Result = Type::getInt8Ty(Context);
  } else if (Class == SIMDLongHandle) {
    Result = Type::getInt64Ty(Context);
  } else if (Class == SIMDUIntHandle) {
    IsSigned = false;
    Result = Type::getInt32Ty(Context);
  } else if (Class == SIMDULongHandle) {
    IsSigned = false;
    Result = Type::getInt64Ty(Context);
  } else if (Class == SIMDVector2Handle) {
    VectorLength = 2;
    Result = Type::getFloatTy(Context);
  } else if (Class == SIMDVector3Handle) {
    VectorLength = 3;
    Result = Type::getFloatTy(Context);
  } else if (Class == SIMDVector4Handle) {
    VectorLength = 4;
    Result = Type::getFloatTy(Context);
  }

  // Doesn't match with any of the cached type handles.
  // Obtain base type by parsing fully qualified class name.
  if (!Result) {
    std::string ClassName = appendClassNameAsString(Class, TRUE, FALSE, FALSE);
    if (ClassName.compare(0, 22, "System.Numerics.Vector") == 0) {
      if (ClassName.compare(22, 3, "`1[") == 0) {
        if (ClassName.compare(25, 13, "System.Single") == 0) {
          SIMDFloatHandle = Class;
          Result = Type::getFloatTy(Context);
        } else if (ClassName.compare(25, 12, "System.Int32") == 0) {
          SIMDIntHandle = Class;
          Result = Type::getInt32Ty(Context);
        } else if (ClassName.compare(25, 13, "System.UInt16") == 0) {
          SIMDUShortHandle = Class;
          IsSigned = false;
          Result = Type::getInt16Ty(Context);
        } else if (ClassName.compare(25, 11, "System.Byte") == 0) {
          SIMDUByteHandle = Class;
          IsSigned = false;
          Result = Type::getInt8Ty(Context);
        } else if (ClassName.compare(25, 13, "System.Double") == 0) {
          SIMDDoubleHandle = Class;
          Result = Type::getDoubleTy(Context);
        } else if (ClassName.compare(25, 12, "System.Int64") == 0) {
          SIMDLongHandle = Class;
          Result = Type::getInt64Ty(Context);
        } else if (ClassName.compare(25, 12, "System.Int16") == 0) {
          SIMDShortHandle = Class;
          Result = Type::getInt16Ty(Context);
        } else if (ClassName.compare(25, 12, "System.SByte") == 0) {
          SIMDByteHandle = Class;
          Result = Type::getInt8Ty(Context);
        } else if (ClassName.compare(25, 13, "System.UInt32") == 0) {
          SIMDUIntHandle = Class;
          IsSigned = false;
          Result = Type::getInt32Ty(Context);
        } else if (ClassName.compare(25, 13, "System.UInt64") == 0) {
          SIMDULongHandle = Class;
          IsSigned = false;
          Result = Type::getInt64Ty(Context);
        }
      } else if (ClassName.compare(22, 2, "2") == 0) {
        SIMDVector2Handle = Class;
        VectorLength = 2;
        return Type::getFloatTy(Context);
      } else if (ClassName.compare(22, 2, "3") == 0) {
        SIMDVector3Handle = Class;
        VectorLength = 3;
        return Type::getFloatTy(Context);
      } else if (ClassName.compare(22, 2, "4") == 0) {
        SIMDVector4Handle = Class;
        VectorLength = 4;
        return Type::getFloatTy(Context);
      }
    }
  }
  if (Result) {
    if (Class == SIMDVector2Handle || Class == SIMDVector3Handle ||
        Class == SIMDVector4Handle) {
      IsGeneric = false;
    } else {
      IsGeneric = true;
      VectorLength = getMaxIntrinsicSIMDVectorLength(Class) * 8 /
                     Result->getPrimitiveSizeInBits();
    }
  }
  return Result;
}

int GenIR::getElementCountOfSIMDType(CORINFO_CLASS_HANDLE Class) {
  int Length = 0;
  bool IsGeneric = false;
  bool IsSigned = false;
  getBaseTypeAndSizeOfSIMDType(Class, Length, IsGeneric, IsSigned);
  return Length;
}

bool GenIR::getIsSigned(CORINFO_CLASS_HANDLE Class) {
  int Length = 0;
  bool IsGeneric = false;
  bool IsSigned = false;
  getBaseTypeAndSizeOfSIMDType(Class, Length, IsGeneric, IsSigned);
  return IsSigned;
}

IRNode *GenIR::vectorGetCount(CORINFO_CLASS_HANDLE Class) {
  return (IRNode *)ConstantInt::get(Type::getInt32Ty(LLVMBuilder->getContext()),
                                    getElementCountOfSIMDType(Class));
}

IRNode *GenIR::vectorGetItem(IRNode *VectorPointer, IRNode *Index,
                             CorInfoType ResType) {
  IRNode *Vector = (IRNode *)LLVMBuilder->CreateLoad(VectorPointer);
  IRNode *Result = (IRNode *)LLVMBuilder->CreateExtractElement(Vector, Index);
  return convertToStackType(Result, ResType);
}

#pragma endregion
