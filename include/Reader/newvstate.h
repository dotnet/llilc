//===------------------- include/Reader/newvstate.h -------------*- C++ -*-===//
//
// LLVM-MSILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Data structures for holding verification state.
//
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_NEW_VSTATE_H
#define MSIL_READER_NEW_VSTATE_H

#include "vtypeinfo.h"
#include "corerror.h"
#include "gverify.h"

class LocalDescr {
public:
  CorInfoType Type;
  CORINFO_CLASS_HANDLE Class;
};

class VerificationState {
public:
  bool BlockIsBad;
  bool *ArgsInitialized;

  // This field caches the token of the delegate method
  // in a potential delegate creation sequence.
  mdToken DelegateMethodRef;

  UINT ConstrainedPrefix : 1;
  UINT ReadonlyPrefix : 1;
  UINT TailPrefix : 1;
  UINT VolatilePrefix : 1;
  UINT UnalignedPrefix : 1;
  UINT TailInBlock : 1;
  // say we initialize the 'this' pointer in a try.
  // Real successors of the block can assume 'this' is inited.
  // nominal successors cannot.  However if we know 'this' was inited in
  UINT ThisInitializedThisBlock : 1;
  UINT StrongThisInitialized : 1;
  UINT ContainsCtorCall : 1; // block is in a ctor and calls a parent or same
                             // class ctor
  const BYTE *DelegateCreateStart;

  InitState ThisInitialized; // 'this' has been initialized on some paths
private:
  VerType *Vstack;
  unsigned Vsp;
  unsigned MaxStack;

  VerificationState() {}

public:
  ReaderBase *Base;

  inline void setStack(VerType *StackMem);

  inline void init(unsigned MaxStackSize, unsigned NumLocals, bool InitLocals,
                   InitState InitState);

  inline void print();

  inline void push(VerType Typ);

  inline VerType pop();

  inline VerType impStackTop(unsigned N = 0);

  // pop an objref which might be an uninitialized 'this' ptr
  // See Partion 3 1.8.1.4
  //   No operations can be performed on an uninitialized 'this'
  //   except for storing into and loading from the object's fields.
  inline VerType popPossiblyUninit();

  inline unsigned stackLevel() { return Vsp; }

  bool isThisPublishable() {
    if (ThisInitialized == ThisInited || ThisInitialized == ThisEHReached)
      return true;
    else
      return false;
  }

  bool isThisInitialized() { return (ThisInitialized == ThisInited); }

  void setThisInitialized() {
    // if its EHREACHED keep that
    if (ThisInitialized != ThisEHReached)
      ThisInitialized = ThisInited;
  }
};

void VerificationState::setStack(VerType *StackMem) { Vstack = StackMem; }

void VerificationState::init(unsigned MaxStackSize, unsigned NumLocals,
                             bool InitLocals, InitState InitState) {
  Vsp = 0;
  MaxStack = MaxStackSize;
  DelegateMethodRef = mdTokenNil;

  BlockIsBad = false;
  ConstrainedPrefix = false;
  ReadonlyPrefix = false;
  TailPrefix = false;
  VolatilePrefix = false;
  UnalignedPrefix = false;
  TailInBlock = false;
  DelegateCreateStart = NULL;
  ThisInitializedThisBlock = false;

  ThisInitialized = InitState;
  StrongThisInitialized = false;

  for (unsigned I = 0; I < NumLocals; I++) {
    ArgsInitialized[I] = InitLocals;
  }
}

VerType VerificationState::pop() {
  VerType Ret = popPossiblyUninit();
  Base->verifyAndReportFound((!Ret.isObjRef()) ||
                                 (!Ret.isUninitialisedObjRef()),
                             Ret, MVER_E_STACK_UNINIT);
  return Ret;
}

// See Partion 3 1.8.1.4
//   No operations can be performed on an uninitialized 'this'
//   except for storing into and loading from the object's fields.
VerType VerificationState::popPossiblyUninit() {
  Base->gverifyOrReturn(Vsp > 0, MVER_E_STACK_UNDERFLOW);

  Vsp--;
  VerType Result = Vstack[Vsp];

// blank out the thing we just popped
#ifndef NDEBUG
  memset(Vstack + Vsp, 0xcd, sizeof(VerType));
#endif

  return Result;
}

void VerificationState::push(VerType Typ) {
  Base->gverifyOrReturn(Vsp < MaxStack, MVER_E_STACK_OVERFLOW);
  Vstack[Vsp] = Typ;
  Vsp++;
}

VerType VerificationState::impStackTop(unsigned N) {
  Base->gverifyOrReturn(Vsp > N, MVER_E_STACK_UNDERFLOW);

  return Vstack[Vsp - N - 1];
}

// =========================================================================
// ================ Exceptions
// =========================================================================

class CallAuthorizationException : public ReaderException {};

class VerificationException : public ReaderException {
public:
  DWORD DwFlags; // VER_ERR_XXX

  union {
    ReaderBaseNS::OPCODE Opcode;
    unsigned long Padding1; // to match with idl generated struct size
  };

  union {
    DWORD DwOffset; // #of bytes from start of method
    long Offset;    // for backward compat with Metadata validator
  };

  union {
    mdToken Token; // for backward compat with metadata validator
    BYTE CallConv;
    CorElementType Elem;
    DWORD StackSlot;        // positon in the Stack
    unsigned long Padding2; // to match with idl generated struct size
  };

  union {
    DWORD Exception1; // Exception Record #
    DWORD VarNumber;  // Variable #
    DWORD ArgNumber;  // Argument #
    DWORD Operand;    // Operand for the opcode
  };

  union {
    DWORD Exception2; // Exception Record #
  };
};

#endif // MSIL_READER_NEW_VSTATE_H
