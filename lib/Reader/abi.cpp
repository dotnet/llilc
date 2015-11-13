//===------------------- include/Reader/abi.cpp -----------------*- C++ -*-===//
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
/// \brief Defines the ABI abstraction used when lowering functions to LLVM IR.
///
//===----------------------------------------------------------------------===//

#include "earlyincludes.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "reader.h"
#include "readerir.h"
#include "abi.h"
#include <cstdint>
#include <cassert>

using namespace llvm;

// Static class with helpers for the Microsoft x86-64 ABI.
class X86_64_Win64 {
private:
  X86_64_Win64() {}

  static ABIArgInfo classify(const ABIType Ty, const DataLayout &DL,
                             bool IsManagedCallingConv);

  static ABIArgInfo classifyResult(const ABIType Ty, const DataLayout &DL,
                                   bool IsManagedCallingConv);

public:
  static void computeSignatureInfo(bool IsManagedCallingConv,
                                   ABIType ResultType,
                                   ArrayRef<ABIType> ArgTypes,
                                   const DataLayout &DL, ABIArgInfo &ResultInfo,
                                   std::vector<ABIArgInfo> &ArgInfos);
};

// Static class wqith helpers for the System V x86-64 ABI.
class X86_64_SysV {
private:
  X86_64_SysV() {}

  static ABIArgInfo classify(LLILCJitContext &JitContext, const ABIType Ty,
                             const DataLayout &DL, bool IsManagedCallingConv,
                             bool IsResult, uint32_t &RequiredIntRegs,
                             uint32_t &RequiredSSERegs);

  static ABIArgInfo classifyArgument(LLILCJitContext &JitContext,
                                     const ABIType Ty, const DataLayout &DL,
                                     bool IsManagedCallingConv,
                                     uint32_t &RequiredIntRegs,
                                     uint32_t &RequiredSSERegs);

  static ABIArgInfo classifyResult(LLILCJitContext &JitContext,
                                   const ABIType Ty, const DataLayout &DL,
                                   bool IsManagedCallingConv);

public:
  static void computeSignatureInfo(LLILCJitContext &JitContext,
                                   bool IsManagedCallingConv,
                                   ABIType ResultType,
                                   ArrayRef<ABIType> ArgTypes,
                                   const DataLayout &DL, ABIArgInfo &ResultInfo,
                                   std::vector<ABIArgInfo> &ArgInfos);
};

class X86_64ABIInfo : public ABIInfo {
private:
  bool IsWindows;
  const DataLayout &TheDataLayout;

public:
  X86_64ABIInfo(Triple TargetTriple, const DataLayout &DL);

  void computeSignatureInfo(LLILCJitContext &JitContext, CallingConv::ID CC,
                            bool IsManagedCallingConv, ABIType ResultType,
                            ArrayRef<ABIType> ArgTypes, ABIArgInfo &ResultInfo,
                            std::vector<ABIArgInfo> &ArgInfos) const override;
};

ABIArgInfo X86_64_Win64::classify(const ABIType ABITy, const DataLayout &DL,
                                  bool IsManagedCallingConv) {
  Type *Ty = ABITy.getType();
  assert(!Ty->isVoidTy());

  if (Ty->isAggregateType() || Ty->isVectorTy()) {
    // If the aggregate's size in bytes is a power of 2 that is less than or
    // equal to 8, it can be passed directly once coerced to an
    // appropriately-sized
    // integer. Otherwise, it must be passed indirectly.
    uint64_t SizeInBits = DL.getTypeSizeInBits(Ty);
    uint64_t SizeInBytes = SizeInBits / 8;
    if (SizeInBytes <= 8 && llvm::isPowerOf2_64(SizeInBytes)) {
      return ABIArgInfo::getDirect(
          IntegerType::get(Ty->getContext(), SizeInBits));
    }

    return ABIArgInfo::getIndirect(Ty);
  }

  if (Ty->isIntegerTy()) {
    assert(Ty->getIntegerBitWidth() <= 64);

    // RyuJIT requires that all arguments and return values smaller than 32 bits
    // are zero- or sign-extended.
    if (IsManagedCallingConv && Ty->getIntegerBitWidth() < 32) {
      return ABITy.isSigned() ? ABIArgInfo::getSignExtend(Ty)
                              : ABIArgInfo::getZeroExtend(Ty);
    }

    return ABIArgInfo::getDirect(Ty);
  }

  if (Ty->isFloatingPointTy()) {
    assert(Ty->isFloatTy() || Ty->isDoubleTy());
    return ABIArgInfo::getDirect(Ty);
  }

  assert(Ty->isPointerTy());
  return ABIArgInfo::getDirect(Ty);
}

ABIArgInfo X86_64_Win64::classifyResult(const ABIType ABITy,
                                        const DataLayout &DL,
                                        bool IsManagedCallingConv) {
  Type *Ty = ABITy.getType();
  if (Ty->isVoidTy()) {
    return ABIArgInfo::getDirect(Ty);
  }

  return classify(ABITy, DL, IsManagedCallingConv);
}

void X86_64_Win64::computeSignatureInfo(bool IsManagedCallingConv,
                                        ABIType ResultType,
                                        ArrayRef<ABIType> ArgTypes,
                                        const DataLayout &DL,
                                        ABIArgInfo &ResultInfo,
                                        std::vector<ABIArgInfo> &ArgInfos) {
  ResultInfo = classifyResult(ResultType, DL, IsManagedCallingConv);

  for (auto &Arg : ArgTypes) {
    ArgInfos.push_back(classify(Arg, DL, IsManagedCallingConv));
  }
}

ABIArgInfo X86_64_SysV::classify(LLILCJitContext &Context, const ABIType ABITy,
                                 const DataLayout &DL,
                                 bool IsManagedCallingConv, bool IsResult,
                                 uint32_t &RequiredIntRegs,
                                 uint32_t &RequiredSSERegs) {
  Type *Ty = ABITy.getType();
  assert(!Ty->isVoidTy());

  if (Ty->isAggregateType() || Ty->isVectorTy()) {
    SYSTEMV_AMD64_CORINFO_STRUCT_REG_PASSING_DESCRIPTOR Desc;

    bool Ok = Context.JitInfo->getSystemVAmd64PassStructInRegisterDescriptor(
        ABITy.getClass(), &Desc);
    assert(Ok);
    (void)Ok; // Silence unused variable warnings in release builds.

    if (!Desc.passedInRegisters) {
      if (IsResult) {
        // Per the SysV ABI spec, return values that are not enregistered are
        // instead passed via a buffer argument allocated by the caller. In this
        // case, the buffer argument will consume a single integer register.
        RequiredIntRegs = 1;
        RequiredSSERegs = 0;
        return ABIArgInfo::getIndirect(Ty);
      } else {
        // Argument values that are not enregistered are passed on the stack.
        RequiredIntRegs = 0;
        RequiredSSERegs = 0;
        return ABIArgInfo::getDirect(Ty);
      }
    } else {
      RequiredIntRegs = 0;
      RequiredSSERegs = 0;

      LLVMContext &LLVMContext = *Context.LLVMContext;
      SmallVector<ABIArgInfo::Expansion, 2> Expansions;
      for (uint32_t I = 0; I < Desc.eightByteCount; I++) {
        Type *Ty;
        uint32_t Size = Desc.eightByteSizes[I];
        switch (Desc.eightByteClassifications[I]) {
        case SystemVClassificationTypeInteger:
          Ty = Type::getIntNTy(LLVMContext, Size * 8);
          RequiredIntRegs++;
          break;

        case SystemVClassificationTypeSSE:
          Ty = Size <= 4 ? Type::getFloatTy(LLVMContext)
                         : Type::getDoubleTy(LLVMContext);
          RequiredSSERegs++;
          break;

        case SystemVClassificationTypeIntegerReference:
          assert(Size == 8);

          // This eightbyte contains a GC reference. Type it as a manged
          // pointer.
          Ty = Type::getInt8Ty(LLVMContext)->getPointerTo(1);
          RequiredIntRegs++;
          break;

        default:
          llvm_unreachable("unexpected SysV classification");
        }

        ABIArgInfo::Expansion Expansion;
        Expansion.TheType = Ty;
        Expansion.Offset = Desc.eightByteOffsets[I];
        Expansions.push_back(Expansion);
      }

      return ABIArgInfo::getExpand(Expansions);
    }
  }

  if (Ty->isIntegerTy()) {
    assert(Ty->getIntegerBitWidth() <= 64);

    RequiredIntRegs = 1;
    RequiredSSERegs = 0;

    // RyuJIT requires that all arguments and return values smaller than 32 bits
    // are zero- or sign-extended.
    if (IsManagedCallingConv && Ty->getIntegerBitWidth() < 32) {
      return ABITy.isSigned() ? ABIArgInfo::getSignExtend(Ty)
                              : ABIArgInfo::getZeroExtend(Ty);
    }

    return ABIArgInfo::getDirect(Ty);
  }

  if (Ty->isFloatingPointTy()) {
    assert(Ty->isFloatTy() || Ty->isDoubleTy());
    RequiredIntRegs = 0;
    RequiredSSERegs = 1;
    return ABIArgInfo::getDirect(Ty);
  }

  assert(Ty->isPointerTy());
  RequiredIntRegs = 1;
  RequiredSSERegs = 0;
  return ABIArgInfo::getDirect(Ty);
}

ABIArgInfo X86_64_SysV::classifyArgument(LLILCJitContext &JitContext,
                                         const ABIType ABITy,
                                         const DataLayout &DL,
                                         bool IsManagedCallingConv,
                                         uint32_t &RequiredIntRegs,
                                         uint32_t &RequiredSSERegs) {
  const bool IsResult = false;
  return classify(JitContext, ABITy, DL, IsManagedCallingConv, IsResult,
                  RequiredIntRegs, RequiredSSERegs);
}

ABIArgInfo X86_64_SysV::classifyResult(LLILCJitContext &JitContext,
                                       const ABIType ABITy,
                                       const DataLayout &DL,
                                       bool IsManagedCallingConv) {
  Type *Ty = ABITy.getType();
  if (Ty->isVoidTy()) {
    return ABIArgInfo::getDirect(Ty);
  }

  const bool IsResult = true;
  uint32_t RequiredIntRegs, RequiredSSERegs;
  return classify(JitContext, ABITy, DL, IsManagedCallingConv, IsResult,
                  RequiredIntRegs, RequiredSSERegs);
}

void X86_64_SysV::computeSignatureInfo(
    LLILCJitContext &Context, bool IsManagedCallingConv, ABIType ResultType,
    ArrayRef<ABIType> ArgTypes, const DataLayout &DL, ABIArgInfo &ResultInfo,
    std::vector<ABIArgInfo> &ArgInfos) {
  ResultInfo = classifyResult(Context, ResultType, DL, IsManagedCallingConv);

  uint32_t FreeIntRegs = ResultInfo.getKind() == ABIArgInfo::Indirect ? 5 : 6;
  uint32_t FreeSSERegs = 8;

  for (auto &Arg : ArgTypes) {
    uint32_t RequiredIntRegs = 0, RequiredSSERegs = 0;
    ABIArgInfo Info = classifyArgument(Context, Arg, DL, IsManagedCallingConv,
                                       RequiredIntRegs, RequiredSSERegs);

    // As per the ABI, if any eightbyte of a struct value does not fit in the
    // number of available registers, the entire struct value is passed on the
    // stack.
    if (Info.getKind() == ABIArgInfo::Expand &&
        (FreeIntRegs < RequiredIntRegs || FreeSSERegs < RequiredSSERegs)) {
      Info = ABIArgInfo::getDirect(Arg.getType());
    } else {
      if (RequiredIntRegs > 0 && FreeIntRegs >= RequiredIntRegs) {
        FreeIntRegs -= RequiredIntRegs;
      }
      if (RequiredSSERegs > 0 && FreeSSERegs >= RequiredSSERegs) {
        FreeSSERegs -= RequiredSSERegs;
      }
    }

    ArgInfos.push_back(std::move(Info));
  }
}

X86_64ABIInfo::X86_64ABIInfo(Triple TargetTriple, const DataLayout &DL)
    : TheDataLayout(DL) {
  assert(TargetTriple.getArch() == Triple::x86_64);
  IsWindows = TargetTriple.isOSWindows();
}

void X86_64ABIInfo::computeSignatureInfo(
    LLILCJitContext &Context, CallingConv::ID CC, bool IsManagedCallingConv,
    ABIType ResultType, ArrayRef<ABIType> ArgTypes, ABIArgInfo &ResultInfo,
    std::vector<ABIArgInfo> &ArgInfos) const {
  if (CC == CallingConv::C) {
    CC = IsWindows ? CallingConv::X86_64_Win64 : CallingConv::X86_64_SysV;
  }

  switch (CC) {
  case CallingConv::X86_64_Win64:
    X86_64_Win64::computeSignatureInfo(IsManagedCallingConv, ResultType,
                                       ArgTypes, TheDataLayout, ResultInfo,
                                       ArgInfos);
    break;

  case CallingConv::X86_64_SysV:
    X86_64_SysV::computeSignatureInfo(Context, IsManagedCallingConv, ResultType,
                                      ArgTypes, TheDataLayout, ResultInfo,
                                      ArgInfos);
    break;

  default:
    assert(CC != CallingConv::C);
    assert(false && "Unsupported calling convention");
  }
}

ABIInfo *ABIInfo::get(Module &M) {
  Triple TargetTriple(M.getTargetTriple());

  switch (TargetTriple.getArch()) {
  case Triple::x86_64:
    return new X86_64ABIInfo(TargetTriple, M.getDataLayout());

  default:
    llvm_unreachable("Unsupported architecture");
  }
}

ABIArgInfo::ABIArgInfo(Kind TheKind, Type *TheType)
    : TheKind(TheKind), TheType(TheType) {}

ABIArgInfo::ABIArgInfo(Kind TheKind, ArrayRef<Expansion> Expansions)
    : TheKind(TheKind) {
  this->Expansions = new SmallVector<Expansion, 2>();
  for (const Expansion &Exp : Expansions) {
    this->Expansions->push_back(Exp);
  }
}

ABIArgInfo::ABIArgInfo(ABIArgInfo &&Other) { *this = std::move(Other); }

ABIArgInfo::~ABIArgInfo() {
  if (TheKind == Kind::Expand && Expansions != nullptr) {
    delete Expansions;
  }
}

ABIArgInfo &ABIArgInfo::operator=(ABIArgInfo &&Other) {
  TheKind = Other.TheKind;
  if (TheKind == Kind::Expand) {
    Expansions = Other.Expansions;
    Other.Expansions = nullptr;
  } else {
    TheType = Other.TheType;
  }

  return *this;
}

ABIArgInfo ABIArgInfo::getDirect(llvm::Type *TheType) {
  return ABIArgInfo(Kind::Direct, TheType);
}

ABIArgInfo ABIArgInfo::getExpand(ArrayRef<Expansion> Expansions) {
  return ABIArgInfo(Kind::Expand, Expansions);
}

ABIArgInfo ABIArgInfo::getZeroExtend(llvm::Type *TheType) {
  return ABIArgInfo(Kind::ZeroExtend, TheType);
}

ABIArgInfo ABIArgInfo::getSignExtend(llvm::Type *TheType) {
  return ABIArgInfo(Kind::SignExtend, TheType);
}

ABIArgInfo ABIArgInfo::getIndirect(llvm::Type *TheType) {
  return ABIArgInfo(Kind::Indirect, TheType);
}

ABIArgInfo::Kind ABIArgInfo::getKind() const { return TheKind; }

Type *ABIArgInfo::getType() const { return TheType; }

llvm::ArrayRef<ABIArgInfo::Expansion> ABIArgInfo::getExpansions() const {
  assert(TheKind == Kind::Expand);
  assert(Expansions != nullptr);
  return ArrayRef<Expansion>(*Expansions);
}

void ABIArgInfo::setIndex(uint32_t Index) { this->Index = Index; }

uint32_t ABIArgInfo::getIndex() const { return Index; }

ABIType::ABIType(llvm::Type *TheType, CORINFO_CLASS_HANDLE Class, bool IsSigned)
    : TheType(TheType), Class(Class), IsSigned(IsSigned) {}

llvm::Type *ABIType::getType() const { return TheType; }

CORINFO_CLASS_HANDLE ABIType::getClass() const { return Class; }

bool ABIType::isSigned() const { return IsSigned; }
