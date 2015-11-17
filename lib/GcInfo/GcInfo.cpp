//===-------- include/gcinfo/gcinfo.cpp -------------------------*- C++ -*-===//
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
/// \brief Implements the generation of CLR GCTables in LLILC
///
//===----------------------------------------------------------------------===//

#include "earlyincludes.h"
#include "GcInfo.h"
#include "LLILCJit.h"
#include "Target.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/StackMapParser.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/WinEHFuncInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Target/TargetFrameLowering.h"

using namespace llvm;

//-------------------------------GcInfo------------------------------------------

bool GcInfo::isGcPointer(const Type *Type) {
  const PointerType *PtrType = dyn_cast<PointerType>(Type);
  if (PtrType != nullptr) {
    return PtrType->getAddressSpace() == GcInfo::ManagedAddressSpace;
  }

  return false;
}

bool GcInfo::isGcAggregate(const Type *AggType) {
  const VectorType *VecType = dyn_cast<VectorType>(AggType);
  if (VecType != nullptr) {
    return isGcPointer(VecType->getScalarType());
  }

  const ArrayType *ArrType = dyn_cast<ArrayType>(AggType);
  if (ArrType != nullptr) {
    return isGcPointer(ArrType->getElementType());
  }

  const StructType *StType = dyn_cast<StructType>(AggType);
  if (StType != nullptr) {
    for (Type *SubType : StType->subtypes()) {
      if (isGcType(SubType)) {
        return true;
      }
    }
  }

  return false;
}

bool GcInfo::isGcAllocation(const llvm::Value *Value) {
  const AllocaInst *Alloca = dyn_cast<const AllocaInst>(Value);
  return ((Alloca != nullptr) && GcInfo::isGcType(Alloca->getAllocatedType()));
}

bool GcInfo::isGcFunction(const llvm::Function *F) {
  if (!F->hasGC()) {
    return false;
  }

  const StringRef CoreCLRName("coreclr");
  return (CoreCLRName == F->getGC());
}

bool GcInfo::isFPBasedFunction(const Function *F) {
  Attribute Attribute = F->getFnAttribute("no-frame-pointer-elim");
  return (Attribute.getValueAsString() == "true");
}

void GcInfo::getGcPointers(StructType *StructTy, const DataLayout &DataLayout,
                           SmallVector<uint32_t, 4> &Pointers) {

  assert(StructTy->isSized());
  const uint32_t PointerSize = DataLayout.getPointerSize();
  const uint32_t TypeSize = DataLayout.getTypeStoreSize(StructTy);

  const StructLayout *MainStructLayout = DataLayout.getStructLayout(StructTy);

  // Walk through the type in pointer-sized jumps.
  for (uint32_t GcOffset = 0; GcOffset < TypeSize; GcOffset += PointerSize) {
    const uint32_t FieldIndex =
        MainStructLayout->getElementContainingOffset(GcOffset);
    Type *FieldTy = StructTy->getStructElementType(FieldIndex);

    // If the field is a value class we need to dive in
    // to its fields and so on, until we reach a primitive type.
    if (FieldTy->isStructTy()) {

      // Prepare to loop through the nesting.
      const StructLayout *OuterStructLayout = MainStructLayout;
      uint32_t OuterOffset = GcOffset;
      uint32_t OuterIndex = FieldIndex;

      while (FieldTy->isStructTy()) {
        // Offset of the Inner class within the outer class
        const uint32_t InnerBaseOffset =
            OuterStructLayout->getElementOffset(OuterIndex);
        // Inner class should start at or before the outer offset
        assert(InnerBaseOffset <= OuterOffset);
        // Determine target offset relative to this inner class.
        const uint32_t InnerOffset = OuterOffset - InnerBaseOffset;
        // Get the inner class layout
        StructType *InnerStructTy = cast<StructType>(FieldTy);
        const StructLayout *InnerStructLayout =
            DataLayout.getStructLayout(InnerStructTy);
        // Find the field at that target offset.
        const uint32_t InnerIndex =
            InnerStructLayout->getElementContainingOffset(InnerOffset);
        // Update for next iteration.
        FieldTy = InnerStructTy->getStructElementType(InnerIndex);
        OuterStructLayout = InnerStructLayout;
        OuterOffset = InnerOffset;
        OuterIndex = InnerIndex;
      }
    }

    if (GcInfo::isGcPointer(FieldTy)) {
      Pointers.push_back(GcOffset);
    }
  }
}

GcFuncInfo *GcInfo::newGcInfo(const llvm::Function *F) {
  assert(getGcInfo(F) == nullptr && "Duplicate GcInfo");
  GcFuncInfo *GcFInfo = new GcFuncInfo(F);
  GcInfoMap[F] = GcFInfo;
  return GcFInfo;
}

GcFuncInfo *GcInfo::getGcInfo(const llvm::Function *F) {
  auto Iterator = GcInfoMap.find(F);
  if (Iterator == GcInfoMap.end()) {
    return nullptr;
  }

  GcFuncInfo *GcFInfo = Iterator->second;
  assert(F == GcFInfo->Function && "Function mismatch");

  return GcFInfo;
}

//-------------------------------GcFuncInfo------------------------------------------

GcFuncInfo::GcFuncInfo(const llvm::Function *F) {
  Function = F;
  GsCkValidRangeStart = 0;
  GsCkValidRangeEnd = 0;
  GenericsContextParamType = GENERIC_CONTEXTPARAM_NONE;
  PSPSymOffset = 0;
  HasFunclets = false;
}

void GcFuncInfo::recordAlloca(const AllocaInst *Alloca) {
  assert(!hasRecord(Alloca) && "Duplicate Allocation");
  Type *Type = Alloca->getAllocatedType();
  AllocaFlags Flags = AllocaFlags::None;

  if (GcInfo::isGcPointer(Type)) {
    Flags = AllocaFlags::GcPointer;
  } else if (GcInfo::isGcAggregate(Type)) {
    Flags = AllocaFlags::GcAggregate;
  }

  AllocaMap[Alloca] = {GcInfo::InvalidPointerOffset, Flags};
}

void GcFuncInfo::recordGcAlloca(const AllocaInst *Alloca) {
  recordAlloca(Alloca);
  assert(AllocaMap[Alloca].isGcValue() && "Expected GcValue");
}

void GcFuncInfo::markGcAlloca(const AllocaInst *Alloca,
                              const AllocaFlags Flags) {
  assert(GcInfo::isGcAllocation(Alloca) && "GcValue expected");
  assert(hasRecord(Alloca) && "Missing Alloca recorded");

  AllocaInfo &AllocaInfo = AllocaMap[Alloca];
  assert(AllocaInfo.isGcValue() && "GcValue is recorded incorrectly");
  assert(AllocaInfo.Offset == GcInfo::InvalidPointerOffset &&
         "Changing annotation after GcInfoRecorder");

  AllocaInfo.Flags = (AllocaFlags)(AllocaInfo.Flags | Flags);
}

void GcFuncInfo::markNonGcAlloca(const AllocaInst *Alloca,
                                 const AllocaFlags Flags) {
  assert(!GcInfo::isGcAllocation(Alloca) && "GcPointer not expected");
  assert(hasRecord(Alloca) && "Missing Alloca Record");

  AllocaInfo &AllocaInfo = AllocaMap[Alloca];
  assert(!AllocaInfo.isGcValue() && "Integer is recorded incorrectly");
  assert(AllocaInfo.Offset == GcInfo::InvalidPointerOffset &&
         "Changing annotation after GcInfoRecorder");

  AllocaInfo.Flags = (AllocaFlags)(AllocaInfo.Flags | Flags);
}

void GcFuncInfo::recordPinned(const AllocaInst *Alloca) {
  markGcAlloca(Alloca, AllocaFlags::Pinned);
}

void GcFuncInfo::recordSecurityObject(const AllocaInst *Alloca) {
  markGcAlloca(Alloca, AllocaFlags::SecurityObject);
}

void GcFuncInfo::recordGenericsContext(
    const AllocaInst *Alloca, const GENERIC_CONTEXTPARAM_TYPE ParamType) {
  const AllocaFlags Flags = AllocaFlags::GenericsContext;
  if (ParamType == GENERIC_CONTEXTPARAM_TYPE::GENERIC_CONTEXTPARAM_THIS) {
    markGcAlloca(Alloca, Flags);
  } else {
    // Integer allocations are not recorded at the time of IR creation.
    // So, record the alloca and then mark.
    recordAlloca(Alloca);
    markNonGcAlloca(Alloca, Flags);
  }

  GenericsContextParamType = ParamType;
}

void GcFuncInfo::recordGsCookie(const AllocaInst *Alloca,
                                const uint32_t ValidRangeStart,
                                const uint32_t ValidRangeEnd) {
  // TODO: This feature is untested.
  // Reader doesn't implement GS Cookie guard yet.
  // Implement stack protection checks
  // https://github.com/dotnet/llilc/issues/353
  assert(false && "UnTested");

  recordAlloca(Alloca);
  markNonGcAlloca(Alloca, AllocaFlags::SecurityObject);
  GsCkValidRangeStart = ValidRangeStart;
  GsCkValidRangeEnd = ValidRangeEnd;
}

void GcFuncInfo::getEscapingLocations(SmallVector<Value *, 4> &EscapingLocs) {
  for (auto AllocaIterator : AllocaMap) {
    EscapingLocs.push_back(
        const_cast<Value *>(static_cast<const Value *>(AllocaIterator.first)));
  }
}

//-------------------------------GcAllocaInfo-----------------------------------

const char *AllocaInfo::getAllocTypeString() const {
  if (Flags & AllocaFlags::GsCookie) {
    return "GS Cookie";
  }

  if (Flags & AllocaFlags::GenericsContext) {
    if (Flags & AllocaFlags::GcPointer) {
      return "GenericsContextThis";
    } else {
      return "GenericsContextTypeArg";
    }
  }

  if (Flags & AllocaFlags::SecurityObject) {
    return "SecurityObject";
  }

  if (Flags & AllocaFlags::GcPointer) {
    return "GcPointer";
  }

  if (Flags & AllocaFlags::GcAggregate) {
    return "GcAggregate";
  }

  llvm_unreachable("Unexpected Flags Combination");
}

//-------------------------------GcInfoRecorder-----------------------------------

char GcInfoRecorder::ID = 0;

bool GcInfoRecorder::runOnMachineFunction(MachineFunction &MF) {
  const Function *F = MF.getFunction();
  if (!GcInfo::isGcFunction(F)) {
    return false;
  }

  LLILCJitContext *Context = LLILCJit::TheJit->getLLILCJitContext();
  GcFuncInfo *GcFuncInfo = Context->GcInfo->getGcInfo(F);
  ValueMap<const AllocaInst *, AllocaInfo> &AllocaMap = GcFuncInfo->AllocaMap;

#if !defined(NDEBUG)
  bool EmitLogs = Context->Options->LogGcInfo;

  if (EmitLogs) {
    dbgs() << "GcInfoRecorder: " << MF.getFunction()->getName() << "\n";
  }
#endif // !NDEBUG

  const MachineFrameInfo *FrameInfo = MF.getFrameInfo();
  int ObjectIndexBegin = FrameInfo->getObjectIndexBegin();
  int ObjectIndexEnd = FrameInfo->getObjectIndexEnd();

  // FrameInfo reports the allocation offsets in terms of the
  // incoming (caller's) StackPointer. Convert these in terms of the
  // current (callee's) StackPointer.
  uint64_t StackPointerSize = MF.getDataLayout().getPointerSize();
  uint64_t SpOffset = FrameInfo->getStackSize() + StackPointerSize;

  for (int Idx = ObjectIndexBegin; Idx < ObjectIndexEnd; Idx++) {
    const AllocaInst *Alloca = FrameInfo->getObjectAllocation(Idx);
    if (Alloca == nullptr) {
      continue;
    }

    if (GcFuncInfo->hasRecord(Alloca)) {
      int32_t SlotOffset = SpOffset + FrameInfo->getObjectOffset(Idx);
      AllocaInfo &AllocaInfo = AllocaMap[Alloca];

      assert(SlotOffset >= 0);
      assert(AllocaInfo.Offset == GcInfo::InvalidPointerOffset &&
             "Two slots for the same alloca!");

      AllocaInfo.Offset = SlotOffset;

#if !defined(NDEBUG)
      if (AllocaInfo.isGcAggregate()) {
        assert(isa<StructType>(Alloca->getAllocatedType()) &&
               "Unhandled Type of GcAggregate");
      } else if (AllocaInfo.isGcPointer()) {
        assert(GcInfo::isGcPointer(Alloca->getAllocatedType()));
      } else {
        assert(!GcInfo::isGcAllocation(Alloca));
      }

      if (EmitLogs) {
        dbgs() << AllocaInfo.getAllocTypeString() << " @ sp+" << SlotOffset
               << " [";
        Alloca->printAsOperand(dbgs(), false);
        dbgs() << "]\n";
      }
#endif // !NDEBUG
    } else {
// All GC-aggregate Allocas must be registered before this phase.
// This ensures that the allocations are properly initialized,
// and marked as frame-escaped if necessary.
//
// TODO: The following check should be:
// assert(!GcInfo::isGcAllocation(Alloca) &&
//        "Gc Allocation Record missing");

#if !defined(NDEBUG)
      if (GcInfo::isGcAllocation(Alloca)) {
        // However, some Gc-pointer slots created by WinEHPrepare phase
        // go unrecorded currently because the AllocaInfos are recorded
        // by the Reader post-pass.
        // TODO: Report the Spill slots created by WinEHPrepare
        // https://github.com/dotnet/llilc/issues/901

        assert(Alloca->hasName());
        assert(Alloca->getName().find(".wineh.spillslot") != StringRef::npos);
        // WinEH shouldn't spill GC-aggregates
        assert(!GcInfo::isGcAggregate(Alloca->getAllocatedType()));

        // The unreported slots are live across safepoints in the
        // EH path, so the execution is correct unless we take the
        // exception path.
        assert(!(Context->Options->ExecuteHandlers &&
                 Context->Options->DoInsertStatepoints) &&
               "Untested: Use at your own risk");
      }
#endif // !NDEBUG
    }
  }

  // Unlike the other offsets reported to the GC, the PSPSym offset is relative
  // to Initial-SP (i.e. the value of the stack pointer just after this
  // method's prolog), NOT Caller-SP.
  if (WinEHFuncInfo *EHInfo = MF.getWinEHFuncInfo()) {
    int PSPSymIndex = EHInfo->PSPSymFrameIdx;
    if (PSPSymIndex != INT_MAX) {
      GcFuncInfo->HasFunclets = true;
      const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
      // SPReg is an out parameter that we're not using here.
      unsigned SPReg;
      int PSPOffset = TFL->getFrameIndexReferenceFromSP(MF, PSPSymIndex, SPReg);
      assert(PSPOffset >= 0);
      GcFuncInfo->PSPSymOffset = static_cast<uint32_t>(PSPOffset);
    }
  }

  return false; // success
}

//-------------------------------GcInfoEmitter-----------------------------------

GcInfoEmitter::GcInfoEmitter(LLILCJitContext *JitCtx, uint8_t *StackMapData,
                             GcInfoAllocator *Allocator)

    : JitContext(JitCtx), LLVMStackMapData(StackMapData),
      Encoder(JitContext->JitInfo, JitContext->MethodInfo, Allocator),
      SlotMap(), FirstTrackedSlot(0), NumTrackedSlots(0) {
#if !defined(NDEBUG)
  this->EmitLogs = JitContext->Options->LogGcInfo;
#endif // !NDEBUG
#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  this->CallSites = nullptr;
  this->CallSiteSizes = nullptr;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
}

void GcInfoEmitter::encodeHeader(const GcFuncInfo *GcFuncInfo) {
  const Function *F = GcFuncInfo->Function;

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "GcTable for Function: " << F->getName() << "\n";
  }
#endif // !NDEBUG

  if (GcFuncInfo->HasFunclets) {
    Encoder.SetWantsReportOnlyLeaf();
    Encoder.SetPSPSymStackSlot(GcFuncInfo->PSPSymOffset);
#if !defined(NDEBUG)
    if (EmitLogs) {
      dbgs() << "Has funclets, PSP Slot: " << GcFuncInfo->PSPSymOffset << "\n";
    }
#endif // !NDEBUG
  }

  // TODO: Set Code Length accurately.
  // https://github.com/dotnet/llilc/issues/679
  // JitContext->HotCodeSize is the size of the allocated code block.
  // It is not the actual length of the current function's code.
  Encoder.SetCodeLength(JitContext->HotCodeSize);
#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Size: " << JitContext->HotCodeSize << "\n";
  }
#endif // !NDEBUG

  if (GcInfo::isFPBasedFunction(F)) {
    Encoder.SetStackBaseRegister(REGNUM_FPBASE);
#if !defined(NDEBUG)
    if (EmitLogs) {
      dbgs() << "  StackBaseRegister: FP\n";
    }
#endif // !NDEBUG
  } else {
#if !defined(NDEBUG)
    if (EmitLogs) {
      dbgs() << "  StackBaseRegister: SP\n";
    }
#endif // !NDEBUG
  }

  if (GcFuncInfo->Function->isVarArg()) {
    Encoder.SetIsVarArg();
  }

#if defined(FIXED_STACK_PARAMETER_SCRATCH_AREA)
  // TODO: Set size of outgoing/scratch area accurately
  // https://github.com/dotnet/llilc/issues/681
  const unsigned ScratchAreaSize = 0;
  Encoder.SetSizeOfStackOutgoingAndScratchArea(ScratchAreaSize);
#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Scratch Area Size: " << ScratchAreaSize << "\n";
  }
#endif // !NDEBUG
#endif // defined(FIXED_STACK_PARAMETER_SCRATCH_AREA)
}

void GcInfoEmitter::encodeTrackedPointers(const GcFuncInfo *GcFuncInfo) {
  ArrayRef<uint8_t> StackMapContentsArray(LLVMStackMapData,
                                          JitContext->StackMapSize);

#if defined(BIGENDIAN)
  typedef StackMapV1Parser<support::big> StackMapParserType;
#else
  typedef StackMapV1Parser<support::little> StackMapParserType;
#endif
  StackMapParserType StackMapParser(StackMapContentsArray);

  // TODO: Once StackMap v2 is implemented, remove this assertion about
  // one function per module, and emit the GcInfo for the records
  // corresponding to the Function 'F'
  assert(StackMapParser.getNumFunctions() == 1 &&
         "Expect only one function with GcInfo in the module");

// Loop over LLVM StackMap records to:
// 1) Note CallSites (safepoints)
// 2) Assign Slot-IDs to each unique gc-pointer location (slot)
// 3) Record liveness (birth/death) of slots per call-site.

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  NumCallSites = StackMapParser.getNumRecords();
  CallSites = new unsigned[NumCallSites];
  CallSiteSizes = new BYTE[NumCallSites];
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)

  // TODO: Determine call-site-size accurately
  // https://github.com/Microsoft/llvm/issues/56
  // Call-site size is not available in LLVM's StackMap v1, so just make up
  // a value for now. The Call-instruction generated by LLILC on X86/X64
  // is typically Call [rax], which has a two-byte encoding.
  //
  // Any size > 0 can be reported as CallSizeSize, see explanation below.
  //
  // CoreCLR's API expects that we report:
  // (a) the offset at the beginning of the Call instruction, and
  // (b) size of the call instruction.
  //
  // LLVM's stackMap currently only reports:
  // (c) the offset at the safepoint after the call instruction (= a+b)
  //
  // When not in a fully-intrruptible block, CoreCLR only
  // uses the value of (a+b) to determine the end of the call
  // instruction. Therefore, we simply report a = c-2 and b = 2 for now.
  //
  // Once call-size size is available in LLVM StackMap v2, we can remove this
  // implementation specific workaround.

  const uint8_t CallSiteSize = 2;

  // LLVM StackMap records all live-pointers per Safepoint, whereas
  // CoreCLR's GCTables record pointer birth/deaths per Safepoint.
  // So, we do the translation using old/new live-pointer-sets
  // using bit-sets for recording the liveness -- one bit per slot.
  //
  // If Untracked slots are allocated before tracked ones, the
  // bits corresponding to the untracked SlotIds will go unused.

  size_t LiveBitSetSize = 25;
  SmallBitVector OldLiveSet(LiveBitSetSize);
  SmallBitVector NewLiveSet(LiveBitSetSize);

  size_t RecordIndex = 0;
  for (const auto &R : StackMapParser.records()) {

    // InstructionOffset - CallSiteSize:
    //   to report the start of the Instruction
    //
    // LLVM's Safepoint reports the offset at the end of the Call
    // instruction, whereas the CoreCLR API expects that we report
    // the start of the Call instruction.

    unsigned InstructionOffset = R.getInstructionOffset() - CallSiteSize;

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
    CallSites[RecordIndex] = InstructionOffset;
    CallSiteSizes[RecordIndex] = CallSiteSize;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)

#if !defined(NDEBUG)
    if (EmitLogs) {
      LiveStream << "    " << RecordIndex << ": @" << InstructionOffset;
    }
#endif // !NDEBUG

    for (const auto &Loc : R.locations()) {

      switch (Loc.getKind()) {
      case StackMapParserType::LocationKind::Constant:
      case StackMapParserType::LocationKind::ConstantIndex:
        continue;

      case StackMapParserType::LocationKind::Register:
        // TODO: Report Live - GC values in Registers
        // https://github.com/dotnet/llilc/issues/474
        // Live gc-pointers are currently spilled to the stack at Safepoints.
        assert(false && "GC-Pointer Live in Register");
        break;

      case StackMapParserType::LocationKind::Direct: {
        // __LLVM_Stackmap reports the liveness of pointers wrt SP even for
        // methods which have a FP.
        assert(Loc.getDwarfRegNum() == DW_STACK_POINTER &&
               "Expect Stack Pointer to be the base");

        GcSlotId SlotID;
        int32_t Offset = Loc.getOffset();
        DenseMap<int32_t, GcSlotId>::const_iterator ExistingSlot =
            SlotMap.find(Offset);
        if (ExistingSlot == SlotMap.end()) {
          SlotID = getTrackedSlot(Offset);

          if (SlotMap.size() > LiveBitSetSize) {
            LiveBitSetSize += LiveBitSetSize;

            assert(LiveBitSetSize > OldLiveSet.size() &&
                   "Overflow -- Too many live pointers");

            OldLiveSet.resize(LiveBitSetSize);
            NewLiveSet.resize(LiveBitSetSize);
          }
        } else {
          SlotID = ExistingSlot->second;
        }

        assert(isTrackedSlot(SlotID) &&
               "Tracked and Untracked slots must be disjoint");
        NewLiveSet[SlotID] = true;
        break;
      }

      default:
        assert(false && "Unexpected Location Type");
        break;
      }
    }

    for (GcSlotId SlotID = 0; SlotID < SlotMap.size(); SlotID++) {
      if (!OldLiveSet[SlotID] && NewLiveSet[SlotID]) {
#if !defined(NDEBUG)
        if (EmitLogs) {
          LiveStream << "  +" << SlotID;
        }
#endif // !NDEBUG
        Encoder.SetSlotState(InstructionOffset, SlotID, GC_SLOT_LIVE);
      } else if (OldLiveSet[SlotID] && !NewLiveSet[SlotID]) {
#if !defined(NDEBUG)
        if (EmitLogs) {
          LiveStream << "  -" << SlotID;
        }
#endif // !NDEBUG
        Encoder.SetSlotState(InstructionOffset, SlotID, GC_SLOT_DEAD);
      }

      OldLiveSet[SlotID] = NewLiveSet[SlotID];
      NewLiveSet[SlotID] = false;
    }

    RecordIndex++;

#if !defined(NDEBUG)
    if (EmitLogs) {
      LiveStream << "\n";
    }
#endif // !NDEBUG
  }
}

void GcInfoEmitter::encodeUntrackedPointers(const GcFuncInfo *GcFuncInfo) {
  for (auto AllocaIterator : GcFuncInfo->AllocaMap) {
    const AllocaInfo &AllocaInfo = AllocaIterator.second;
    const AllocaInst *Alloca = AllocaIterator.first;

    assert(AllocaInfo.Offset != GcInfo::InvalidPointerOffset &&
           "Invalid Offset for Alloca Record");

    if (AllocaInfo.isGcAggregate()) {
      encodeGcAggregate(Alloca, AllocaInfo);
    } else if (AllocaInfo.isGcPointer()) {
      getUntrackedSlot(AllocaInfo.Offset, AllocaInfo.isPinned());
    }

    // The Followig information is not yet reported to the Runtime
    // for the want of additional information.
    //  -> Prolog Size
    //  -> GS Cookie validity range
    //
    // TODO: The SlotOffset may need to be modified wrt Caller's SP
    // for the following interfaces.

    if (AllocaInfo.Flags & AllocaFlags::GsCookie) {
      // TODO: GC: Report GS Cookie
      // https://github.com/dotnet/llilc/issues/768

      // Encoder.SetGSCookieStackSlot(AllocaInfo.Offset,
      //                              GcFuncInfo->GsCkValidRangeStart,
      //                              GcFuncInfo->GsCkValidRangeEnd);
    }

    if (AllocaInfo.Flags & AllocaFlags::GenericsContext) {
      // TODO: GC: Report GenericsInstContextStackSlot
      // https://github.com/dotnet/llilc/issues/766

      // Encoder.SetGenericsInstContextStackSlot(
      //  AllocaInfo.Offset,
      //  GcFuncInfo->GenericsContextParamType);
    }

    if (AllocaInfo.Flags & AllocaFlags::SecurityObject) {
      // TODO: GC: Report SecurityObjectStackSlot
      // https://github.com/dotnet/llilc/issues/767

      // Encoder.SetSecurityObjectStackSlot(AllocaInfo.Offset);
    }
  }
}

void GcInfoEmitter::encodeGcAggregate(const AllocaInst *Alloca,
                                      const AllocaInfo &AllocaInfo) {
  int32_t AggregateOffset = AllocaInfo.Offset;
  Type *Type = Alloca->getAllocatedType();
  StructType *StructTy = cast<StructType>(Type);
  SmallVector<uint32_t, 4> GcPtrOffsets;
  const DataLayout &DataLayout = JitContext->CurrentModule->getDataLayout();
  GcInfo::getGcPointers(StructTy, DataLayout, GcPtrOffsets);
  bool IsPinned = AllocaInfo.isPinned();
  bool IsObjectReference = true; // Only ObjectReferences can be stored in
                                 // GC Aggregates.

  assert(GcPtrOffsets.size() > 0 && "GC Aggregate without GC pointers!");

  for (uint32_t GcPtrOffset : GcPtrOffsets) {
    getUntrackedSlot(AggregateOffset + GcPtrOffset, IsPinned,
                     IsObjectReference);
  }
}

void GcInfoEmitter::finalizeEncoding() {
  // Finalize Slot IDs to enable compact representation
  Encoder.FinalizeSlotIds();

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  // Encode Call-sites
  assert(CallSiteSizes != nullptr);
  assert(CallSites != nullptr);
  assert(NumCallSites > 0);
  Encoder.DefineCallSites(CallSites, CallSiteSizes, NumCallSites);
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
}

void GcInfoEmitter::emitEncoding() {
  Encoder.Build();
  Encoder.Emit();

#if !defined(NDEBUG)
  if (EmitLogs) {
    dbgs() << "  Slots:\n" << SlotStream.str();
    dbgs() << "  Safepoints:\n" << LiveStream.str() << "\n";
  }
#endif // !NDEBUG
}

GcInfoEmitter::~GcInfoEmitter() {
#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  delete CallSites;
  delete CallSiteSizes;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
}

void GcInfoEmitter::emitGCInfo(const GcFuncInfo *GcFuncInfo) {
  assert((GcFuncInfo != nullptr) && "Function missing GcInfo");

  encodeHeader(GcFuncInfo);

  if (needsPointerReporting(GcFuncInfo->Function)) {
    // Assign Slots for Tracked pointers and report their liveness
    encodeTrackedPointers(GcFuncInfo);
    // Assign Slots for untracked pointers
    encodeUntrackedPointers(GcFuncInfo);
    // Finalization must be done after all encodings
    finalizeEncoding();
  }

  emitEncoding();
}

void GcInfoEmitter::emitGCInfo() {
  for (auto GcInfoIterator : JitContext->GcInfo->GcInfoMap) {
    GcFuncInfo *GcFuncInfo = GcInfoIterator->second;
    if (needsGCInfo(GcFuncInfo->Function)) {
      emitGCInfo(GcFuncInfo);
    }
  }
}

bool GcInfoEmitter::needsGCInfo(const Function *F) {
  return !F->isDeclaration() && GcInfo::isGcFunction(F);
}

bool GcInfoEmitter::needsPointerReporting(const Function *F) {
  bool TargetPreciseGcRuntime = JitContext->Options->DoInsertStatepoints;
  bool HasGcSafePoints = (LLVMStackMapData != nullptr);
  return TargetPreciseGcRuntime && HasGcSafePoints;
}

bool GcInfoEmitter::isTrackedSlot(const GcSlotId SlotID) {
  return (NumTrackedSlots > 0) && (SlotID >= FirstTrackedSlot) &&
         (SlotID < FirstTrackedSlot + NumTrackedSlots);
}

GcSlotId GcInfoEmitter::getSlot(const int32_t Offset, const GcSlotFlags Flags) {
  assert(Offset != GcInfo::InvalidPointerOffset && "Invalid Slot Offset");
  assert(!hasSlot(Offset) && "Slot already allocated");

  GcSlotId SlotID = Encoder.GetStackSlotId(Offset, Flags, GC_SP_REL);
  SlotMap[Offset] = SlotID;

  assert(SlotID == (SlotMap.size() - 1) && "SlotIDs dis-contiguous");

#if !defined(NDEBUG)
  if (EmitLogs) {
    SlotStream << "    [" << SlotID << "]: "
               << "sp+" << Offset << " ("
               << ((Flags & GC_SLOT_INTERIOR) ? "M" : "O")
               << ((Flags & GC_SLOT_UNTRACKED) ? "U" : "")
               << ((Flags & GC_SLOT_PINNED) ? "P" : "") << ")\n";
  }
#endif // !NDEBUG

  return SlotID;
}

GcSlotId GcInfoEmitter::getTrackedSlot(const int32_t Offset) {
  // TODO: Identify Object and Managed pointers differently
  // https://github.com/dotnet/llilc/issues/28
  // We currently conservatively describe all slots as containing
  // interior pointers
  const GcSlotFlags ManagedPointerFlags = (GcSlotFlags)GC_SLOT_INTERIOR;
  GcSlotId SlotID = getSlot(Offset, ManagedPointerFlags);
  NumTrackedSlots++;

  if (NumTrackedSlots == 1) {
    FirstTrackedSlot = SlotID;
  }

  return SlotID;
}

GcSlotId GcInfoEmitter::getUntrackedSlot(const int32_t Offset, bool IsPinned,
                                         bool IsObjectRef) {
  GcSlotFlags UntrackedFlags = (GcSlotFlags)GC_SLOT_UNTRACKED;

  if (IsPinned) {
    // Pinned pointers are always ObjectReferences
    UntrackedFlags = (GcSlotFlags)(GC_SLOT_UNTRACKED | GC_SLOT_PINNED);
  } else if (!IsObjectRef) {
    UntrackedFlags = (GcSlotFlags)(GC_SLOT_UNTRACKED | GC_SLOT_INTERIOR);
  }

  GcSlotId SlotID = getSlot(Offset, UntrackedFlags);

  return SlotID;
}
