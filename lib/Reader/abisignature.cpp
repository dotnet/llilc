//===------------------- include/Reader/abisignature..cpp -------*- C++ -*-===//
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
/// \brief Defines ABI signature abstractions used when lowering functions to
///        LLVM IR.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "reader.h"
#include "readerir.h"
#include "abi.h"
#include "abisignature.h"
#include <cstdint>
#include <cassert>

using namespace llvm;

static CallingConv::ID getLLVMCallingConv(CorInfoCallConv CC) {
  switch (CC) {
  case CORINFO_CALLCONV_STDCALL:
    return CallingConv::X86_StdCall;
  case CORINFO_CALLCONV_THISCALL:
    return CallingConv::X86_ThisCall;
  case CORINFO_CALLCONV_FASTCALL:
    return CallingConv::X86_FastCall;
  default:
    return CallingConv::C;
  }
}

ABISignature::ABISignature(const ReaderCallSignature &Signature, GenIR &Reader,
                           const ABIInfo &TheABIInfo) {
  const CallArgType &ResultType = Signature.getResultType();
  const std::vector<CallArgType> &ArgTypes = Signature.getArgumentTypes();
  const uint32_t NumArgs = ArgTypes.size();

  Type *LLVMResultType = Reader.getType(ResultType.CorType, ResultType.Class);

  SmallVector<Type *, 16> LLVMArgTypes(NumArgs);
  uint32_t I = 0;
  for (const CallArgType &Arg : ArgTypes) {
    LLVMArgTypes[I++] = Reader.getType(Arg.CorType, Arg.Class);
  }

  CallingConv::ID CC = getLLVMCallingConv(Signature.getCallingConvention());
  TheABIInfo.computeSignatureInfo(CC, LLVMResultType, LLVMArgTypes, Result,
                                  Args);

  if (Result.getKind() == ABIArgInfo::Indirect) {
    FuncResultType = Reader.getManagedPointerType(Result.getType());
  } else {
    FuncResultType = Result.getType();
  }
}

Value *ABISignature::coerce(GenIR &Reader, Type *TheType, Value *TheValue) {
  Type *ValueType = TheValue->getType();

  if (TheType == ValueType) {
    return TheValue;
  }

  // TODO: the code spit could probably be better here.
  IRBuilder<> &Builder = *Reader.LLVMBuilder;
  Type *TargetPtrTy = TheType->getPointerTo();
  Value *ValuePtr = (Value *)Reader.addressOfValue((IRNode *)TheValue);
  Value *TargetPtr = Builder.CreatePointerCast(ValuePtr, TargetPtrTy);
  return Builder.CreateLoad(TargetPtr);
}

ABICallSignature::ABICallSignature(const ReaderCallSignature &TheSignature,
                                   GenIR &Reader, const ABIInfo &TheABIInfo)
    : ABISignature(TheSignature, Reader, TheABIInfo), Signature(TheSignature) {}

Value *ABICallSignature::emitCall(GenIR &Reader, llvm::Value *Target,
                                  bool MayThrow, llvm::ArrayRef<Value *> Args,
                                  llvm::Value *IndirectionCell,
                                  llvm::Value **CallNode) const {
  assert(Target->getType()->isIntegerTy(Reader.TargetPointerSizeInBits));

  // Compute the function type
  bool HasIndirectResult = Result.getKind() == ABIArgInfo::Indirect;
  bool HasIndirectionCell = IndirectionCell != nullptr;
  uint32_t NumExtraArgs =
      (HasIndirectResult ? 1 : 0) + (HasIndirectionCell ? 1 : 0);
  int32_t ResultIndex = -1;
  Value *ResultNode = nullptr;
  SmallVector<Type *, 16> ArgumentTypes(Args.size() + NumExtraArgs);
  SmallVector<Value *, 16> Arguments(Args.size() + NumExtraArgs);
  IRBuilder<> &Builder = *Reader.LLVMBuilder;

  // Check for VSD calls.
  if (HasIndirectionCell) {
    // The indirection cell is passed as the first argument. The backend will
    // place it in the appropriate register. The indirection cell should be
    // integer-typed.
    assert(IndirectionCell->getType()->isIntegerTy(
        Reader.TargetPointerSizeInBits));
    ArgumentTypes[0] = IndirectionCell->getType();
    Arguments[0] = IndirectionCell;
  }

  if (HasIndirectResult) {
    ResultIndex = (HasIndirectionCell ? 1 : 0) + (Signature.hasThis() ? 1 : 0);
    ArgumentTypes[ResultIndex] =
        Reader.getUnmanagedPointerType(Result.getType());
    Arguments[ResultIndex] = ResultNode =
        Reader.createTemporary(Result.getType());
  }

  uint32_t I = HasIndirectionCell ? 1 : 0, J = 0;
  for (auto Arg : Args) {
    if (ResultIndex >= 0 && I == (uint32_t)ResultIndex) {
      I++;
    }

    const ABIArgInfo &ArgInfo = this->Args[J];
    Type *ArgType = Arg->getType();

    if (ArgInfo.getKind() == ABIArgInfo::Indirect) {
      // TODO: byval attribute support
      ArgumentTypes[I] = ArgType->getPointerTo();
      Value *Temp = Reader.createTemporary(ArgType);
      Builder.CreateStore(Arg, Temp);
      Arguments[I] = Temp;
    } else {
      ArgumentTypes[I] = ArgInfo.getType();
      Arguments[I] = coerce(Reader, ArgInfo.getType(), Arg);
    }

    I++, J++;
  }

  const bool IsVarArg = false;
  Type *FunctionTy = FunctionType::get(FuncResultType, ArgumentTypes, IsVarArg);
  Type *FunctionPtrTy = Reader.getUnmanagedPointerType(FunctionTy);

  Target = Builder.CreateIntToPtr(Target, FunctionPtrTy);
  CallSite Call = Reader.makeCall(Target, MayThrow, Arguments);

  CallingConv::ID CC;
  if (HasIndirectionCell) {
    assert(Signature.getCallingConvention() == CORINFO_CALLCONV_DEFAULT);
    CC = CallingConv::CLR_VirtualDispatchStub;
  } else {
    CC = getLLVMCallingConv(Signature.getCallingConvention());
  }
  Call.setCallingConv(CC);

  if (ResultNode == nullptr) {
    assert(!HasIndirectResult);
    const CallArgType &SigResultType = Signature.getResultType();
    Type *Ty = Reader.getType(SigResultType.CorType, SigResultType.Class);
    ResultNode = coerce(Reader, Ty, Call.getInstruction());
  } else {
    ResultNode = Builder.CreateLoad(ResultNode);
  }

  *CallNode = Call.getInstruction();
  return ResultNode;
}

ABIMethodSignature::ABIMethodSignature(
    const ReaderMethodSignature &TheSignature, GenIR &Reader,
    const ABIInfo &TheABIInfo)
    : ABISignature(TheSignature, Reader, TheABIInfo), Signature(&TheSignature) {
}

Function *ABIMethodSignature::createFunction(GenIR &Reader, Module &M) {
  // Compute the function type
  LLVMContext &Context = M.getContext();
  bool HasIndirectResult = Result.getKind() == ABIArgInfo::Indirect;
  uint32_t NumExtraArgs = HasIndirectResult ? 1 : 0;
  int32_t ResultIndex = -1;
  SmallVector<Type *, 16> ArgumentTypes(Args.size() + NumExtraArgs);

  if (HasIndirectResult) {
    ResultIndex = Signature->hasThis() ? 1 : 0;
    Result.setIndex((uint32_t)ResultIndex);
    ArgumentTypes[ResultIndex] = Reader.getManagedPointerType(Result.getType());
  }

  uint32_t I = 0;
  for (auto &Arg : Args) {
    if (ResultIndex >= 0 && I == (uint32_t)ResultIndex) {
      I++;
    }

    if (Arg.getKind() == ABIArgInfo::Indirect) {
      // TODO: byval attribute support
      ArgumentTypes[I] = Reader.getManagedPointerType(Arg.getType());
    } else {
      ArgumentTypes[I] = Arg.getType();
    }
    Arg.setIndex(I);

    I++;
  }

  const bool IsVarArg = false;
  FunctionType *FunctionTy =
      FunctionType::get(FuncResultType, ArgumentTypes, IsVarArg);
  Function *F = Function::Create(FunctionTy, Function::ExternalLinkage,
                                 M.getModuleIdentifier(), &M);

  // Use "param" for these initial parameter values. Numbering here
  // is strictly positional (hence includes implicit parameters).
  uint32_t N = 0;
  for (Function::arg_iterator Args = F->arg_begin(); Args != F->arg_end();
       Args++) {
    Args->setName(Twine("param") + Twine(N++));
  }

  CallingConv::ID CC;
  if (Signature->hasSecretParameter()) {
    assert((--F->arg_end())->getType()->isIntegerTy());

    AttributeSet Attrs = F->getAttributes();
    F->setAttributes(
        Attrs.addAttribute(Context, F->arg_size(), "CLR_SecretParameter"));
    CC = CallingConv::CLR_SecretParameter;
  } else {
    CC = CallingConv::C;
  }
  F->setCallingConv(CC);

  if (LLILCJit::TheJit->ShouldInsertStatepoints) {
    F->setGC("statepoint-example");
  }

  return F;
}

const ABIArgInfo &ABIMethodSignature::getResultInfo() const { return Result; }

const ABIArgInfo &ABIMethodSignature::getArgumentInfo(uint32_t I) const {
  assert(I < Args.size());
  return Args[I];
}
