//===---- lib/MSILReader/readerir.cpp ---------------------------*- C++ -*-===//
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

#include "readerir.h"
#include "imeta.h"
#include "newvstate.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Debug.h"       // for dbgs()
#include "llvm/Support/raw_ostream.h" // for errs()
#include "llvm/Support/ConvertUTF.h"  // for ConvertUTF16toUTF8
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
  Stack = (IRNode **)Reader->getTempMemory((size_t)MaxStack * sizeof(IRNode *));
  Top = -1;
  Max = MaxStack - 1;
}

void GenStack::push(IRNode *NewVal, IRNode **NewIR) {

  // Note that in this client, due to inlining and jmp, MaxStack
  // is not a hard and fast value we can count on.  It is possible
  // to contrive cases where the GenIR stack would contain more than
  // MaxStack items, so we need to be prepared to reallocate in those
  // rare circumstances.
  if (Top >= Max) {
    // Reallocate the stack to make some more room.
    uint32_t NewSize = Max * 2 + 2;
    IRNode **NewStack =
        (IRNode **)Reader->getTempMemory((size_t)NewSize * sizeof(IRNode *));
    memcpy(NewStack, Stack, (size_t)(Max + 1) * sizeof(IRNode *));
    Stack = NewStack;
    Max = NewSize - 1;
  }

  IRNode *Result = NewVal;

  ASSERT(GenIR::isValidStackType(Result));

  Stack[++Top] = Result;
}

IRNode *GenStack::pop() {
  ASSERTM(Top >= 0, "stack underflow");
  if (Top < 0)
    LLILCJit::fatal(CORJIT_BADCODE);

  return Stack[Top--];
}

void GenStack::clearStack() { Top = -1; }

bool GenStack::empty() { return (Top == -1); }

void GenStack::assertEmpty() { ASSERT(empty()); }

uint32_t GenStack::depth() { return (uint32_t)(Top + 1); }

IRNode *GenStack::getIterator(ReaderStackIterator **Iterator) {
  *((size_t *)Iterator) = Top;
  if (empty())
    return NULL;
  else
    return (IRNode *)Stack[Top];
}

IRNode *GenStack::iteratorGetNext(ReaderStackIterator **Iterator) {
  size_t *I = (size_t *)Iterator;
  if (*I == 0)
    return NULL;
  return Stack[--(*I)];
}

void GenStack::iteratorReplace(ReaderStackIterator **Iterator, IRNode *Node) {
  Stack[*((size_t *)Iterator)] = Node;
}

IRNode *GenStack::getReverseIterator(ReaderStackIterator **Iterator) {
  return getReverseIteratorFromDepth(Iterator, depth());
}

IRNode *GenStack::getReverseIteratorFromDepth(ReaderStackIterator **Iterator,
                                              uint32_t Depth) {
  size_t Idx = (size_t)(Top - Depth + 1);
  ASSERT(Idx >= 0);
  *((size_t *)Iterator) = Idx;
  if (empty())
    return NULL;
  else
    return (IRNode *)Stack[Idx];
}

IRNode *GenStack::reverseIteratorGetNext(ReaderStackIterator **Iterator) {
  size_t *I = (size_t *)Iterator;
  if (*I == Top)
    return NULL;
  return Stack[++(*I)];
}

#if !defined(NODEBUG)
void GenStack::print() {
  dbgs() << "{GenStack dump, Top first, depth = " << depth() << '\n';
  ReaderStackIterator *Iterator;
  IRNode *N = getIterator(&Iterator);
  int32_t I = 0;
  while (N) {
    dbgs() << "[" << I++ << "]: ";
    Reader->dbPrintIRNode(N);
    N = iteratorGetNext(&Iterator);
  }
  dbgs() << "}\n";
}
#endif

ReaderStack *GenStack::copy() {
  GenStack *Copy;

  void *Buffer = Reader->getTempMemory(sizeof(GenStack));
  Copy = new (Buffer) GenStack(Max + 1, Reader);
  Copy->Top = Top;
  for (int32_t I = 0; I <= Top; I++)
    Copy->Stack[I] = Stack[I];
  return Copy;
}

ReaderStack *GenIR::createStack(uint32_t MaxStack, ReaderBase *Reader) {
  void *Buffer = Reader->getTempMemory(sizeof(GenStack));
  // extra 16 should reduce frequency of reallocation when inlining / jmp
  return new (Buffer) GenStack(MaxStack + 16, Reader);
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

IRNode *rgnGetHead(EHRegion *Region) { return NULL; }

void rgnSetHead(EHRegion *Region, IRNode *Head) { return; }

IRNode *rgnGetLast(EHRegion *Region) { return NULL; }

void rgnSetLast(EHRegion *Region, IRNode *Last) { return; }

bool rgnGetIsLive(EHRegion *Region) { return false; }

void rgnSetIsLive(EHRegion *Region, bool Live) { return; }

void rgnSetParent(EHRegion *Region, EHRegion *Parent) {
  Region->Parent = Parent;
}

EHRegion *rgnGetParent(EHRegion *Region) { return Region->Parent; }

void rgnSetChildList(EHRegion *Region, EHRegionList *Children) {
  Region->Children = Children;
}
EHRegionList *rgnGetChildList(EHRegion *Region) { return Region->Children; }

bool rgnGetHasNonLocalFlow(EHRegion *Region) { return false; }
void rgnSetHasNonLocalFlow(EHRegion *Region, bool NonLocalFlow) { return; }
IRNode *rgnGetEndOfClauses(EHRegion *Region) { return NULL; }
void rgnSetEndOfClauses(EHRegion *Region, IRNode *Node) { return; }
IRNode *rgnGetTryBodyEnd(EHRegion *Region) { return NULL; }
void rgnSetTryBodyEnd(EHRegion *Region, IRNode *Node) { return; }
ReaderBaseNS::TryKind rgnGetTryType(EHRegion *Region) {
  return ReaderBaseNS::TryKind::TRY_None;
}
void rgnSetTryType(EHRegion *Region, ReaderBaseNS::TryKind Type) { return; }
int rgnGetTryCanonicalExitOffset(EHRegion *TryRegion) { return 0; }
void rgnSetTryCanonicalExitOffset(EHRegion *TryRegion, int32_t Offset) {
  return;
}
EHRegion *rgnGetExceptFilterRegion(EHRegion *Region) { return NULL; }
void rgnSetExceptFilterRegion(EHRegion *Region, EHRegion *FilterRegion) {
  return;
}
EHRegion *rgnGetExceptTryRegion(EHRegion *Region) { return NULL; }
void rgnSetExceptTryRegion(EHRegion *Region, EHRegion *TryRegion) { return; }
bool rgnGetExceptUsesExCode(EHRegion *Region) { return false; }
void rgnSetExceptUsesExCode(EHRegion *Region, bool UsesExceptionCode) {
  return;
}
EHRegion *rgnGetFilterTryRegion(EHRegion *Region) { return NULL; }
void rgnSetFilterTryRegion(EHRegion *Region, EHRegion *TryRegion) { return; }
EHRegion *rgnGetFilterHandlerRegion(EHRegion *Region) { return NULL; }
void rgnSetFilterHandlerRegion(EHRegion *Region, EHRegion *Handler) { return; }
EHRegion *rgnGetFinallyTryRegion(EHRegion *FinallyRegion) { return NULL; }
void rgnSetFinallyTryRegion(EHRegion *FinallyRegion, EHRegion *TryRegion) {
  return;
}
bool rgnGetFinallyEndIsReachable(EHRegion *FinallyRegion) { return false; }
void rgnSetFinallyEndIsReachable(EHRegion *FinallyRegion, bool IsReachable) {
  return;
}
EHRegion *rgnGetFaultTryRegion(EHRegion *FaultRegion) { return NULL; }
void rgnSetFaultTryRegion(EHRegion *FaultRegion, EHRegion *TryRegion) {
  return;
}
EHRegion *rgnGetCatchTryRegion(EHRegion *CatchRegion) { return NULL; }
void rgnSetCatchTryRegion(EHRegion *CatchRegion, EHRegion *TryRegion) {
  return;
}
mdToken rgnGetCatchClassToken(EHRegion *CatchRegion) { return 0; }
void rgnSetCatchClassToken(EHRegion *CatchRegion, mdToken Token) { return; }

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

  CORINFO_METHOD_HANDLE MethodHandle = JitContext->MethodInfo->ftn;
  Function = getFunction(MethodHandle);

  // Capture low-level info about the return type for use in Return.
  CORINFO_SIG_INFO Sig;
  getMethodSig(MethodHandle, &Sig);
  ReturnCorType = Sig.retType;

  EntryBlock = BasicBlock::Create(*JitContext->LLVMContext, "entry", Function);

  LLVMBuilder = new IRBuilder<>(*this->JitContext->LLVMContext);
  LLVMBuilder->SetInsertPoint(EntryBlock);

  // Note numArgs may exceed the IL argument count when there
  // are hidden args like the varargs cookie or type descriptor.
  // Since we add these hidden args to the function's type, we can use the
  // type's argument count to get the right number here.
  uint32_t NumArgs = Function->getFunctionType()->getFunctionNumParams();
  ASSERT(NumArgs >= JitContext->MethodInfo->args.totalILArgs());
  uint32_t NumLocals = JitContext->MethodInfo->locals.numArgs;

  LocalVars.resize(NumLocals);
  LocalVarCorTypes.resize(NumLocals);
  Arguments.resize(NumArgs);
  ArgumentCorTypes.resize(NumArgs);
  HasThis = JitContext->MethodInfo->args.hasThis();
  HasTypeParameter = JitContext->MethodInfo->args.hasTypeArg();
  HasVarargsToken = JitContext->MethodInfo->args.isVarArg();
  KeepGenericContextAlive = false;

  initParamsAndAutos(NumArgs, NumLocals);

  // Take note of the current insertion point in case we need
  // to add more allocas later.
  if (EntryBlock->empty()) {
    TempInsertionPoint = nullptr;
  } else {
    TempInsertionPoint = &EntryBlock->back();
  }

  Function::arg_iterator Args = Function->arg_begin();
  Value *CurrentArg;
  int32_t I;
  for (CurrentArg = Args++, I = 0; CurrentArg != Function->arg_end();
       CurrentArg = Args++, I++) {
    if (CurrentArg->getType()->isStructTy()) {
      // LLVM doesn't use the same calling convention as other .Net jits
      // for structs, and we want to be able to select jits per-method
      // so we need them to interoperate.
      throw NotYetImplementedException("Struct parameter");
    }
    LLVMBuilder->CreateStore(CurrentArg, Arguments[I]);
  }

  // Check for special cases where the Jit needs to do extra work.
  const uint32_t MethodFlags = getCurrentMethodAttribs();
  const uint32_t JitFlags = JitContext->Flags;

  // TODO: support for synchronized methods
  if (MethodFlags & CORINFO_FLG_SYNCH) {
    throw NotYetImplementedException("synchronized method");
  }

  // TODO: support for JustMyCode hook
  if ((JitFlags & CORJIT_FLG_DEBUG_CODE) &&
      !(JitFlags & CORJIT_FLG_IL_STUB)) { 

    bool IsIndirect = false;
    void * DebugHandle = 
      getJustMyCodeHandle(getCurrentMethodHandle(), &IsIndirect);

    if (DebugHandle != nullptr) {
      throw NotYetImplementedException("just my code hook");
    }
  }

  // TODO: support for secret parameter for shared IL stubs
  if ((JitFlags & CORJIT_FLG_IL_STUB) &&
      (JitFlags & CORJIT_FLG_PUBLISH_SECRET_PARAM)) {
    throw NotYetImplementedException("publish secret param");
  }

  // TODO: Insert class initialization check if necessary
  CorInfoInitClassResult InitResult =
    initClass(NULL, getCurrentMethodHandle(), getCurrentContext());
  const bool InitClass = InitResult & CORINFO_INITCLASS_USE_HELPER;
  if (InitClass) {
     throw NotYetImplementedException("init class");
  }
}

void GenIR::readerMiddlePass() { return; }

void GenIR::readerPostPass(bool IsImportOnly) {

  // If the generic context must be kept live,
  // insert the necessary code to make it so.
  Value *ContextAddress = nullptr;

  if (KeepGenericContextAlive) {
    CorInfoOptions Options = JitContext->MethodInfo->options;
    if (Options & CORINFO_GENERICS_CTXT_FROM_THIS) {
      ASSERT(HasThis);
      ContextAddress = Arguments[0];
      throw NotYetImplementedException("keep alive generic context: this");
    } else {
      ASSERT(Options & (CORINFO_GENERICS_CTXT_FROM_METHODDESC |
                        CORINFO_GENERICS_CTXT_FROM_METHODTABLE));
      ASSERT(HasTypeParameter);
      ContextAddress = Arguments[HasThis ? (HasVarargsToken ? 2 : 1) : 0];
      throw NotYetImplementedException("keep alive generic context: !this");
    }
  }

  // Cleanup the memory we've been using.
  delete LLVMBuilder;
}

#pragma endregion

#pragma region UTILITIES

//===----------------------------------------------------------------------===//
//
// MSIL Reader Utilities
//
//===----------------------------------------------------------------------===//

// Translate an ArgOrdinal (from MSIL) into an in index
// into the Arguments array.
uint32_t GenIR::argOrdinalToArgIndex(uint32_t ArgOrdinal) {
  bool MightNeedShift = !HasThis || ArgOrdinal > 0;
  if (MightNeedShift) {
    uint32_t Delta = (HasTypeParameter ? 1 : 0) + (HasVarargsToken ? 1 : 0);
    return ArgOrdinal + Delta;
  }

  return ArgOrdinal;
}

// Translate an index into the Arguments array into
// the ordinal used in MSIL.
uint32_t GenIR::argIndexToArgOrdinal(uint32_t ArgIndex) {
  bool MightNeedShift = !HasThis || ArgIndex > 0;
  if (MightNeedShift) {
    uint32_t Delta = (HasTypeParameter ? 1 : 0) + (HasVarargsToken ? 1 : 0);
    ASSERT(ArgIndex >= Delta);
    return ArgIndex - Delta;
  }

  return ArgIndex;
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
    ASSERT(HasThis);
    SymName = "this";
    break;

  case ReaderSpecialSymbolType::Reader_InstParam:
    ASSERT(HasTypeParameter);
    SymName = "$TypeArg";
    break;

  case ReaderSpecialSymbolType::Reader_VarArgsToken:
    ASSERT(HasVarargsToken);
    SymName = "$VarargsToken";
    HasVarargsToken = true;
    break;

  default:
    UseNumber = true;
    if (!IsAuto) {
      Number = argIndexToArgOrdinal(Num);
    }
    break;
  }

  Type *LLVMType = this->getType(CorType, Class);
  AllocaInst *AllocaInst = LLVMBuilder->CreateAlloca(
      LLVMType, NULL,
      UseNumber ? Twine(SymName) + Twine(Number) : Twine(SymName));

  if (IsAuto) {
    LocalVars[Num] = AllocaInst;
    LocalVarCorTypes[Num] = CorType;
  } else {
    Arguments[Num] = AllocaInst;
    ArgumentCorTypes[Num] = CorType;
  }
}

Function *GenIR::getFunction(CORINFO_METHOD_HANDLE MethodHandle) {
  Module *M = JitContext->CurrentModule;
  FunctionType *Ty = getFunctionType(MethodHandle);
  llvm::Function *F = Function::Create(Ty, Function::ExternalLinkage,
                                       M->getModuleIdentifier(), M);

  ASSERT(Ty == F->getFunctionType());

  // Use "param" for these initial parameter values. Numbering here
  // is strictly positional (hence includes implicit parameters).
  uint32_t N = 0;
  for (Function::arg_iterator Args = F->arg_begin(); Args != F->arg_end();
       Args++) {
    Args->setName(Twine("param") + Twine(N++));
  }

  return F;
}

// Return true if this IR node is a reference to the
// original this pointer passed to the method. Can
// conservatively return false.
bool GenIR::objIsThis(IRNode *Obj) { return false; }

// Create a new temporary with the indicated type.
Instruction *GenIR::createTemporary(Type *Ty) {
  // Put the alloca for this temporary into the entry block so
  // the temporary uses can appear anywhere.
  IRBuilder<>::InsertPoint IP = LLVMBuilder->saveIP();

  if (TempInsertionPoint == nullptr) {
    // There are no local, param or temp allocas in the entry block, so set
    // the insertion point to the first point in the block.
    LLVMBuilder->SetInsertPoint(EntryBlock->getFirstInsertionPt());
  } else {
    // There are local, param or temp allocas. TempInsertionPoint refers to
    // the last of them. Set the insertion point to the next instruction since
    // the builder will insert new instructions before the insertion point.
    LLVMBuilder->SetInsertPoint(TempInsertionPoint->getNextNode());
  }

  AllocaInst *AllocaInst = LLVMBuilder->CreateAlloca(Ty);
  // Update the end of the alloca range.
  TempInsertionPoint = AllocaInst;
  LLVMBuilder->restoreIP(IP);

  return AllocaInst;
}

// Get the value of the unmodified this object.
IRNode *GenIR::thisObj(IRNode **NewIR) {
  ASSERT(HasThis);
  Function::arg_iterator Args = Function->arg_begin();
  Value *UnmodifiedThis = Args++;
  return (IRNode *)UnmodifiedThis;
}

// Get the value of the varargs token (aka argList).
IRNode *GenIR::argList(IRNode **NewIR) {
  ASSERT(HasVarargsToken);
  Function::arg_iterator Args = Function->arg_begin();
  if (HasThis) {
    Args++;
  }
  Value *ArgList = Args++;
  return (IRNode *)ArgList;
}

// Get the value of the instantiation parameter (aka type parameter).
IRNode *GenIR::instParam(IRNode **NewIR) {
  ASSERT(HasTypeParameter);
  Function::arg_iterator Args = Function->arg_begin();
  if (HasThis) {
    Args++;
  }
  if (HasVarargsToken) {
    Args++;
  }
  Value *TypeParameter = Args++;
  return (IRNode *)TypeParameter;
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

void ReaderBase::debugError(const char *Filename, unsigned Linenumber,
                            const char *S) {
  assert(0);
  // TODO
  // if (s) JitContext->JitInfo->doAssert(Filename, Linenumber, S);
  // ASSERTNR(UNREACHED);
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

Type *GenIR::getType(CorInfoType CorType, CORINFO_CLASS_HANDLE ClassHandle,
                     bool GetRefClassFields) {
  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;

  switch (CorType) {
  case CorInfoType::CORINFO_TYPE_UNDEF:
    return NULL;

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
    ASSERT(ClassHandle != NULL);
    return getClassType(ClassHandle, true, GetRefClassFields);

  case CorInfoType::CORINFO_TYPE_VALUECLASS:
  case CorInfoType::CORINFO_TYPE_REFANY: {
    ASSERT(ClassHandle != NULL);
    return getClassType(ClassHandle, false, true);
  }

  case CorInfoType::CORINFO_TYPE_PTR:
  case CorInfoType::CORINFO_TYPE_BYREF: {
    ASSERT(ClassHandle != 0);
    bool IsPtr = (CorType == CorInfoType::CORINFO_TYPE_PTR);
    Type *ClassType = NULL;
    CORINFO_CLASS_HANDLE ChildClassHandle = NULL;
    CorInfoType ChildCorType = getChildType(ClassHandle, &ChildClassHandle);
    // LLVM does not allow void*, so use char* instead.
    if (ChildCorType == CORINFO_TYPE_VOID) {
      ASSERT(IsPtr);
      ClassType = getType(CORINFO_TYPE_CHAR, NULL);
    } else if (ChildCorType == CORINFO_TYPE_UNDEF) {
      // Presumably a value class...?
      ClassType = getType(CORINFO_TYPE_VALUECLASS, ClassHandle);
    } else {
      ClassType = getType(ChildCorType, ChildClassHandle);
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
// If GetRefClassFields is false, then we won't fill in the
// field information for ref classes. This is used to avoid
// getting trapped in cycles in the type reference graph.
Type *GenIR::getClassType(CORINFO_CLASS_HANDLE ClassHandle, bool IsRefClass,
                          bool GetRefClassFields) {
  // Check if we've already created a type for this class handle.
  Type *ResultTy = NULL;
  StructType *StructTy = NULL;
  uint32_t ArrayRank = getArrayRank(ClassHandle);
  bool IsArray = ArrayRank > 0;
  CORINFO_CLASS_HANDLE ArrayElementHandle = NULL;
  CorInfoType ArrayElementType = CorInfoType::CORINFO_TYPE_UNDEF;

  // Two different handles can identify the same array: the actual array handle
  // and the handle of its MethodTable. Because of that we have a separate map
  // for arrays with <element type, element handle, array rank> tuple as key.
  if (IsArray) {
    ArrayElementType = getChildType(ClassHandle, &ArrayElementHandle);
    auto MapElement = ArrayTypeMap->find(
        std::make_tuple(ArrayElementType, ArrayElementHandle, ArrayRank));
    if (MapElement != ArrayTypeMap->end()) {
      ResultTy = MapElement->second;
    }
  } else {
    auto MapElement = ClassTypeMap->find(ClassHandle);
    if (MapElement != ClassTypeMap->end()) {
      ResultTy = MapElement->second;
    }
  }

  if (ResultTy != NULL) {
    // See if we can just return this result.
    bool CanReturnCachedType = true;

    if (IsRefClass) {
      // ResultTy should be ptr-to struct.
      ASSERT(ResultTy->isPointerTy());
      Type *ReferentTy = cast<PointerType>(ResultTy)->getPointerElementType();
      ASSERT(ReferentTy->isStructTy());
      StructTy = cast<StructType>(ReferentTy);

      // If we need fields and don't have them yet, we
      // can't return the cached type without doing some
      // work to finish it off.
      if (GetRefClassFields && StructTy->isOpaque()) {
        CanReturnCachedType = false;
      }
    } else {
      // Value classes should be structs and all filled in
      ASSERT(ResultTy->isStructTy());
      ASSERT(!cast<StructType>(ResultTy)->isOpaque());
    }

    if (CanReturnCachedType) {
      return ResultTy;
    }
  }

  // Cache the context and data layout.
  LLVMContext &LLVMContext = *JitContext->LLVMContext;
  const DataLayout *DataLayout = JitContext->EE->getDataLayout();

  // We need to fill in or create a new type for this class.
  if (StructTy == NULL) {
    // Need to create one ... add it to the map now so it's
    // there if we make a recursive request.
    StructTy = StructType::create(LLVMContext);
    ResultTy =
        IsRefClass ? (Type *)getManagedPointerType(StructTy) : (Type *)StructTy;
    if (IsArray) {
      (*ArrayTypeMap)[std::make_tuple(ArrayElementType, ArrayElementHandle,
                                      ArrayRank)] = ResultTy;
    } else {
      (*ClassTypeMap)[ClassHandle] = ResultTy;
    }

    // Fetch the name of this type for use in dumps.
    // Note some constructed types like arrays may not have names.
    int32_t NameSize = 0;
    const bool IncludeNamespace = true;
    const bool FullInst = false;
    const bool IncludeAssembly = false;
    // We are using appendClassName instead of getClassName because
    // getClassName omits namespaces from some types (e.g., nested classes).
    // We may still get the same name for two different structs because
    // two classes with the same fully-qualified names may live in different
    // assemblies. In that case StructType->setName will append a unique suffix
    // to the conflicting name.
    NameSize = appendClassName(NULL, &NameSize, ClassHandle, IncludeNamespace,
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
      if (Result == conversionOK) {
        ASSERT((size_t)(&WideCharBuffer[BufferLength] -
                        (const char16_t *)UTF16Start) == 0);
        StructTy->setName((char *)ClassName);
      }
      delete[] ClassName;
      delete[] WideCharBuffer;
    }
  }

  // Bail out if we just want a placeholder for a ref class.
  // We will fill in details later.
  if (IsRefClass && !GetRefClassFields) {
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

  // Keep track of any ref classes that we deferred
  // examining in detail, so we can come back to them
  // when this class is filled in.
  std::vector<CORINFO_CLASS_HANDLE> DeferredDetailClasses;

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
      // It's always ok to ask for the details of a parent type.
      Type *PointerToParentTy = getClassType(ParentClassHandle, true, true);
      // It's possible that we added the fields to this struct if a parent
      // had fields of this type. In that case we are already done.
      if (!StructTy->isOpaque()) {
        return ResultTy;
      }

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
      ASSERT(FieldHandle != NULL);
      const uint32_t FieldOffset = getFieldOffset(FieldHandle);
      DerivedFields.push_back(std::make_pair(FieldOffset, FieldHandle));
    }

    // Putting offset first in the pair lets us use the
    // default comparator here.
    std::sort(DerivedFields.begin(), DerivedFields.end());

    // Now walk the fields in increasing offset order, adding
    // them and padding to the struct as we go.
    for (const auto &FieldPair : DerivedFields) {
      const uint32_t FieldOffset = FieldPair.first;
      CORINFO_FIELD_HANDLE FieldHandle = FieldPair.second;

      // Bail out for now if we see a union type.
      if (FieldOffset < ByteOffset) {
        ASSERT(IsUnion);
        throw NotYetImplementedException("union types");
      }

      // Account for padding by injecting a field.
      if (FieldOffset > ByteOffset) {
        const uint32_t PadSize = FieldOffset - ByteOffset;
        Type *PadTy = ArrayType::get(Type::getInt8Ty(LLVMContext), PadSize);
        Fields.push_back(PadTy);
        ByteOffset += DataLayout->getTypeSizeInBits(PadTy) / 8;
      }

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

      // Add this field to the collection.
      //
      // If this field is a ref class reference, we don't need the full
      // details on the referred-to class, and asking for the details here
      // causes trouble with certain recursive type graphs, for example:
      //
      // class A { B b; }
      // class B : extends A { int c };
      //
      // We need to know the size of A before we can finish B. So we can't
      // ask for B's details while filling out A.

      ASSERT(FieldOffset == ByteOffset);
      CORINFO_CLASS_HANDLE FieldClassHandle;
      CorInfoType CorInfoType = getFieldType(FieldHandle, &FieldClassHandle);
      bool GetFieldDetails = (CorInfoType != CORINFO_TYPE_CLASS);
      Type *FieldTy = getType(CorInfoType, FieldClassHandle, GetFieldDetails);
      // If we don't get the details now, make sure to ask
      // for them later.
      if (!GetFieldDetails) {
        DeferredDetailClasses.push_back(FieldClassHandle);
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

    // We should have detected unions up above and bailed.
    ASSERT(!IsUnion);

    // If this is a value class, account for any additional end
    // padding that the runtime sees fit to add.
    //
    // We'd like to get the overall size right for
    // other types, but we have no way to check. So we'll have to
    // settle for having their fields at the right offsets.
    if (!IsRefClass) {
      const uint32_t EEClassSize = getClassSize(ClassHandle);
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
    // and an array of elements.
    if (IsArray) {
      // Array length is (u)int32 ....
      Type *ArrayLengthTy = Type::getInt32Ty(LLVMContext);
      Fields.push_back(ArrayLengthTy);
      ByteOffset += DataLayout->getTypeSizeInBits(ArrayLengthTy) / 8;

      // For 64 bit targets there's then a 32 bit pad.
      const uint32_t PointerSize = DataLayout->getPointerSizeInBits();
      if (PointerSize == 64) {
        Type *ArrayPadTy = ArrayType::get(Type::getInt8Ty(LLVMContext), 4);
        Fields.push_back(ArrayPadTy);
        ByteOffset += DataLayout->getTypeSizeInBits(ArrayPadTy) / 8;
      }

      // For arrays of ref classes there's an array element
      // type.
      CORINFO_CLASS_HANDLE ArrayElementHandle = NULL;
      CorInfoType ArrayElementCorTy =
          getChildType(ClassHandle, &ArrayElementHandle);

      if (ArrayElementCorTy == CORINFO_TYPE_CLASS) {
        Type *ArrayElementFieldTy =
            Type::getIntNPtrTy(LLVMContext, TargetPointerSizeInBits);
        Fields.push_back(ArrayElementFieldTy);
        ByteOffset += DataLayout->getTypeSizeInBits(ArrayElementFieldTy) / 8;
      }

      Type *ElementTy = getType(ArrayElementCorTy, ArrayElementHandle);
      // Next comes the array of elements. Nominally 0 size so no
      // ByteOffset update.
      //
      // TODO: There may be some inter-element padding here for arrays
      // of value classes.
      Type *ArrayOfElementTy = ArrayType::get(ElementTy, 0);
      Fields.push_back(ArrayOfElementTy);
    }
  }

  // Install the field list (even if empty) to complete the struct.
  // Since padding is explicit, this is an LLVM packed struct.
  StructTy->setBody(Fields, true /* isPacked */);

  // For value classes we can do further checking and validate
  // against the runtime's view of the class.
  //
  // Note the runtime only gives us size and gc info for value classes so
  // we can't do this more generally.
  if (!IsRefClass) {

    // Verify overall size matches up.
    const uint32_t LLVMClassSize = DataLayout->getTypeSizeInBits(StructTy) / 8;
    const uint32_t EEClassSize = getClassSize(ClassHandle);
    ASSERT(EEClassSize == LLVMClassSize);

    // Verify that the LLVM type contains the same information
    // as the GC field info from the runtime.
    GCLayoutStruct *RuntimeGCInfo = getClassGCLayout(ClassHandle);
    const StructLayout *MainStructLayout =
        DataLayout->getStructLayout(StructTy);
    const uint32_t PointerSize = DataLayout->getPointerSize();

    // Walk through the type in pointer-sized jumps.
    for (uint32_t GCOffset = 0; GCOffset < EEClassSize;
         GCOffset += PointerSize) {
      const bool ExpectGCPointer =
          (RuntimeGCInfo != NULL) &&
          (RuntimeGCInfo->GCLayout[GCOffset / PointerSize] !=
           CorInfoGCType::TYPE_GC_NONE);
      const uint32_t FieldIndex =
          MainStructLayout->getElementContainingOffset(GCOffset);
      Type *FieldTy = StructTy->getStructElementType(FieldIndex);

      // If the field is a value class we need to dive in
      // to its fields and so on, until we reach a primitive type.
      if (FieldTy->isStructTy()) {

        // Prepare to loop through the nesting.
        const StructLayout *OuterStructLayout = MainStructLayout;
        uint32_t OuterOffset = GCOffset;
        uint32_t OuterIndex = FieldIndex;

        while (FieldTy->isStructTy()) {
          // Offset of the Inner class within the outer class
          const uint32_t InnerBaseOffset =
              OuterStructLayout->getElementOffset(OuterIndex);
          // Inner class should start at or before the outer offset
          ASSERT(InnerBaseOffset <= OuterOffset);
          // Determine target offset relative to this inner class.
          const uint32_t InnerOffset = OuterOffset - InnerBaseOffset;
          // Get the inner class layout
          StructType *InnerStructTy = cast<StructType>(FieldTy);
          const StructLayout *InnerStructLayout =
              DataLayout->getStructLayout(InnerStructTy);
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

      const bool IsGCPointer = FieldTy->isPointerTy() &&
                               isManagedPointerType(cast<PointerType>(FieldTy));

      // LLVM's type and the runtime must agree here.
      ASSERT(ExpectGCPointer == IsGCPointer);
    }
  }

  // Now that this class's fields are filled in, go back
  // and fill in the details for those classes we deferred
  // handling earlier.
  for (const auto &DeferredClassHandle : DeferredDetailClasses) {
    // These are ref classes, and we want their details.
    getClassType(DeferredClassHandle, true, true);
  }

  // Return the struct or a pointer to it as requested.
  return ResultTy;
}

// Obtain an LLVM function type from a method handle.
FunctionType *GenIR::getFunctionType(CORINFO_METHOD_HANDLE Method) {
  CORINFO_SIG_INFO Sig;
  getMethodSig(Method, &Sig);
  CORINFO_CLASS_HANDLE Class = getMethodClass(Method);
  FunctionType *Result = getFunctionType(Sig, Class);
  return Result;
}

// Obtain an LLVM function type from a signature.
//
// If the signature has an implicit 'this' parameter,
// ThisClass must be passed in as the appropriate class handle.
FunctionType *GenIR::getFunctionType(CORINFO_SIG_INFO &Sig,
                                     CORINFO_CLASS_HANDLE ThisClass) {
  CorInfoType ReturnType = Sig.retType;
  CORINFO_CLASS_HANDLE ReturnClass = Sig.retTypeClass;
  Type *LLVMReturnType = this->getType(ReturnType, ReturnClass);
  std::vector<Type *> Arguments;

  if (Sig.hasThis()) {
    // We'd better have a valid class handle.
    ASSERT(ThisClass != nullptr);

    // See if the this pointer class is an valueclass
    uint32_t Attribs = getClassAttribs(ThisClass);

    CorInfoType CorType;

    if ((Attribs & CORINFO_FLG_VALUECLASS) == 0) {
      CorType = CORINFO_TYPE_CLASS;
    } else {
      CorType = CORINFO_TYPE_BYREF;
    }

    Type *LLVMArgType = this->getType(CorType, ThisClass);
    Arguments.push_back(LLVMArgType);
  }

  bool IsVarArg = Sig.isVarArg();

  if (IsVarArg) {
    CORINFO_CLASS_HANDLE Class =
        getBuiltinClass(CorInfoClassId::CLASSID_ARGUMENT_HANDLE);
    Type *VarArgCookieType = getType(CORINFO_TYPE_PTR, Class);
    Arguments.push_back(VarArgCookieType);
  }

  bool HasTypeArg = Sig.hasTypeArg();

  if (HasTypeArg) {
    // maybe not the right type... for now just match what we pick in
    // ReaderBase::buildUpParams
    CORINFO_CLASS_HANDLE Class =
        getBuiltinClass(CorInfoClassId::CLASSID_TYPE_HANDLE);
    Type *TypeArgType = getType(CORINFO_TYPE_PTR, Class);
    Arguments.push_back(TypeArgType);
  }

  CORINFO_ARG_LIST_HANDLE NextArg = Sig.args;

  for (uint32_t I = 0; I < Sig.numArgs; ++I) {
    CorInfoType ArgType = CorInfoType::CORINFO_TYPE_UNDEF;
    CORINFO_CLASS_HANDLE Class;
    NextArg = this->argListNext(NextArg, &Sig, &ArgType, &Class);
    Type *LLVMArgType = this->getType(ArgType, Class);
    Arguments.push_back(LLVMArgType);
  }

  FunctionType *FunctionType =
      FunctionType::get(LLVMReturnType, Arguments, IsVarArg);

  return FunctionType;
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
  case Type::TypeID::StructTyID:
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
    return TargetPointerSizeInBits;

  default:
    ASSERT(UNREACHED);
    return 0; // Silence the return value warning
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
  case Type::TypeID::StructTyID:
    // Already a valid stack type.
    break;

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
    ASSERT(Size >= DesiredSize);

    // A convert is needed if we're changing size
    // or implicitly converting int to ptr.
    const bool NeedsTruncation = (Size > DesiredSize);
    const bool NeedsReinterpret =
        ((CorType == CorInfoType::CORINFO_TYPE_PTR) ||
         (CorType == CorInfoType::CORINFO_TYPE_BYREF));

    if (NeedsTruncation) {
      // Hopefully we don't need both....
      ASSERT(!NeedsReinterpret);
      const bool IsSigned = isSigned(CorType);
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
      Type *ResultTy = getType(CorType, NULL);
      Result = (IRNode *)LLVMBuilder->CreateFPCast(Node, ResultTy);
    }

    break;
  }

  case Type::TypeID::PointerTyID: {
    // May need to cast referent types.
    if (Ty != ResultTy) {
      Result = (IRNode *)LLVMBuilder->CreatePointerCast(Node, ResultTy);
    }
    break;
  }

  case Type::TypeID::StructTyID:
    ASSERT(Ty == ResultTy);
    // No conversions possible/necessary.
    break;

  default:
    // An invalid type
    ASSERTNR(UNREACHED);
  }

  return Result;
}

PointerType *GenIR::getManagedPointerType(Type *ElementType) {
  return PointerType::get(ElementType, ManagedAddressSpace);
}

PointerType *GenIR::getUnmanagedPointerType(Type *ElementType) {
  return PointerType::get(ElementType, UnmanagedAddressSpace);
}

bool GenIR::isManagedPointerType(PointerType *PointerType) {
  return PointerType->getAddressSpace() == ManagedAddressSpace;
}

#pragma endregion

#pragma region FLOW GRAPH

//===----------------------------------------------------------------------===//
//
// MSIL READER Flow Graph Support
//
//===----------------------------------------------------------------------===//

EHRegion *fgNodeGetRegion(FlowGraphNode *Node) { return nullptr; }

EHRegion *fgNodeGetRegion(llvm::Function *Function) { return nullptr; }

void fgNodeSetRegion(FlowGraphNode *Node, EHRegion *Region) { return; }

FlowGraphNode *GenIR::fgGetHeadBlock() {
  return ((FlowGraphNode *)&Function->getEntryBlock());
}

FlowGraphNode *GenIR::fgGetTailBlock() {
  return ((FlowGraphNode *)&Function->back());
}

FlowGraphNode *GenIR::makeFlowGraphNode(uint32_t TargetOffset,
                                        EHRegion *Region) {
  FlowGraphNode *Node = (FlowGraphNode *)BasicBlock::Create(
      *JitContext->LLVMContext, "", Function);
  fgNodeSetStartMSILOffset(Node, TargetOffset);
  return Node;
}

bool irNodeIsLabel(IRNode *Node) { return Node->getType()->isLabelTy(); }

IRNode *GenIR::fgMakeBranch(IRNode *LabelNode, IRNode *InsertNode,
                            uint32_t CurrentOffset, bool IsConditional,
                            bool IsNominal) {
  LLVMBuilder->SetInsertPoint((BasicBlock *)InsertNode);
  BranchInst *BranchInst = NULL;
  if (IsConditional) {
    // Fake condition. The real condition will be inserted when
    // processing basic blocks.
    unsigned NumBits = 1;
    bool IsSigned = false;
    ConstantInt *ZeroConst =
        ConstantInt::get(*JitContext->LLVMContext, APInt(NumBits, 0, IsSigned));
    BranchInst =
        LLVMBuilder->CreateCondBr(ZeroConst, (BasicBlock *)LabelNode, NULL);
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
  BasicBlock *NewBlock;
  if (Inst == NULL) {
    NewBlock = BasicBlock::Create(*JitContext->LLVMContext, "", Function,
      TheBasicBlock->getNextNode());
    TerminatorInst *TermInst = TheBasicBlock->getTerminator();
    if (TermInst != NULL) {
      if (isa<UnreachableInst>(TermInst)) {
        // do nothing
      } else {
        BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
        if (BranchInstruction != NULL) {
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
    NewBlock = TheBasicBlock->splitBasicBlock(Inst);
  }
  return (FlowGraphNode *)NewBlock;
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
  return (IRNode *)LLVMBuilder->CreateSwitch(loadNull(nullptr),
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

IRNode *GenIR::fgMakeEndFinally(IRNode *InsertNode, uint32_t CurrentOffset,
                                bool IsLexicalEnd) {
  // TODO: figure out what (if any) marker we need to generate here
  return NULL;
}

void GenIR::beginFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrOffset,
                               bool IsVerifyOnly) {
  BasicBlock *Block = (BasicBlock *)Fg;
  TerminatorInst *TermInst = Block->getTerminator();
  if (TermInst != NULL) {
    LLVMBuilder->SetInsertPoint(TermInst);
  } else {
    LLVMBuilder->SetInsertPoint(Block);
  }
}

void GenIR::endFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrOffset,
                             IRNode **NewIR) {
  return;
}

IRNode *GenIR::findBlockSplitPointAfterNode(IRNode *Node) {
  if (Node == NULL) {
    return NULL;
  }
  return (IRNode *)((Instruction *)Node)->getNextNode();
}

// Get the last non-placekeeping node in block
IRNode *fgNodeGetEndInsertIRNode(FlowGraphNode *FgNode) {
  BasicBlock *Block = (BasicBlock *)FgNode;
  if (Block->empty()) {
    return NULL;
  } else {
    return (IRNode *)&(((BasicBlock *)FgNode)->back());
  }
}

void GenIR::replaceFlowGraphNodeUses(FlowGraphNode *OldNode,
                                     FlowGraphNode *NewNode) {
  BasicBlock *OldBlock = (BasicBlock *)OldNode;
  OldBlock->replaceAllUsesWith(NewNode);
  OldBlock->eraseFromParent();
}

bool fgEdgeListIsNominal(FlowGraphEdgeList *FgEdge) {
  // This is supposed to return true for exception edges.
  return false;
}

// Hook called from reader fg builder to identify potential inline candidates.
bool GenIR::fgCall(ReaderBaseNS::OPCODE Opcode, mdToken Token,
                   mdToken ConstraintToken, unsigned MsilOffset, IRNode *Block,
                   bool CanInline, bool IsTailCall, bool IsUnmarkedTailCall,
                   bool ReadOnly) {
  return false;
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
  while (nullptr != Idom &&
         fgNodeGetRegion(Idom) != fgNodeGetRegion(Function) &&
         fgNodeGetRegion(Idom) != fgNodeGetRegion(FgNode)) {
    Idom = getNextIDom(Idom);
  }

  return Idom;
}

FlowGraphEdgeList *fgNodeGetSuccessorList(FlowGraphNode *FgNode) {
  FlowGraphEdgeList *FgEdge = new FlowGraphSuccessorEdgeList(FgNode);
  if (fgEdgeListGetSink(FgEdge) == NULL) {
    return NULL;
  }
  return FgEdge;
}

FlowGraphEdgeList *fgEdgeListGetNextSuccessor(FlowGraphEdgeList *FgEdge) {
  FgEdge->moveNext();
  if (fgEdgeListGetSink(FgEdge) == NULL) {
    return NULL;
  }
  return FgEdge;
}

FlowGraphEdgeList *fgNodeGetPredecessorList(FlowGraphNode *Fg) {
  FlowGraphEdgeList *FgEdge = new FlowGraphPredecessorEdgeList(Fg);
  if (fgEdgeListGetSource(FgEdge) == NULL) {
    return NULL;
  }
  return FgEdge;
}

FlowGraphEdgeList *fgEdgeListGetNextPredecessor(FlowGraphEdgeList *FgEdge) {
  FgEdge->moveNext();
  if (fgEdgeListGetSource(FgEdge) == NULL) {
    return NULL;
  }
  return FgEdge;
}

FlowGraphNode *fgEdgeListGetSink(FlowGraphEdgeList *FgEdge) {
  return FgEdge->getSink();
}

FlowGraphNode *fgEdgeListGetSource(FlowGraphEdgeList *FgEdge) {
  return FgEdge->getSource();
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

FlowGraphNode *GenIR::fgNodeGetNext(FlowGraphNode *FgNode) {
  if (FgNode == &(Function->getBasicBlockList().back())) {
    return NULL;
  } else {
    return (FlowGraphNode *)((BasicBlock *)FgNode)->getNextNode();
  }
}

FlowGraphNode *GenIR::fgPrePhase(FlowGraphNode *Fg) { return Fg; }

void GenIR::fgPostPhase() { return; }

void GenIR::fgAddLabelToBranchList(IRNode *LabelNode, IRNode *BranchNode) {
  return;
}

void GenIR::insertIBCAnnotations() { return; }

IRNode *GenIR::fgNodeFindStartLabel(FlowGraphNode *Block) { return NULL; }

bool GenIR::fgBlockHasFallThrough(FlowGraphNode *Block) { return false; }

#pragma endregion

#pragma region MSIL OPCODES

//===----------------------------------------------------------------------===//
//
// MSIL READER opcode to LLVM IR translation
//
//===----------------------------------------------------------------------===//

IRNode *GenIR::loadConstantI4(int32_t Constant, IRNode **NewIR) {
  uint32_t NumBits = 32;
  bool IsSigned = true;

  return (IRNode *)ConstantInt::get(*JitContext->LLVMContext,
                                    APInt(NumBits, Constant, IsSigned));
}

IRNode *GenIR::loadConstantI8(int64_t Constant, IRNode **NewIR) {
  uint32_t NumBits = 64;
  bool IsSigned = true;

  return (IRNode *)ConstantInt::get(*JitContext->LLVMContext,
                                    APInt(NumBits, Constant, IsSigned));
}

IRNode *GenIR::loadConstantI(size_t Constant, IRNode **NewIR) {
  uint32_t NumBits = TargetPointerSizeInBits;
  bool IsSigned = true;
  return (IRNode *)ConstantInt::get(*JitContext->LLVMContext,
                                    APInt(NumBits, Constant, IsSigned));
}

IRNode *GenIR::loadConstantR4(float Value, IRNode **NewIR) {
  return (IRNode *)ConstantFP::get(*JitContext->LLVMContext, APFloat(Value));
}

IRNode *GenIR::loadConstantR8(double Value, IRNode **NewIR) {
  return (IRNode *)ConstantFP::get(*JitContext->LLVMContext, APFloat(Value));
}

// Load the array length field.
IRNode *GenIR::loadLen(IRNode *Address, IRNode **NewIR) {
  // Validate address is ptr to struct.
  Type *AddressTy = Address->getType();
  ASSERT(AddressTy->isPointerTy());
  Type *ArrayTy = cast<PointerType>(AddressTy)->getPointerElementType();
  ASSERT(ArrayTy->isStructTy());

  // TODO: verify this struct looks like an array... field index 1 is at
  // offset 4 with type i32; last "field" is zero sized array.

  // Length field is at field index 1. Get its address.
  Value *LengthFieldAddress = LLVMBuilder->CreateStructGEP(Address, 1);

  // Load and return the length.
  // TODO: this load cannot be aliased.
  Value *Length = LLVMBuilder->CreateLoad(LengthFieldAddress);

  // Result is an unsigned native int.
  IRNode *Result = convertToStackType((IRNode *)Length,
                                      CorInfoType::CORINFO_TYPE_NATIVEUINT);
  return (IRNode *)Result;
}

// Load the string length field.
IRNode *GenIR::loadStringLen(IRNode *Address, IRNode **NewIR) {
  // Validate address is ptr to struct.
  Type *AddressTy = Address->getType();
  ASSERT(AddressTy->isPointerTy());
  Type *StringTy = cast<PointerType>(AddressTy)->getPointerElementType();
  ASSERT(StringTy->isStructTy());

  // Verify this type is a string.
  StringRef StringName = cast<StructType>(StringTy)->getStructName();
  ASSERT(StringName.startswith("System.String"));

  // Length field is at field index 1. Get its address.
  Value *LengthFieldAddress = LLVMBuilder->CreateStructGEP(Address, 1);

  // Load and return the length.
  // TODO: this load cannot be aliased.
  Value *Length = LLVMBuilder->CreateLoad(LengthFieldAddress);
  return (IRNode *)Length;
}

// Load a character from a string.
IRNode *GenIR::stringGetChar(IRNode *Address, IRNode *Index, IRNode **NewIR) {
  // Validate address is ptr to struct.
  Type *AddressTy = Address->getType();
  ASSERT(AddressTy->isPointerTy());
  Type *StringTy = cast<PointerType>(AddressTy)->getPointerElementType();
  ASSERT(StringTy->isStructTy());

  // Verify this type is a string.
  StringRef StringName = cast<StructType>(StringTy)->getStructName();
  ASSERT(StringName.startswith("System.String"));

  // Cache the context
  LLVMContext &Context = *JitContext->LLVMContext;

  // Build up gep indices.
  Value *Indexes[] = {ConstantInt::get(Type::getInt32Ty(Context), 0),
                      ConstantInt::get(Type::getInt32Ty(Context), 2), Index};

  // Index to the desired char.
  Value *CharAddress = LLVMBuilder->CreateInBoundsGEP(Address, Indexes);

  // Load and return the char.
  Value *Char = LLVMBuilder->CreateLoad(CharAddress);
  IRNode *Result =
      convertToStackType((IRNode *)Char, CorInfoType::CORINFO_TYPE_CHAR);

  return Result;
}

IRNode *GenIR::loadNull(IRNode **NewIR) {
  Type *NullType =
      getManagedPointerType(Type::getInt8Ty(*JitContext->LLVMContext));
  return (IRNode *)Constant::getNullValue(NullType);
}

IRNode *GenIR::unaryOp(ReaderBaseNS::UnaryOpcode Opcode, IRNode *Arg1,
                       IRNode **NewIR) {

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
  return NULL;
}

IRNode *GenIR::binaryOp(ReaderBaseNS::BinaryOpcode Opcode, IRNode *Arg1,
                        IRNode *Arg2, IRNode **NewIR) {

  struct BinaryTriple {
    Instruction::BinaryOps Opcode;
    bool IsOverflow;
    bool IsUnsigned;
  };

  static const BinaryTriple IntMap[ReaderBaseNS::LastBinaryOpcode] = {
      {Instruction::BinaryOps::Add, false, false},  // ADD
      {Instruction::BinaryOps::Add, true, false},   // ADD_OVF
      {Instruction::BinaryOps::Add, true, true},    // ADD_OVF_UN
      {Instruction::BinaryOps::And, false, false},  // AND
      {Instruction::BinaryOps::SDiv, false, false}, // DIV
      {Instruction::BinaryOps::UDiv, false, true},  // DIV_UN
      {Instruction::BinaryOps::Mul, false, false},  // MUL
      {Instruction::BinaryOps::Mul, true, false},   // MUL_OVF
      {Instruction::BinaryOps::Mul, true, true},    // MUL_OVF_UN
      {Instruction::BinaryOps::Or, false, false},   // OR
      {Instruction::BinaryOps::SRem, false, false}, // REM
      {Instruction::BinaryOps::URem, false, true},  // REM_UN
      {Instruction::BinaryOps::Sub, false, false},  // SUB
      {Instruction::BinaryOps::Sub, true, false},   // SUB_OVF
      {Instruction::BinaryOps::Sub, true, true},    // SUB_OVF_UN
      {Instruction::BinaryOps::Xor, false, false}   // XOR
  };

  static const BinaryTriple FloatMap[ReaderBaseNS::LastBinaryOpcode] = {
      {Instruction::BinaryOps::FAdd, false, false}, // ADD
      {Instruction::BinaryOps::FAdd, true, false},  // ADD_OVF
      {Instruction::BinaryOps::FAdd, true, true},   // ADD_OVF_UN
      {Instruction::BinaryOps::And, false, false},  // AND
      {Instruction::BinaryOps::FDiv, false, false}, // DIV
      {Instruction::BinaryOps::FDiv, false, true},  // DIV_UN
      {Instruction::BinaryOps::FMul, false, false}, // MUL
      {Instruction::BinaryOps::FMul, true, false},  // MUL_OVF
      {Instruction::BinaryOps::FMul, true, true},   // MUL_OVF_UN
      {Instruction::BinaryOps::Or, false, false},   // OR
      {Instruction::BinaryOps::FRem, false, false}, // REM
      {Instruction::BinaryOps::FRem, false, true},  // REM_UN
      {Instruction::BinaryOps::FSub, false, false}, // SUB
      {Instruction::BinaryOps::FSub, true, false},  // SUB_OVF
      {Instruction::BinaryOps::FSub, true, true},   // SUB_OVF_UN
      {Instruction::BinaryOps::Xor, false, false}   // XOR
  };

  Type *Type1 = Arg1->getType();
  Type *Type2 = Arg2->getType();
  Type *ResultType = binaryOpType(Type1, Type2);

  // If the result is a pointer, see if we have simple
  // pointer + int op...
  if (ResultType->isPointerTy()) {
    if (Opcode == ReaderBaseNS::Add) {
      IRNode *PtrAdd = genPointerAdd(Arg1, Arg2);
      if (PtrAdd != NULL) {
        return PtrAdd;
      }
    } else if (Opcode == ReaderBaseNS::Sub) {
      IRNode *PtrSub = genPointerSub(Arg1, Arg2);
      if (PtrSub != NULL) {
        return PtrSub;
      }
    }
  }

  bool IsFloat = ResultType->isFloatingPointTy();
  const BinaryTriple *Triple = IsFloat ? FloatMap : IntMap;

  bool IsOverflow = Triple[Opcode].IsOverflow;

  if (IsOverflow) {
    throw NotYetImplementedException("BinaryOp Overflow");
  }

  bool IsUnsigned = Triple[Opcode].IsUnsigned;

  Instruction::BinaryOps Op = Triple[Opcode].Opcode;

  if (Type1 != ResultType) {
    Arg1 = convert(ResultType, Arg1, !IsUnsigned);
  }

  if (Type2 != ResultType) {
    Arg2 = convert(ResultType, Arg2, !IsUnsigned);
  }

  IRNode *Result;
  if (IsFloat && Op == Instruction::BinaryOps::FRem) {
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

    Result = callHelperImpl(Helper, ResultType, NewIR, Arg1, Arg2);
  } else {
    Result = (IRNode *)LLVMBuilder->CreateBinOp(Op, Arg1, Arg2);
  }
  return Result;
}

Type *GenIR::binaryOpType(Type *Type1, Type *Type2) {
  if (Type1->isPointerTy()) {
    if (Type2->isPointerTy()) {
      return Type::getIntNTy(*this->JitContext->LLVMContext,
                             TargetPointerSizeInBits);
    }
    ASSERTNR(!Type2->isFloatingPointTy());
    return Type1;
  } else if (Type2->isPointerTy()) {
    ASSERTNR(!Type1->isFloatingPointTy());
    return Type2;
  }

  if (Type1 == Type2) {
    return Type1;
  }

  uint32_t Size1 = Type1->getPrimitiveSizeInBits();
  uint32_t Size2 = Type2->getPrimitiveSizeInBits();

  if (Type1->isFloatingPointTy()) {
    if (Type2->isFloatingPointTy()) {
      return Size1 > Size2 ? Type1 : Type2;
    }
    return Type1;
  } else if (Type2->isFloatingPointTy()) {
    return Type2;
  }

  if (Size1 > Size2) {
    return Type1;
  } else {
    return Type2;
  }
}

// Handle simple field access via a structural GEP.
IRNode *GenIR::simpleFieldAddress(IRNode *BaseAddress,
                                  CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                  CORINFO_FIELD_INFO *FieldInfo,
                                  IRNode **NewIR) {
  // Determine field index and referent type.
  CORINFO_FIELD_HANDLE FieldHandle = ResolvedToken->hField;
  Type *BaseAddressTy = BaseAddress->getType();
  ASSERT(BaseAddressTy->isPointerTy());
  Type *BaseObjTy = cast<PointerType>(BaseAddressTy)->getElementType();
  Value *Address = NULL;

  if (BaseObjTy->isStructTy() &&
      (FieldIndexMap->find(FieldHandle) != FieldIndexMap->end())) {

    const uint32_t FieldIndex = (*FieldIndexMap)[FieldHandle];
    StructType *BaseObjStructTy = cast<StructType>(BaseObjTy);

    // Double-check that the field index is sensible. Note
    // in unverifiable IL we may not have proper referent types and
    // so may see what appear to be unrelated field accesses.
    if (BaseObjStructTy->getNumElements() >= FieldIndex) {
      const DataLayout *DataLayout = JitContext->EE->getDataLayout();
      const StructLayout *StructLayout =
          DataLayout->getStructLayout(BaseObjStructTy);
      const uint32_t FieldOffset = StructLayout->getElementOffset(FieldIndex);
      ASSERT(FieldOffset == FieldInfo->offset);

      Address = LLVMBuilder->CreateStructGEP(BaseAddress, FieldIndex);
    }
  }

  if (Address == NULL) {
    // We can't find the struct type or the field index, or the field index
    // doesn't make sense for the referent type we have on hand.
    // It can happen, for example, if we cast native int pointer to
    // IntPtr pointer. Unfortunately we can't get the enclosing type
    // via ICorJitInfo interface so we can't create a struct version of GEP.

    Address = binaryOp(ReaderBaseNS::Add, BaseAddress,
                       loadConstantI(FieldInfo->offset, NewIR), NewIR);
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
    return NULL;
  }

  // Swap base and offset if we got it wrong.
  if (Arg2IsPointer) {
    BasePtr = Arg2;
    Offset = Arg1;
  }

  // Bail if offset is not integral.
  Type *OffsetTy = Offset->getType();
  if (!OffsetTy->isIntegerTy()) {
    return NULL;
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
    return NULL;
  }

  // Bail if offset is not integral.
  Type *OffsetTy = Offset->getType();
  if (!OffsetTy->isIntegerTy()) {
    return NULL;
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
                       ReaderAlignType Alignment, bool IsVolatile,
                       IRNode **NewIR) {
  uint32_t LocalIndex = LocalOrdinal;
  Value *LocalAddress = LocalVars[LocalIndex];
  Type *LocalTy = LocalAddress->getType()->getPointerElementType();
  IRNode *Value =
      convertFromStackType(Arg1, LocalVarCorTypes[LocalIndex], LocalTy);
  LLVMBuilder->CreateStore(Value, LocalAddress);
}

IRNode *GenIR::loadLocal(uint32_t LocalOrdinal, IRNode **NewIR) {
  uint32_t LocalIndex = LocalOrdinal;
  Value *LocalAddress = LocalVars[LocalIndex];
  IRNode *Value = (IRNode *)LLVMBuilder->CreateLoad(LocalAddress);
  IRNode *Result = convertToStackType(Value, LocalVarCorTypes[LocalIndex]);
  return Result;
}

IRNode *GenIR::loadLocalAddress(uint32_t LocalOrdinal, IRNode **NewIR) {
  uint32_t LocalIndex = LocalOrdinal;
  return loadManagedAddress(LocalVars, LocalIndex);
}

void GenIR::storeArg(uint32_t ArgOrdinal, IRNode *Arg1,
                     ReaderAlignType Alignment, bool IsVolatile,
                     IRNode **NewIR) {
  uint32_t ArgIndex = argOrdinalToArgIndex(ArgOrdinal);
  Value *ArgAddress = Arguments[ArgIndex];
  Type *ArgTy = ArgAddress->getType()->getPointerElementType();
  IRNode *Value = convertFromStackType(Arg1, ArgumentCorTypes[ArgIndex], ArgTy);
  LLVMBuilder->CreateStore(Value, ArgAddress);
}

IRNode *GenIR::loadArg(uint32_t ArgOrdinal, bool IsJmp, IRNode **NewIR) {
  if (IsJmp) {
    throw NotYetImplementedException("JMP");
  }
  uint32_t ArgIndex = argOrdinalToArgIndex(ArgOrdinal);
  Value *ArgAddress = Arguments[ArgIndex];
  IRNode *Value = (IRNode *)LLVMBuilder->CreateLoad(ArgAddress);
  IRNode *Result = convertToStackType(Value, ArgumentCorTypes[ArgIndex]);
  return Result;
}

IRNode *GenIR::loadArgAddress(uint32_t ArgOrdinal, IRNode **NewIR) {
  uint32_t ArgIndex = argOrdinalToArgIndex(ArgOrdinal);
  return loadManagedAddress(Arguments, ArgIndex);
}

IRNode *
GenIR::loadManagedAddress(const std::vector<Value *> &UnmanagedAddresses,
                          uint32_t Index) {
  Value *UnmanagedAddress = UnmanagedAddresses[Index];
  Type *ElementType = UnmanagedAddress->getType()->getPointerElementType();
  Type *ManagedPointerType = getManagedPointerType(ElementType);

  // ldloca and ldarga have to return managed pointers. Since we can't influence
  // the address space of the pointer alloca returns we have to add an
  // AddrSpaceCast to ManagedPointerType here. Normally we try to avoid such
  // casts.
  return (IRNode *)LLVMBuilder->CreateAddrSpaceCast(UnmanagedAddress,
                                                    ManagedPointerType);
}

// Load the address of the field described by pResolvedToken
// from the object Obj.
IRNode *GenIR::loadFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                IRNode *Obj, IRNode **NewIR) {
  bool ObjIsThis = objIsThis(Obj);
  CORINFO_FIELD_INFO FieldInfo;

  // TODO: optimize 'this' pointer reference for contextful classes

  int32_t AccessFlags = ObjIsThis ? CORINFO_ACCESS_THIS : CORINFO_ACCESS_ANY;
  AccessFlags |= CORINFO_ACCESS_ADDRESS;

  getFieldInfo(ResolvedToken, (CORINFO_ACCESS_FLAGS)AccessFlags, &FieldInfo);

  IRNode *Result = getFieldAddress(ResolvedToken, &FieldInfo, Obj, true, NewIR);

  return Result;
}

// Get the address of the field described by pResolvedToken
// from the object Obj. Optionally null check.
IRNode *GenIR::getFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               CORINFO_FIELD_INFO *FieldInfo, IRNode *Obj,
                               bool MustNullCheck, IRNode **NewIR) {
  // Get the field address.
  Type *AddressTy = Obj->getType();
  ASSERT(AddressTy->isPointerTy());
  const bool IsGcPointer = isManagedPointerType(cast<PointerType>(AddressTy));
  Value *RawAddress = rdrGetFieldAddress(ResolvedToken, FieldInfo, Obj,
                                         IsGcPointer, MustNullCheck, NewIR);

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
                         ReaderAlignType AlignmentPrefix, bool IsVolatile,
                         IRNode **NewIR) {
  // Gather relevant facts about this field access.
  bool ObjIsThis = objIsThis(Obj);
  int32_t AccessFlags = ObjIsThis ? CORINFO_ACCESS_THIS : CORINFO_ACCESS_ANY;
  AccessFlags |= CORINFO_ACCESS_GET;
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(ResolvedToken, (CORINFO_ACCESS_FLAGS)AccessFlags, &FieldInfo);

  // LoadStaticField and GetFieldAddress already generate
  // checkFieldAuthorization calls, so
  // only put them in the paths that terminate other ways.

  // It's legal to use LDFLD on a static field. In that case,
  // we need to make sure that we evaluate the object address for
  // side-effects, but then we treat it like LDSFLD.
  if (FieldInfo.fieldFlags & CORINFO_FLG_FIELD_STATIC) {
    pop(Obj, NewIR);

    // TODO: check that unaligned load from static field is illegal.
    return loadStaticField(ResolvedToken, IsVolatile, NewIR);
  }

  // Determine the type of the field element.
  CorInfoType CorInfoType = FieldInfo.fieldType;
  CORINFO_CLASS_HANDLE Class = FieldInfo.structType;
  Type *FieldTy = getType(CorInfoType, Class);

  // Fields typed as GC pointers are always aligned,
  // so ignore any smaller alignment prefix
  if (FieldTy->isPointerTy() &&
      isManagedPointerType(cast<PointerType>(FieldTy))) {
    AlignmentPrefix = Reader_AlignNatural;
  }

  // If accessing the field requires a helper, then we need to
  // call the helper routine; we can't just load the address
  // and do a load-indirect off it.
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INSTANCE_HELPER) {
    throw NotYetImplementedException("LoadField via helper");
  }

  // The operand on top of the stack may be the address of the
  // valuetype, or it could be an instance of the valuetype. If
  // it's an instance, we need to get its address.
  //
  // Some C# compiler versions used ldfld on floats and doubles.
  // Tolerate this by getting the address instead of the value.
  Type *AddressTy = Obj->getType();

  if (AddressTy->isStructTy() || AddressTy->isFloatingPointTy()) {
    Obj = addressOfLeaf(Obj, NewIR);
  }

  if (CorInfoType == CORINFO_TYPE_VALUECLASS ||
      CorInfoType == CORINFO_TYPE_REFANY) {
    AlignmentPrefix = getMinimumClassAlignment(Class, AlignmentPrefix);
  }

  IRNode *Address =
      getFieldAddress(ResolvedToken, &FieldInfo, Obj, false, NewIR);

  if (FieldTy->isStructTy()) {
    return loadObj(ResolvedToken, Address, AlignmentPrefix, IsVolatile, true,
                   NewIR);
  } else {
    LoadInst *LoadInst = LLVMBuilder->CreateLoad(Address, IsVolatile);
    uint32_t Align = (AlignmentPrefix == Reader_AlignNatural)
                         ? TargetPointerSizeInBits / 8
                         : AlignmentPrefix;
    LoadInst->setAlignment(Align);

    IRNode *Result = convertToStackType((IRNode *)LoadInst, CorInfoType);

    return Result;
  }
}

void GenIR::storeField(CORINFO_RESOLVED_TOKEN *FieldToken, IRNode *ValueToStore,
                       IRNode *Object, ReaderAlignType Alignment,
                       bool IsVolatile, IRNode **NewIR) {
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
    storeStaticField(FieldToken, ValueToStore, IsVolatile, NewIR);
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
    handleMemberAccess(FieldInfo.accessAllowed, FieldInfo.accessCalloutHelper,
                       NewIR);

    throw NotYetImplementedException("store field via helper");
    return;
  }

  // Otherwise, obtain the address of the field.
  IRNode *Address =
      getFieldAddress(FieldToken, &FieldInfo, Object, false, NewIR);

  // Stores might require write barriers. If so, call the appropriate
  // helper method.
  const bool NeedsWriteBarrier =
      JitContext->JitInfo->isWriteBarrierHelperRequired(FieldHandle);
  if (NeedsWriteBarrier) {
    rdrCallWriteBarrierHelper(Address, ValueToStore, Alignment, IsVolatile,
                              NewIR, FieldToken, !IsStructTy, false, true,
                              false);
    return;
  }

  // We do things differently based on whether the field is a value class.
  if (!IsStructTy) {
    makeStore(FieldTy, Address, ValueToStore, IsVolatile, NewIR);
    return;
  } else {
    // The WVM lowerer cannot handle multi-byte indirs whose base pointer
    // is the address of a field.
    if (FieldCorType == CORINFO_TYPE_VALUECLASS ||
        FieldCorType == CORINFO_TYPE_REFANY) {
      Alignment = getMinimumClassAlignment(FieldClassHandle, Alignment);
    }
    storeObj(FieldToken, ValueToStore, Address, Alignment, IsVolatile, true,
             NewIR);
    return;
  }
}

void GenIR::storePrimitiveType(IRNode *Value, IRNode *Addr,
                               CorInfoType CorInfoType,
                               ReaderAlignType Alignment, bool IsVolatile,
                               IRNode **NewIR) {
  ASSERTNR(isPrimitiveType(CorInfoType));

  // Get type of the result.
  Type *AddressTy = Addr->getType();
  ASSERT(AddressTy->isPointerTy());
  PointerType *Ty = cast<PointerType>(AddressTy);
  Type *ReferentTy = Ty->getPointerElementType();
  IRNode *TypedAddr = Addr;

  // We need to cast the address when types are mismatched.
  Type *ExpectedTy = Value->getType();
  if (ReferentTy != ExpectedTy) {
    Type *PtrToExpectedTy = getUnmanagedPointerType(ExpectedTy);
    TypedAddr =
      (IRNode *)LLVMBuilder->CreatePointerCast(Addr, PtrToExpectedTy);
  }

  uint32_t Align = (Alignment == Reader_AlignNatural)
    ? TargetPointerSizeInBits / 8
    : Alignment;
  StoreInst *StoreInst = LLVMBuilder->CreateStore(Value, TypedAddr, IsVolatile);
  StoreInst->setAlignment(Align);
}

// Helper used by StorePrimitive and StoreField.
void GenIR::makeStore(Type *Ty, Value *Address, Value *ValueToStore,
                      bool IsVolatile, IRNode **NewIR) {
  if (IsVolatile) {
    // TODO: There is a JitConfig call back which can alter
    // how volatile stores are handled.
    throw NotYetImplementedException("Volatile store");
  }

  LLVMBuilder->CreateStore(ValueToStore, Address, IsVolatile);
}

void GenIR::storeStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                             IRNode *ValueToStore,
                             bool IsVolatile, IRNode **NewIR) {
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
  Value *Address = rdrGetStaticFieldAddress(FieldToken, &FieldInfo, NewIR);

  // If the runtime asks us to use a helper for the store, do so.
  const bool NeedsWriteBarrier =
    JitContext->JitInfo->isWriteBarrierHelperRequired(FieldHandle);
  if (NeedsWriteBarrier) {
    // Statics are always on the heap, so we can use an unchecked write barrier
    rdrCallWriteBarrierHelper((IRNode*)Address, ValueToStore,
                              Reader_AlignNatural, IsVolatile, NewIR,
                              FieldToken, !IsStructTy,
                              false, true, true);
    return;
  }

  Type *PtrToFieldTy = getUnmanagedPointerType(FieldTy);
  if (Address->getType()->isIntegerTy()) {
    Address = LLVMBuilder->CreateIntToPtr(Address, PtrToFieldTy);
  } else {
    ASSERT(Address->getType()->isPointerTy());
    Address = LLVMBuilder->CreatePointerCast(Address, PtrToFieldTy);
  }

  // Create an assignment which stores the value into the static field.
  if (!IsStructTy) {
    makeStore(FieldTy, Address, ValueToStore, IsVolatile, NewIR);
  } else {
    throw NotYetImplementedException("Store value type to static field");
  }
}

IRNode *GenIR::loadStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                               bool IsVolatile, IRNode **NewIR) {
  // Gather information about the field.
  CORINFO_FIELD_HANDLE FieldHandle = FieldToken->hField;
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(FieldToken, CORINFO_ACCESS_GET, &FieldInfo);

  // Handle case where field is a constant zero.
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INTRINSIC_ZERO) {
    return loadConstantI(0, NewIR);
  }

  // Handle case where field is a constant empty string
  if (FieldInfo.fieldAccessor == CORINFO_FIELD_INTRINSIC_EMPTY_STRING) {
    void *StringHandle;
    InfoAccessType Iat = JitContext->JitInfo->emptyStringLiteral(&StringHandle);
    return stringLiteral(mdTokenNil, StringHandle, Iat, NewIR);
  }

  // Gather information about the field type.
  CORINFO_CLASS_HANDLE FieldClassHandle;
  CorInfoType FieldCorType = getFieldType(FieldHandle, &FieldClassHandle);
  Type *FieldTy = getType(FieldCorType, FieldClassHandle);

  // TODO: Replace static read-only fields with constant when possible

  // Get static field address. Convert to pointer.
  Value *Address = rdrGetStaticFieldAddress(FieldToken, &FieldInfo, NewIR);
  Type *PtrToFieldTy = getUnmanagedPointerType(FieldTy);
  if (Address->getType()->isIntegerTy()) {
    Address = LLVMBuilder->CreateIntToPtr(Address, PtrToFieldTy);
  } else {
    ASSERT(Address->getType()->isPointerTy());
    Address = LLVMBuilder->CreatePointerCast(Address, PtrToFieldTy);
  }

  IRNode *FieldValue = (IRNode *)LLVMBuilder->CreateLoad(Address, IsVolatile);
  IRNode *Result = convertToStackType(FieldValue, FieldCorType);
  return Result;
}

IRNode *GenIR::addressOfValue(IRNode *Leaf, IRNode **NewIR) {
  throw NotYetImplementedException("AddressOfValue");
}

IRNode *GenIR::addressOfLeaf(IRNode *Leaf, IRNode **NewIR) {
  throw NotYetImplementedException("AddressOfLeaf");
}

void GenIR::branch(IRNode **NewIR) {
  TerminatorInst *TermInst = LLVMBuilder->GetInsertBlock()->getTerminator();
  ASSERT(TermInst != NULL);
  BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
  ASSERT(BranchInstruction != NULL);
}

IRNode *GenIR::call(ReaderBaseNS::CallOpcode Opcode, mdToken Token,
                    mdToken ConstraintTypeRef, mdToken LoadFtnToken,
                    bool HasReadOnlyPrefix, bool HasTailCallPrefix,
                    bool IsUnmarkedTailCall, uint32_t CurrOffset,
                    bool *RecursiveTailCall, IRNode **NewIR) {
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
  return rdrCall(Data, Opcode, &CallNode, NewIR);
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
IRNode *GenIR::callHelper(CorInfoHelpFunc HelperID, IRNode *Dst, IRNode **NewIR,
                          IRNode *Arg1, IRNode *Arg2, IRNode *Arg3,
                          IRNode *Arg4, ReaderAlignType Alignment,
                          bool IsVolatile, bool NoCtor, bool CanMoveUp) {
  LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
  Type *ReturnType =
      (Dst == NULL) ? Type::getVoidTy(LLVMContext) : Dst->getType();
  return callHelperImpl(HelperID, ReturnType, NewIR, Arg1, Arg2, Arg3, Arg4,
                        Alignment, IsVolatile, NoCtor, CanMoveUp);
}

IRNode *GenIR::callHelperImpl(CorInfoHelpFunc HelperID, Type *ReturnType,
                              IRNode **NewIR, IRNode *Arg1, IRNode *Arg2,
                              IRNode *Arg3, IRNode *Arg4,
                              ReaderAlignType Alignment, bool IsVolatile,
                              bool NoCtor, bool CanMoveUp) {
  ASSERT(HelperID != CORINFO_HELP_UNDEF);

  // TODO: We can turn some of these helper calls into intrinsics.
  // When doing so, make sure the intrinsics are not optimized
  // for the volatile operations.
  IRNode *Address = getHelperCallAddress(HelperID, NewIR);

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
  IRNode *Call = (IRNode *)LLVMBuilder->CreateCall(Target, Arguments);

  if (IsVolatile && isNonVolatileWriteHelperCall(HelperID)) {
    // TODO: this is only needed where CLRConfig::INTERNAL_JitLockWrite is set
    // For now, conservatively we emit barrier regardless.
    memoryBarrier(NewIR);
  }

  return Call;
}

IRNode *GenIR::getHelperCallAddress(CorInfoHelpFunc HelperId, IRNode **NewIR) {
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
                        IsIndirect, IsIndirect, true, false, NewIR);
}

bool GenIR::canMakeDirectCall(ReaderCallTargetData *CallTargetData) {
  return !CallTargetData->isJmp();
}

IRNode *GenIR::makeDirectCallTargetNode(CORINFO_METHOD_HANDLE Method,
                                        void *CodeAddr) {
  uint32_t NumBits = TargetPointerSizeInBits;
  bool IsSigned = false;

  ConstantInt *CodeAddrValue = ConstantInt::get(
      *JitContext->LLVMContext, APInt(NumBits, (uint64_t)CodeAddr, IsSigned));

  FunctionType *FunctionType = getFunctionType(Method);

  return (IRNode *)LLVMBuilder->CreateIntToPtr(
      CodeAddrValue, getUnmanagedPointerType(FunctionType));
}

IRNode *GenIR::genCall(ReaderCallTargetData *CallTargetInfo,
                       CallArgTriple *ArgArray, uint32_t NumArgs,
                       IRNode **CallNode, IRNode **NewIR) {

  IRNode *Call = NULL, *ReturnNode = NULL;
  IRNode *TargetNode = CallTargetInfo->getCallTargetNode();
  CORINFO_SIG_INFO *SigInfo = CallTargetInfo->getSigInfo();

  unsigned HiddenMBParamSize = 0;
  GCLayoutStruct *GCLayout;

  if (CallTargetInfo->isTailCall()) {
    throw NotYetImplementedException("Tail call");
  }

  if (SigInfo->hasTypeArg()) {
    throw NotYetImplementedException("Call HasTypeArg");
  }

  // Ask GenIR to create return value.
  if (!CallTargetInfo->isNewObj()) {
    ReturnNode = makeCallReturnNode(SigInfo, &HiddenMBParamSize, &GCLayout);
  }

  std::vector<Value *> Arguments;

  for (uint32_t I = 0; I < NumArgs; I++) {
    IRNode *ArgNode = ArgArray[I].ArgNode;
    CorInfoType CorType = ArgArray[I].ArgType;
    CORINFO_CLASS_HANDLE Class = ArgArray[I].ArgClass;
    Type *ArgType = this->getType(CorType, Class);

    if (ArgType->isStructTy()) {
      throw NotYetImplementedException("Call has value type args");
    }

    if (I == 0) {
      if (CallTargetInfo->isNewObj()) {
        // Memory and a representative node for the 'this' pointer for newobj
        // has not been created yet. Pass a null value of the right type for now;
        // it will be replaced by the real value in canonNewObjCall.
        ASSERT(ArgNode == NULL);
        ArgNode = (IRNode *)Constant::getNullValue(ArgType);
      } else if (CallTargetInfo->needsNullCheck()) {
        // Insert this Ptr null check if required
        ASSERT(SigInfo->hasThis());
        ArgNode = genNullCheck(ArgNode, NewIR);
      }
    }
    IRNode *Arg = convertFromStackType(ArgNode, CorType, ArgType);
    Arguments.push_back(Arg);
  }

  // We may need to fix the type on the TargetNode.
  const bool FixFunctionType =
      CallTargetInfo->isCallVirt() || CallTargetInfo->isCallI();
  if (FixFunctionType) {
    CORINFO_CLASS_HANDLE ThisClass = nullptr;
    if (SigInfo->hasThis()) {
      ThisClass = ArgArray[0].ArgClass;
    }
    Type *FunctionTy =
        getUnmanagedPointerType(getFunctionType(*SigInfo, ThisClass));
    if (TargetNode->getType()->isPointerTy()) {
      TargetNode =
          (IRNode *)LLVMBuilder->CreatePointerCast(TargetNode, FunctionTy);
    } else {
      ASSERT(TargetNode->getType()->isIntegerTy());
      TargetNode =
          (IRNode *)LLVMBuilder->CreateIntToPtr(TargetNode, FunctionTy);
    }
  } else {
    // We should have a usable type already.
    ASSERT(TargetNode->getType()->isPointerTy());
    ASSERT(TargetNode->getType()->getPointerElementType()->isFunctionTy());
  }

  CallInst *CallInst = LLVMBuilder->CreateCall(TargetNode, Arguments);
  CorInfoIntrinsics IntrinsicID = CallTargetInfo->getCorInstrinsic();

  if ((0 <= IntrinsicID) && (IntrinsicID < CORINFO_INTRINSIC_Count)) {
    throw NotYetImplementedException("Call intrinsic");
  }

  // TODO: deal with PInvokes and var args.

  Call = (IRNode *)CallInst;

  *CallNode = Call;

  bool Done = false;
  // Process newobj. This may involve changing the call target.
  if (CallTargetInfo->isNewObj()) {
    Done = canonNewObjCall(Call, CallTargetInfo, &ReturnNode, NewIR);
  }

  if (!Done) {
    // Add VarArgs cookie to outgoing param list
    if (callIsCorVarArgs(Call)) {
      canonVarargsCall(Call, CallTargetInfo, NewIR);
    }
  }

  if (ReturnNode != NULL) {
    return ReturnNode;
  }
  if (SigInfo->retType != CORINFO_TYPE_VOID) {
    IRNode *Result = convertToStackType((IRNode *)Call, SigInfo->retType);
    return Result;
  } else {
    return NULL;
  }
}

// Canonicalizes a newobj call.
// Returns true if the call is done being processed.
// Outparam is the value to be pushed on the stack (this pointer of new object).
bool GenIR::canonNewObjCall(IRNode *CallNode,
                            ReaderCallTargetData *CallTargetData,
                            IRNode **OutResult, IRNode **NewIR) {
  uint32_t ClassAttribs = CallTargetData->getClassAttribs();
  CORINFO_CLASS_HANDLE ClassHandle = CallTargetData->getClassHandle();

  CorInfoType CorInfoType;
  uint32_t MbSize;

  ReaderBase::getClassType(ClassHandle, ClassAttribs, &CorInfoType, &MbSize);

  bool DoneBeingProcessed = false;
  bool IsArray = ((ClassAttribs & CORINFO_FLG_ARRAY) != 0);
  bool IsVarObjSize = ((ClassAttribs & CORINFO_FLG_VAROBJSIZE) != 0);
  bool IsValueClass = ((ClassAttribs & CORINFO_FLG_VALUECLASS) != 0);

  CallInst *CallInstruction = dyn_cast<CallInst>(CallNode);
  BasicBlock *CurrentBlock = CallInstruction->getParent();
  BasicBlock::iterator SavedInsertPoint = LLVMBuilder->GetInsertPoint();
  LLVMBuilder->SetInsertPoint(CallInstruction);

  if (IsArray) {
    // Zero-based, one-dimensional arrays are allocated via newarr;
    // all other arrays are allocated via newobj
    canonNewArrayCall(CallNode, CallTargetData, OutResult, NewIR);
    LLVMBuilder->SetInsertPoint(CurrentBlock, SavedInsertPoint);
    DoneBeingProcessed = true;
  } else if (IsVarObjSize) {
    // We are allocating an object whose size depends on constructor args
    // (e.g., string). In this case the call to the constructor will allocate
    // the object.

    // Leave the 'this' argument to the constructor call as null.
    ASSERTNR(CallInstruction->getArgOperand(0)->getValueID() ==
             Value::ConstantPointerNullVal);

    // Change the type of the called function and
    // the type of the CallInstruction.
    CallInst *CallInstruction = dyn_cast<CallInst>(CallNode);
    Value *CalledValue = CallInstruction->getCalledValue();
    PointerType *CalledValueType =
        dyn_cast<PointerType>(CalledValue->getType());
    FunctionType *FuncType =
        dyn_cast<FunctionType>(CalledValueType->getElementType());

    // Construct the new function type.
    std::vector<Type *> Arguments;

    for (unsigned I = 0; I < FuncType->getNumParams(); ++I) {
      Arguments.push_back(FuncType->getParamType(I));
    }

    FunctionType *NewFunctionType = FunctionType::get(
        FuncType->getParamType(0), Arguments, FuncType->isVarArg());

    // Create a call target with the right type.
    Value *NewCalledValue = LLVMBuilder->CreatePointerCast(
        CalledValue, getUnmanagedPointerType(NewFunctionType));
    CallInstruction->setCalledFunction(NewCalledValue);

    // Change the type of the call instruction.
    CallInstruction->mutateType(FuncType->getParamType(0));

    LLVMBuilder->SetInsertPoint(CurrentBlock, SavedInsertPoint);
    *OutResult = (IRNode *)CallInstruction;
  } else if (IsValueClass) {
    // We are allocating an instance of a value class on the stack.
    Type *StructType = this->getType(CorInfoType, ClassHandle);
    Instruction *AllocaInst = createTemporary(StructType);

    // Initialize the struct to zero.
    LLVMContext &LLVMContext = *this->JitContext->LLVMContext;
    Value *ZeroByte = Constant::getNullValue(Type::getInt8Ty(LLVMContext));
    uint32_t Align = 0;
    LLVMBuilder->CreateMemSet(AllocaInst, ZeroByte, MbSize, Align);

    // Create a managed pointer to the struct instance and pass it as the 'this'
    // argument to the constructor call.
    Type *ManagedPointerType = getManagedPointerType(StructType);
    Value *ManagedPointerToStruct =
        LLVMBuilder->CreateAddrSpaceCast(AllocaInst, ManagedPointerType);
    Value *CalledValue = CallInstruction->getCalledValue();
    PointerType *CalledValueType =
        dyn_cast<PointerType>(CalledValue->getType());
    FunctionType *FuncType =
        dyn_cast<FunctionType>(CalledValueType->getElementType());
    Type *ThisType = FuncType->getFunctionParamType(0);
    ManagedPointerToStruct =
        LLVMBuilder->CreatePointerCast(ManagedPointerToStruct, ThisType);
    CallInstruction->setArgOperand(0, ManagedPointerToStruct);
    LLVMBuilder->SetInsertPoint(CurrentBlock, SavedInsertPoint);
    *OutResult = (IRNode *)LLVMBuilder->CreateLoad(AllocaInst);
  } else {
    // We are allocating a fixed-size class on the heap.
    // Create a call to the newobj helper specific to this class,
    // and use its return value as the
    // 'this' pointer to be passed as the first argument to the constructor.

    // Create the address operand for the newobj helper.
    CorInfoHelpFunc HelperId = getNewHelper(CallTargetData->getResolvedToken());
    Value *Dest = CallInstruction->getArgOperand(0);
    Value *ThisPointer = callHelper(HelperId, (IRNode *)Dest, NewIR,
                                    CallTargetData->getClassHandleNode(NewIR));
    CallInstruction->setArgOperand(0, ThisPointer);
    LLVMBuilder->SetInsertPoint(CurrentBlock, SavedInsertPoint);
    *OutResult = (IRNode *)ThisPointer;
  }

  return DoneBeingProcessed;
}

void GenIR::canonNewArrayCall(IRNode *Call,
                              ReaderCallTargetData *CallTargetData,
                              IRNode **OutResult, IRNode **NewIR) {
  CallInst *CallInstruction = dyn_cast<CallInst>(Call);
  Value *CalledValue = CallInstruction->getCalledValue();
  PointerType *CalledValueType = dyn_cast<PointerType>(CalledValue->getType());
  FunctionType *FuncType =
      dyn_cast<FunctionType>(CalledValueType->getElementType());

  // To construct the array we need to call a helper passing it the class handle
  // for the constructor method, the number of arguments to the constructor and
  // the arguments to the constructor.

  // Construct the new function type.
  std::vector<Type *> NewTypeArguments;
  std::vector<Value *> NewArguments;

  // The first argument is the class handle.
  IRNode *ClassHandle = CallTargetData->getClassHandleNode(NewIR);
  ASSERTNR(ClassHandle);

  NewTypeArguments.push_back(ClassHandle->getType());
  NewArguments.push_back(ClassHandle);

  // The second argument is the number of arguments to follow.
  uint32_t NumBits = 32;
  bool IsSigned = true;
  Value *NumArgs = ConstantInt::get(
      *JitContext->LLVMContext,
      APInt(NumBits, CallTargetData->getSigInfo()->numArgs, IsSigned));
  ASSERTNR(NumArgs);

  NewTypeArguments.push_back(NumArgs->getType());
  NewArguments.push_back(NumArgs);

  // The rest of the arguments are the same as in the original newobj call.
  // It's a vararg call so add arguments but not type arguments.
  for (unsigned I = 1; I < FuncType->getNumParams(); ++I) {
    NewArguments.push_back(CallInstruction->getArgOperand(I));
  }
  bool IsVarArg = true;
  FunctionType *NewFunctionType =
      FunctionType::get(FuncType->getParamType(0), NewTypeArguments, IsVarArg);

  // Create a call target with the right type.
  // Get the address of the Helper descr.
  IRNode *Target = getHelperCallAddress(CORINFO_HELP_NEW_MDARR, NewIR);
  Value *NewCalledValue = LLVMBuilder->CreateIntToPtr(
      Target, getUnmanagedPointerType(NewFunctionType));

  // Replace the old call instruction with the new one.
  CallInst *NewCallInstruction =
      LLVMBuilder->CreateCall(NewCalledValue, NewArguments);
  CallInstruction->eraseFromParent();

  *OutResult = (IRNode *)NewCallInstruction;

  return;
}

bool GenIR::callIsCorVarArgs(IRNode *CallNode) {
  CallInst *CallInstruction = dyn_cast<CallInst>(CallNode);
  Value *CalledValue = CallInstruction->getCalledValue();
  PointerType *CalledValueType = dyn_cast<PointerType>(CalledValue->getType());
  return dyn_cast<FunctionType>(CalledValueType->getElementType())->isVarArg();
}

IRNode *GenIR::conv(ReaderBaseNS::ConvOpcode Opcode, IRNode *Arg1,
                    IRNode **NewIR) {

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

  if (Info.CheckForOverflow) {
    throw NotYetImplementedException("Convert Overflow");
  }

  Type *SourceTy = Arg1->getType();
  Type *TargetTy = getType(Info.CorType, NULL);
  const bool SourceIsSigned = !Info.SourceIsUnsigned;
  const bool DestIsSigned = TargetTy->isIntegerTy() && isSigned(Info.CorType);
  Value *Conversion = nullptr;

  if (SourceTy == TargetTy) {
    Conversion = Arg1;
  } else if (SourceTy->isIntegerTy() && TargetTy->isIntegerTy()) {
    Conversion = LLVMBuilder->CreateIntCast(Arg1, TargetTy, DestIsSigned);
  } else if (SourceTy->isPointerTy() && TargetTy->isIntegerTy()) {
    Conversion = LLVMBuilder->CreatePtrToInt(Arg1, TargetTy);
  } else if (SourceTy->isIntegerTy() && TargetTy->isFloatingPointTy()) {
    Conversion = SourceIsSigned ? LLVMBuilder->CreateSIToFP(Arg1, TargetTy)
                                : LLVMBuilder->CreateUIToFP(Arg1, TargetTy);
  } else if (SourceTy->isFloatingPointTy() && TargetTy->isIntegerTy()) {
    Conversion = DestIsSigned ? LLVMBuilder->CreateFPToSI(Arg1, TargetTy)
                              : LLVMBuilder->CreateFPToUI(Arg1, TargetTy);
  } else if (SourceTy->isFloatingPointTy() && TargetTy->isFloatingPointTy()) {
    Conversion = LLVMBuilder->CreateFPCast(Arg1, TargetTy);
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

IRNode *GenIR::makeCallReturnNode(CORINFO_SIG_INFO *Sig,
                                  unsigned *HiddenMBParamSize,
                                  GCLayoutStruct **GcLayout) {
  if ((Sig->retType == CORINFO_TYPE_REFANY) ||
      (Sig->retType == CORINFO_TYPE_VALUECLASS)) {
    throw NotYetImplementedException("Return refany or value class");
  }

  return NULL;
}

// Common to both recursive and non-recursive tail call considerations.
// The debug messages are only wanted when checking the general case
// and not for special recursive checks.
bool GenIR::commonTailCallChecks(CORINFO_METHOD_HANDLE DeclaredMethod,
                                 CORINFO_METHOD_HANDLE ExactMethod,
                                 bool IsUnmarkedTailCall, bool SuppressMsgs) {
  // Note that localloc works with tail call provided that we don't perform
  // recursive tail call optimization, so there is a special check for
  // that condition in FgOptRecurse but not here.
  const char *Reason = NULL;
  bool SuppressReport = false;
  uint32_t MethodCompFlags = getCurrentMethodAttribs();
  if (MethodCompFlags & CORINFO_FLG_SYNCH) {
    Reason = "synchronized";
  }
#if 0
  else if (MethodCompFlags & CORINFO_FLG_SECURITYCHECK) {
    Reason = "caller's declarative security";
  }
  else if (IsUnmarkedTailCall && !Optimizing) {
    Reason = "not optimizing";
  }
#endif
  else if (IsUnmarkedTailCall && HasLocAlloc) {
    Reason = "localloc";
  }
#if 0
  else if (FSecurityChecksNeeded) {
    Reason = "GS";
  }
  else if (IsUnmarkedTailCall && hasLocalAddressTaken()) {
    Reason = "local address taken";
  }
#endif
  else if (!canTailCall(DeclaredMethod, ExactMethod, !IsUnmarkedTailCall)) {
    Reason = "canTailCall API\n";
    SuppressReport = true;
  } else {
    return true;
  }

  ASSERTNR(Reason != NULL);
  if (!SuppressMsgs) {
    fprintf(stderr, "ALL: %splicit tail call request not honored due to %s\n",
            IsUnmarkedTailCall ? "im" : "ex", Reason);
  }
  return false;
}

// return true iff recursive and should turn into loop
// While building the flow graph, hasLocAlloc will always be false.
// If the flowgraph builder was wrong in that guess, it takes responsibility
// for fixing the bad labels before inserting arcs.  When this routine is
// invoked later to fill in the blocks, it will get a useful hasLocAlloc
// value and will know that we are treating the tail call or jmp as the
// non-recursive case (in other words not optimizing it into a loop).
// Note that we currently won't perform the recursive tail call optimization
// on an inlined function because we use the parameter list from the entry
// tuple in order to map outgoing parameters back into incoming parameters for
// the loop, and inlinees do not have a standard entry tuple with a parameter
// list.
// NOTE: Do not do the recursive tail if the runtime says we can't inline this
//  method into itself. The reasoning behind this is that it allows a profiler
//  to disable this optimization since it results in the loss of some profiling
//  information.
bool GenIR::fgOptRecurse(ReaderCallTargetData *Data) {
  if (Data->isCallI() || Data->isNewObj() || !Data->isTailCall())
    return false;

  ASSERTNR(Data->getMethodHandle());
  CORINFO_METHOD_HANDLE Method = Data->getKnownMethodHandle();

  // Do recursive tail call only on non-virtual method calls
  if (Method == NULL) {
    return false;
  }

  uint32_t MethodCompFlags = getCurrentMethodAttribs();
  if ((Method != getCurrentMethodHandle())
      // Not yet implemented (but can do a regular tail call)
      ||
      (MethodCompFlags & CORINFO_FLG_SHAREDINST)
#if 0
    || (SS_ATTRIB(CI_Entry(ciPtr)) & AA_VARARGS)
#endif
      ||
      !Data->recordCommonTailCallChecks(commonTailCallChecks(
          Method, Method, Data->isUnmarkedTailCall(), false))
      // treat as inlining since we're removing the call
      ||
      (canInline(Method, Method, NULL) != INLINE_PASS)) {
    // we might want a DBFLAG msg here, but since this routine may be
    // called multiple times for a given call it would just add clutter.
    return false;
  }

  return true;
}

// return true iff recursive and should turn into loop
// *** only valid for JMP calls ***
// NOTE: Do not do the recursive tail if the runtime says we can't inline this
//  method into itself. The reasoning behind this is that it allows a profiler
//  to disable this optimization since it results in the loss of some profiling
//  information.
bool GenIR::fgOptRecurse(mdToken Token) {
  struct Param : RuntimeFilterParams {
    CORINFO_METHOD_HANDLE Method;
    mdToken Token;
  } Params;

  Params.Method = NULL;
  Params.Token = Token;
  Params.This = this;

  PAL_TRY(Param *, PParam, &Params) {
    CORINFO_RESOLVED_TOKEN ResolvedToken;
    PParam->This->resolveToken(PParam->Token, CORINFO_TOKENKIND_Method,
                               &ResolvedToken);
    PParam->Method = ResolvedToken.hMethod;
  }
  PAL_EXCEPT_FILTER(runtimeFilter) {
    runtimeHandleException(&(Params.ExceptionPointers));
    Params.Method = NULL;
  }
  PAL_ENDTRY

  if (!Params.Method) {
    return false;
  }

  uint32_t MethodCompFlags = getCurrentMethodAttribs();
  if ((Params.Method != getCurrentMethodHandle())
      // Not yet implemented (but can do a regular tail call)
      ||
      (MethodCompFlags & CORINFO_FLG_SHAREDINST)
#if 0
    || (SS_ATTRIB(CI_Entry(ciPtr)) & AA_VARARGS)
#endif
      || (!commonTailCallChecks(Params.Method, Params.Method, false, true))
      // treat as inlining since we're removing the call
      ||
      (canInline(Params.Method, Params.Method, NULL) != INLINE_PASS)) {
    // we might want a DBFLAG msg here, but since this routine may be
    // called multiple times for a given call it would just add clutter.
    return false;
  }

  return true;
}

void GenIR::returnOpcode(IRNode *Opr, bool IsSynchronousMethod,
                         IRNode **NewIR) {
  Type *ReturnTy = Function->getReturnType();
  if (Opr == NULL) {
    ASSERT(ReturnTy->isVoidTy());
    LLVMBuilder->CreateRetVoid();
  } else {
    Value *ReturnValue = convertFromStackType(Opr, ReturnCorType, ReturnTy);
    LLVMBuilder->CreateRet(ReturnValue);
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

void GenIR::nop(IRNode **NewIR) {
  // Preserve Nops in debug builds since they may carry unique source positions.
  if ((JitContext->Flags & CORJIT_FLG_DEBUG_CODE) != 0) {
    // LLVM has no high-level NOP instruction. Put in a placeholder for now.
    // We may need to pick something else that survives lowering.
    Value *DoNothing = Intrinsic::getDeclaration(JitContext->CurrentModule,
                                                 Intrinsic::donothing);
    LLVMBuilder->CreateCall(DoNothing);
  }
}

void GenIR::pop(IRNode *Opr, IRNode **NewIR) {
  // No actions needed.
}

void GenIR::dup(IRNode *Opr, IRNode **Result1, IRNode **Result2,
                IRNode **NewIR) {
  *Result1 = Opr;
  *Result2 = Opr;
}

bool GenIR::memoryBarrier(IRNode **NewIR) {
  // TODO: Here we emit mfence which is stronger than sfence
  // that CLR needs.
  // We could improve this further by using
  // lock or byte ptr [rsp], 0
  // which is faster than sfence.
  LLVMBuilder->CreateFence(SequentiallyConsistent);
  return true;
}

void GenIR::switchOpcode(IRNode *Opr, IRNode **NewIR) {
   // We split the block right after the switch during the flow-graph build.
   // The terminator is switch instruction itself.
   // Now condition operand is updated.
   BasicBlock *CurrBlock = LLVMBuilder->GetInsertBlock();
   TerminatorInst *TermInst = CurrBlock->getTerminator();
   SwitchInst *SwitchInstruction = cast<SwitchInst>(TermInst);

   SwitchInstruction->setCondition(Opr);
}

void GenIR::throwOpcode(IRNode *Arg1, IRNode **NewIR) {
  // Using a call for now; this will need to be invoke
  // when we get EH flow properly modeled.
  CallInst *ThrowCall =
      (CallInst *)callHelper(CORINFO_HELP_THROW, NULL, NewIR, Arg1);

  // Annotate the helper
  ThrowCall->setDoesNotReturn();
}

IRNode *GenIR::genNullCheck(IRNode *Node, IRNode **NewIR) {
  BasicBlock *CheckBlock = LLVMBuilder->GetInsertBlock();
  BasicBlock::iterator InsertPoint = LLVMBuilder->GetInsertPoint();
  Instruction *NextInstruction =
      (InsertPoint == CheckBlock->end() ? nullptr : (Instruction *)InsertPoint);

  // Create the throw block so we can reference it later.
  // Note: we could generate much smaller IR by reusing the same throw block for
  // all null checks, but at the cost of not being able to tell in a debugger
  // what was null.  The current strategy is to favor debuggability.
  // TODO: Find a way to annotate the throw blocks as cold so they get laid out
  // out-of-line.
  BasicBlock *ThrowBlock =
      BasicBlock::Create(*JitContext->LLVMContext, "ThrowNullRef", Function);

  // Split the block.  This creates a goto connecting the blocks that we'll
  // replace with the conditional branch.
  // Note that we split at offset NextInstrOffset rather than CurrInstrOffset.
  // We're already generating the IR for the instr at CurrInstrOffset, and using
  // NextInstrOffset here ensures that we won't redundantly try to add this
  // instruction again when processing moves to the new NonNullBlock.
  BasicBlock *NonNullBlock = ReaderBase::fgSplitBlock(
      (FlowGraphNode *)CheckBlock, NextInstrOffset, (IRNode *)NextInstruction);
  TerminatorInst *Goto = CheckBlock->getTerminator();

  // Insert the compare against null.
  LLVMBuilder->SetInsertPoint(Goto);
  Value *Compare = LLVMBuilder->CreateIsNotNull(Node, "NullCheck");

  // Swap the conditional branch in place of the goto.
  BranchInst *Branch =
      LLVMBuilder->CreateCondBr(Compare, NonNullBlock, ThrowBlock);
  Goto->eraseFromParent();

  // FIll in the throw block.
  LLVMBuilder->SetInsertPoint(ThrowBlock);
  // TODO: Use throw_null_ref helper once that's available from CoreCLR.  For
  // now, use throw_div_zero since it has the right signature and we don't
  // expect exceptions to work dynamically anyway.
  CallInst *ThrowCall =
      (CallInst *)callHelper(CORINFO_HELP_THROWDIVZERO, nullptr, NewIR);
  ThrowCall->setDoesNotReturn();
  LLVMBuilder->CreateUnreachable();

  // Give the throw block equal start and end offsets so subsequent processing
  // won't try to translate MSIL into it.
  fgNodeSetStartMSILOffset((FlowGraphNode *)ThrowBlock, CurrInstrOffset);
  fgNodeSetEndMSILOffset((FlowGraphNode *)ThrowBlock, CurrInstrOffset);

  // Null out the throw block operand stack.
  fgNodeSetOperandStack((FlowGraphNode *)ThrowBlock, nullptr);

  // Move the insert point back to the first instruction in the non-null path.
  if (NextInstruction == nullptr) {
    LLVMBuilder->SetInsertPoint(NonNullBlock);
  } else {
    LLVMBuilder->SetInsertPoint(NonNullBlock->getFirstInsertionPt());
  }

  return Node;
};

void GenIR::leave(uint32_t TargetOffset, bool IsNonLocal,
                  bool EndsWithNonLocalGoto, IRNode **NewIR) {
  // TODO: handle exiting through nested finallies
  // currently FG-building phase 1 generates an appropriate
  // branch instruction for trivial leaves and rejects others
  return;
}

IRNode *GenIR::loadStr(mdToken Token, IRNode **NewIR) {
  // TODO: Special handling for cold blocks
  void *StringHandle;
  InfoAccessType Iat = constructStringLiteral(Token, &StringHandle);
  ASSERTNR(StringHandle != NULL);

  return stringLiteral(Token, StringHandle, Iat, NewIR);
}

IRNode *GenIR::stringLiteral(mdToken Token, void *StringHandle,
                             InfoAccessType Iat, IRNode **NewIR) {
  IRNode *StringPtrNode = NULL;
  switch (Iat) {
#if defined(FEATURE_BASICFREEZE)
  case IAT_VALUE:
    StringPtrNode = handleToIRNode(Token, StringHandle, 0, false, false, true,
                                   false, NewIR, true);
    break;
#endif
  case IAT_PVALUE:
  case IAT_PPVALUE: {
    // Get the raw address of the pointer to reference to string.
    IRNode *RawAddress = handleToIRNode(
        Token, StringHandle, 0, (Iat == IAT_PPVALUE), true, true, false, NewIR);
    // Cast it to the right address type.
    CORINFO_CLASS_HANDLE StringClassHandle =
        getBuiltinClass(CorInfoClassId::CLASSID_STRING);
    Type *StringRefTy = getType(CORINFO_TYPE_CLASS, StringClassHandle);
    Type *AddressTy = getUnmanagedPointerType(StringRefTy);
    IRNode *TypedAddress =
        (IRNode *)LLVMBuilder->CreateIntToPtr(RawAddress, AddressTy);
    // Fetch the string reference.
    StringPtrNode = loadIndir(ReaderBaseNS::LdindRef, TypedAddress,
                              Reader_AlignNatural, false, false, NewIR);
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
                              IRNode **NewIR,
                              bool IsFrozenObject /* default = false */
                              ) {
  if (IsIndirect || IsCallTarget || IsFrozenObject) {
    throw NotYetImplementedException("NYI handle cases");
  }

  // TODO: There is more work for ngen scenario here. We are ignoring
  // fRelocatable and realHandle for now.

  uint32_t NumBits = TargetPointerSizeInBits;
  bool IsSigned = false;

  ConstantInt *HandleValue = ConstantInt::get(
      *JitContext->LLVMContext, APInt(NumBits, (uint64_t)EmbHandle, IsSigned));

  return (IRNode *)HandleValue;
}

// TODO: currently PtrType telling base or interior pointer is ignored.
// So for now, deliberately we keep this API to retain the call site.
IRNode *GenIR::makePtrNode(ReaderPtrType PtrType) { return loadNull(nullptr); }

// Load a pointer-sized value from the indicated address.
// Used when navigating through runtime data structures.
// This should not be used for accessing user data types.
IRNode *GenIR::derefAddress(IRNode *Address, bool DstIsGCPtr, bool IsConst,
                            IRNode **NewIR) {

  // TODO: If IsConst is false, the load could cause a null pointer exception,
  // so we may need an explicit null check. Not sure if there's a covering
  // upstream check or not, so be cautious now.
  if (!IsConst) {
    throw NotYetImplementedException("non-const derefAddress");
  }

  // We don't know the true referent type so just use a pointer sized
  // integer for the result.
  Type *ReferentTy =
      Type::getIntNTy(*JitContext->LLVMContext, TargetPointerSizeInBits);

  // Address is a pointer, but since it may come from dereferencing into
  // runtime data structures with unknown field types, we may need a cast here
  // to make it so.
  Type *AddressTy = Address->getType();
  if (!AddressTy->isPointerTy()) {
    // Ensure we have a suitable integer value
    ASSERT(AddressTy->isIntegerTy());
    ASSERT(AddressTy->getScalarSizeInBits() == TargetPointerSizeInBits);
    // We should not be trying to cast an integer value to a GC pointer.
    ASSERT(!DstIsGCPtr);
    // Cast to get a pointer-sized referent type.
    Type *CastTy = getUnmanagedPointerType(ReferentTy);
    Address = (IRNode *)LLVMBuilder->CreateIntToPtr(Address, CastTy);
  } else if (DstIsGCPtr) {
    // Verify we have a managed pointer.
    ASSERT(isManagedPointerType(cast<PointerType>(AddressTy)));
    // Cast to get a pointer-sized referent type.
    Type *CastTy = getManagedPointerType(ReferentTy);
    Address = (IRNode *)LLVMBuilder->CreatePointerCast(Address, CastTy);
  } else {
    // Verify we have an unmanaged pointer.
    ASSERT(!isManagedPointerType(cast<PointerType>(AddressTy)));
    // Cast to get a pointer-sized referent type.
    Type *CastTy = getUnmanagedPointerType(ReferentTy);
    Address = (IRNode *)LLVMBuilder->CreatePointerCast(Address, CastTy);
  }

  Value *Result = LLVMBuilder->CreateLoad(Address);
  return (IRNode *)Result;
}

IRNode *GenIR::loadVirtFunc(IRNode *Arg1, CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            CORINFO_CALL_INFO *CallInfo, IRNode **NewIR) {
  IRNode *TypeToken = genericTokenToNode(ResolvedToken, NewIR, true);
  IRNode *MethodToken = genericTokenToNode(ResolvedToken, NewIR);

  Type *Ty = Type::getIntNTy(*this->JitContext->LLVMContext,
                             TargetPointerSizeInBits);
  IRNode *CodeAddress = callHelperImpl(CORINFO_HELP_VIRTUAL_FUNC_PTR, Ty,
                                       NewIR, Arg1, TypeToken, MethodToken);

  FunctionType *FunctionType = getFunctionType(CallInfo->hMethod);
  return (IRNode *)LLVMBuilder->CreateIntToPtr(CodeAddress,
      getUnmanagedPointerType(FunctionType));
}

IRNode *GenIR::loadPrimitiveType(IRNode *Addr, CorInfoType CorInfoType,
                                 ReaderAlignType Alignment, bool IsVolatile,
                                 bool IsInterfReadOnly, IRNode **NewIR) {
  ASSERTNR(isPrimitiveType(CorInfoType) || CorInfoType == CORINFO_TYPE_REFANY);

  // Get type of the result.
  Type *AddressTy = Addr->getType();
  ASSERT(AddressTy->isPointerTy());
  PointerType *Ty = cast<PointerType>(AddressTy);
  Type *ReferentTy = Ty->getPointerElementType();
  IRNode *TypedAddr = Addr;

  // For the 'REFANY' case, verify the address carries
  // reasonable typing. Address producer must ensure this.
  if (CorInfoType == CORINFO_TYPE_REFANY) {
    // The result of the load is an object reference,
    // So addr should be ptr to managed ptr to struct
    if (!ReferentTy->isPointerTy()) {
      // If we hit this we should fix the address producer, not
      // coerce the type here.
      throw NotYetImplementedException("unexpected type in load primitive");
    }
    ASSERT(ReferentTy->isPointerTy());
    PointerType *ReferentPtrTy = cast<PointerType>(ReferentTy);
    ASSERT(isManagedPointerType(ReferentPtrTy));
    ASSERT(ReferentTy->getPointerElementType()->isStructTy());
    // GC pointers are always naturally aligned
    Alignment = Reader_AlignNatural;
  } else {
    // For the true primitve case we may need to cast the address.
    Type *ExpectedTy = this->getType(CorInfoType, NULL);
    if (ReferentTy != ExpectedTy) {
      Type *PtrToExpectedTy = getUnmanagedPointerType(ExpectedTy);
      TypedAddr =
          (IRNode *)LLVMBuilder->CreatePointerCast(Addr, PtrToExpectedTy);
    }
  }

  uint32_t Align = (Alignment == Reader_AlignNatural)
                       ? TargetPointerSizeInBits / 8
                       : Alignment;
  LoadInst *LoadInst = LLVMBuilder->CreateLoad(TypedAddr, IsVolatile);
  LoadInst->setAlignment(Align);

  IRNode *Result = convertToStackType((IRNode *)LoadInst, CorInfoType);

  return Result;
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

IRNode *GenIR::cmp(ReaderBaseNS::CmpOpcode Opcode, IRNode *Arg1, IRNode *Arg2,
                   IRNode **NewIR) {

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
    // int32 can be compared with nativeint
    ASSERT(!IsPointer1 && !IsPointer2 && !IsFloat1);
    bool IsSigned = true;
    if (Size1 == 32) {
      Arg1 = (IRNode *)LLVMBuilder->CreateIntCast(Arg1, Ty2, IsSigned);
    } else {
      Arg2 = (IRNode *)LLVMBuilder->CreateIntCast(Arg2, Ty1, IsSigned);
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

  if (IsFloat1) {
    Cmp = LLVMBuilder->CreateFCmp(FloatCmpMap[Opcode], Arg1, Arg2);
  } else {
    Cmp = LLVMBuilder->CreateICmp(IntCmpMap[Opcode], Arg1, Arg2);
  }

  IRNode *Result = convertToStackType((IRNode *)Cmp, CORINFO_TYPE_INT);

  return Result;
}

void GenIR::boolBranch(ReaderBaseNS::BoolBranchOpcode Opcode, IRNode *Arg1,
                       IRNode **NewIR) {
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
  ASSERT(TermInst != NULL);
  BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
  ASSERT(BranchInstruction != NULL);
  BranchInstruction->setCondition(Condition);
}

void GenIR::condBranch(ReaderBaseNS::CondBranchOpcode Opcode, IRNode *Arg1,
                       IRNode *Arg2, IRNode **NewIR) {

  // TODO: make this bit of code (which also appears in Cmp)
  // into a helper routine.

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

  ASSERT(Size1 == Size2);
  ASSERT((Size1 == 32) || (Size1 == 64));
  ASSERT(IsFloat1 == IsFloat2);

  // Make types agree without perturbing bit patterns.
  if (Ty1 != Ty2) {
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
      IsFloat1 ? LLVMBuilder->CreateFCmp(FloatBranchMap[Opcode], Arg1, Arg2)
               : LLVMBuilder->CreateICmp(IntBranchMap[Opcode], Arg1, Arg2);

  // Patch up the branch instruction
  TerminatorInst *TermInst = LLVMBuilder->GetInsertBlock()->getTerminator();
  ASSERT(TermInst != NULL);
  BranchInst *BranchInstruction = dyn_cast<BranchInst>(TermInst);
  ASSERT(BranchInstruction != NULL);
  BranchInstruction->setCondition(Condition);
}

IRNode *GenIR::getStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                     IRNode **NewIR) {
  CORINFO_FIELD_INFO FieldInfo;
  getFieldInfo(ResolvedToken, CORINFO_ACCESS_ADDRESS, &FieldInfo);
  return rdrGetStaticFieldAddress(ResolvedToken, &FieldInfo, NewIR);
}

IRNode *GenIR::shift(ReaderBaseNS::ShiftOpcode Opcode, IRNode *ShiftAmount,
                     IRNode *ShiftOperand, IRNode **NewIR) {
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
IRNode *GenIR::sizeofOpcode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode **NewIR) {
  uint32_t ClassSize = getClassSize(ResolvedToken->hClass);
  uint32_t NumBits = 32;
  bool IsSigned = false;
  IRNode *Value = (IRNode *)ConstantInt::get(
      *JitContext->LLVMContext, APInt(NumBits, ClassSize, IsSigned));
  IRNode *Result = convertToStackType(Value, CorInfoType::CORINFO_TYPE_UINT);

  return Result;
}

IRNode *GenIR::newObj(mdToken Token, mdToken LoadFtnToken, uint32_t CurrOffset,
                      IRNode **NewIR) {
  // Generate the constructor call
  // rdrCall and GenCall process newobj
  //  so there's nothing else to do.
  bool IsRecursive = false;
  bool ReadOnlyPrefix = false;
  bool TailCallPrefix = false;
  bool IsUnmarkedTailCall = false;
  IRNode *Result = call(ReaderBaseNS::NewObj, Token, mdTokenNil, LoadFtnToken,
                        ReadOnlyPrefix, TailCallPrefix, IsUnmarkedTailCall,
                        CurrOffset, &IsRecursive, NewIR);
  ASSERTNR(!IsRecursive); // No tail recursive new-obj calls
  return Result;
}

IRNode *GenIR::newArr(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                      IRNode **NewIR) {
  CORINFO_CLASS_HANDLE ElementType;

  // The second argument to the helper is the number of elements in the array.
  // Create the second argument, the number of elements in the array.
  // This needs to be of type native int.
  Type *NumOfElementsType =
      Type::getIntNTy(*this->JitContext->LLVMContext, TargetPointerSizeInBits);
  IRNode *NumOfElements =
      convertToStackType(Arg1, CorInfoType::CORINFO_TYPE_NATIVEINT);

  // Or token with CORINFO_ANNOT_ARRAY so that we get back an array-type handle
  bool EmbedParent = false;
  bool MustRestoreHandle = true;
  IRNode *Token =
      genericTokenToNode(ResolvedToken, NewIR, EmbedParent, MustRestoreHandle,
                         (CORINFO_GENERIC_HANDLE *)&ElementType, NULL);

  Type *ArrayType =
      getType(CorInfoType::CORINFO_TYPE_CLASS, ResolvedToken->hClass);
  Value *Destination = Constant::getNullValue(ArrayType);

  return callHelper(getNewArrHelper(ElementType), (IRNode *)Destination, NewIR,
                    Token, NumOfElements);
}

// CastOp - Generates code for castclass or isinst.
IRNode *GenIR::castOp(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *ObjRefNode,
                      IRNode **NewIR, CorInfoHelpFunc HelperId) {
  CORINFO_GENERIC_HANDLE HandleType = NULL;

  // Create the type node
  bool EmbedParent = false;
  bool MustRestoreHandle = false;

  IRNode *ClassHandleNode = genericTokenToNode(
      ResolvedToken, NewIR, EmbedParent, MustRestoreHandle, &HandleType, NULL);
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

  Type *ResultType =
      getType(CorInfoType::CORINFO_TYPE_CLASS, ResolvedToken->hClass);
  IRNode *Dst = (IRNode *)Constant::getNullValue(ResultType);

  // Generate the helper call or intrinsic
  bool IsVolatile = false;
  bool DoesNotInvokeStaticCtor = Optimize;
  return callHelper(HelperId, Dst, NewIR, ClassHandleNode, ObjRefNode, NULL,
                    NULL, Reader_AlignUnknown, IsVolatile,
                    DoesNotInvokeStaticCtor);
}

// Override the cast class optimization
bool GenIR::disableCastClassOptimization() {
  // TODO: We may want to enable cast class optimization unless it's disabled
  // by some flags or we are generating debug code or it's causing problems
  // downstream.

  return true;
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
void GenIR::removeStackInterference(IRNode **NewIR) { return; }

void GenIR::removeStackInterferenceForLocalStore(uint32_t Opcode,
                                                 uint32_t Ordinal,
                                                 IRNode **NewIR) {
  return;
}

void GenIR::maintainOperandStack(IRNode **Opr1, IRNode **Opr2,
                                 FlowGraphNode *CurrentBlock, IRNode **NewIR) {

  if (ReaderOperandStack->depth() == 0) {
    return;
  }

  FlowGraphEdgeList *SuccessorList = fgNodeGetSuccessorListActual(CurrentBlock);

  if (SuccessorList == nullptr) {
    clearStack(NewIR);
    return;
  }

  while (SuccessorList != nullptr) {
    FlowGraphNode *SuccessorBlock = fgEdgeListGetSink(SuccessorList);

    FlowGraphEdgeList *SuccessorPredecessorList =
        fgNodeGetPredecessorListActual(SuccessorBlock);

    FlowGraphEdgeList *SuccessorPredecessorListNext =
        fgEdgeListGetNextPredecessorActual(SuccessorPredecessorList);

    if (SuccessorPredecessorListNext == nullptr) {
      // The current node is the only predecessor of this Successor. We need to
      // create a stack for the Successor and copy the items from the current
      // stack.
      if ((fgNodeGetStartMSILOffset(SuccessorBlock) ==
           fgNodeGetEndMSILOffset(SuccessorBlock)) &&
          (fgNodeGetSuccessorListActual(SuccessorBlock) == nullptr)) {
        // There is no MSIL for this block and it has no successors, so no need
        // to propagate the stack.  This is a common case for implicit exception
        // throw blocks.
      } else {
        fgNodeSetOperandStack(SuccessorBlock, ReaderOperandStack->copy());
      }
    } else {
      ReaderStack *SuccessorStack = fgNodeGetOperandStack(SuccessorBlock);
      bool CreatePHIs = false;
      if (SuccessorStack == nullptr) {
        // We need to create a new stack for the Successor and populate it
        // with PHI instructions corresponding to the values on the current
        // stack.
        SuccessorStack =
            createStack(std::min(MethodInfo->maxStack,
                                 std::min(100U, MethodInfo->ILCodeSize)),
                        this);
        fgNodeSetOperandStack(SuccessorBlock, SuccessorStack);
        CreatePHIs = true;
      }

      ReaderStackIterator *Iterator;
      IRNode *Current = ReaderOperandStack->getReverseIterator(&Iterator);
      Instruction *CurrentInst = SuccessorBlock->begin();
      PHINode *Phi = nullptr;
      while (Current != nullptr) {
        Value *CurrentValue = (Value *)Current;
        if (CreatePHIs) {
          // The Successor has at least 2 predecessors so we use 2 as the
          // hint for the number of PHI sources.
          TerminatorInst *TermInst = SuccessorBlock->getTerminator();
          if (TermInst != nullptr) {
            Phi = PHINode::Create(CurrentValue->getType(), 2, "", TermInst);
          } else {
            Phi =
                PHINode::Create(CurrentValue->getType(), 2, "", SuccessorBlock);
          }
        } else {
          // PHI instructions should have been inserted already
          Phi = dyn_cast<PHINode>(CurrentInst);
          ASSERT(Phi != nullptr);
          CurrentInst = CurrentInst->getNextNode();
        }
        if (Phi->getType() != CurrentValue->getType()) {
          throw NotYetImplementedException("Phi type mismatch");
        }
        Phi->addIncoming(CurrentValue, (BasicBlock *)CurrentBlock);
        SuccessorStack->push((IRNode *)Phi, NewIR);
        Current = ReaderOperandStack->reverseIteratorGetNext(&Iterator);
      }

      // The number if PHI instructions should match the number of values on the
      // stack.
      if (!CreatePHIs) {
        Phi = dyn_cast<PHINode>(CurrentInst);
        ASSERT(Phi == nullptr);
      }
    }
    SuccessorList = fgEdgeListGetNextSuccessorActual(SuccessorList);
  }

  clearStack(NewIR);
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
