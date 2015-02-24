//===------------------- include/Reader/reader.h ----------------*- C++ -*-===//
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
/// \brief Declares the ReaderBase class, which provides a generic framework for
/// translating MSIL bytecode into some other representation.
///
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_H
#define MSIL_READER_H

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <map>

#include "global.h"
#include "Pal/LLILCPal.h"
#if !defined(_MSC_VER)
#include "ntimage.h"
#endif
#include "cor.h"
#include "corjit.h"
#include "readerenum.h"
#include "gverify.h"
#include "switches.h"

// as defined in src\vm\vars.hpp
#define MAX_CLASSNAME_LENGTH 1024

#ifndef COUNTOF
#define COUNTOF(a) (sizeof(a) / sizeof(*a))
#endif

// -----------------------------------------------------------------
// Debugging
// -----------------------------------------------------------------

#define ASSERTM(Predicate, Message)                                            \
  do {                                                                         \
    if (!(Predicate)) {                                                        \
      ReaderBase::debugError(__FILE__, __LINE__, Message);                     \
    }                                                                          \
  } while (0)
#define ASSERT(Predicate) ASSERTM(Predicate, #Predicate)
#define UNREACHED 0
#ifndef _MSC_VER
#define ASSUME(Predicate) __assume(Predicate)
#else
#define ASSUME(Predicate)
#endif

#if !defined(_DEBUG)
#define NODEBUG 1
#define RELEASE 1
#endif

#ifndef NODEBUG
#define ASSERTMNR(Predicate, Message) ASSERTM(Predicate, Message)
#define ASSERTNR(Predicate) ASSERTMNR(Predicate, #Predicate)
#define ASSERTDBG(Predicate) ASSERTM(Predicate, #Predicate)
#define TODO() ASSERTMNR(0, "TODO\n")
#else
#define ASSERTMNR(Predicate, Message) ASSUME(Predicate)
#define ASSERTNR(Predicate) ASSUME(Predicate)
#define ASSERTDBG(Predicate)
#define TODO()
#endif

// ---------------------- HRESULT value definitions -----------------
//
// HRESULT definitions
//
//
//  Values are 32 bit values layed out as follows:
//
//   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1
//   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
//  +---+-+-+-----------------------+-------------------------------+
//  |Sev|C|R|     Facility          |               Code            |
//  +---+-+-+-----------------------+-------------------------------+
//
//  where
//
//      Sev - is the severity code
//
//          00 - Success
//          01 - Informational
//          10 - Warning
//          11 - Error
//
//      C - is the Customer code flag
//
//      R - is a reserved bit
//
//      Facility - is the facility code
//
//      Code - is the facility's status code
//
//
// Internal JIT exceptions.

#define FACILITY_LLILCJIT 0x64 // This is a made up facility code

// Some fatal error occurred
#define LLILCJIT_FATAL_ERROR CORJIT_INTERNALERROR
// An out of memory error occurred in the LLILCJIT
#define LLILCJIT_NOMEM_ERROR CORJIT_OUTOFMEM

#define LLILCJIT_FATALEXCEPTION_CODE (0xE0000000 | FACILITY_LLILCJIT << 16 | 1)
#define LLILCJIT_READEREXCEPTION_CODE (0xE0000000 | FACILITY_LLILCJIT << 16 | 2)

//===========================================================================

// Function: jitFilter
//
//  Filter to detect/handle internal JIT exceptions.
//  Returns EXCEPTION_EXECUTE_HANDLER for LLILCJIT exceptions,
//  and EXCEPTION_CONTINUE_SEARCH for all others.
//
#ifdef __cplusplus
extern "C"
#endif
    int
    jitFilter(PEXCEPTION_POINTERS ExceptionPointersPtr, void *Param);
extern void _cdecl fatal(int Errnum, ...);

// Global environment config variables (set by GetConfigString).
// These are defined/set in jit.cpp.

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t EnvConfigCseOn;
#ifndef NDEBUG
extern uint32_t EnvConfigCseBinarySearch;
extern uint32_t EnvConfigCseMax;
extern uint32_t EnvConfigCopyPropMax;
extern uint32_t EnvConfigDeadCodeMax;
extern uint32_t EnvConfigCseStats;
#endif // !NDEBUG
#if !defined(CC_PEVERIFY)
extern uint32_t EnvConfigTailCallOpt;
#if !defined(NODEBUG)
extern uint32_t EnvConfigDebugVerify;
extern uint32_t EnvConfigTailCallMax;
#endif // !NODEBUG
#endif // !CC_PEVERIFY
extern uint32_t EnvConfigPInvokeInline;
extern uint32_t EnvConfigPInvokeCalliOpt;
extern uint32_t EnvConfigNewGCCalc;
extern uint32_t EnvConfigTurnOffDebugInfo;
extern char16_t *EnvConfigJitName;

extern bool HaveEnvConfigCseOn;
extern bool HaveEnvConfigCseStats;
#ifndef NDEBUG
extern bool HaveEnvConfigCseBinarySearch;
extern bool HaveEnvConfigCseMax;
extern bool HaveEnvConfigCopyPropMax;
extern bool HaveEnvConfigDeadCodeMax;
#endif // !NDEBUG
#if !defined(CC_PEVERIFY)
extern bool HaveEnvConfigTailCallOpt;
#if !defined(NODEBUG)
extern bool HaveEnvConfigDebugVerify;
extern bool HaveEnvConfigTailCallMax;
#endif // !NODEBUG
#endif // !CC_PEVERIFY
extern bool HaveEnvConfigPInvokeInline;
extern bool HaveEnvConfigPInvokeCalliOpt;
extern bool HaveEnvConfigNewGCCalc;
extern bool HaveEnvConfigTurnOffDebugInfo;
extern bool HaveEnvConfigJitName;

} // extern "C"

struct JITFilterParams {
  CorJitResult *ErrorCode;
};

struct JITFilterCommonParams {
  EXCEPTION_POINTERS ExceptionPointers;
};

// Forward declarations for client defined structures
class GenIR;  // Compiler dependent IR production
class IRNode; // Your compiler intermediate representation
class ReaderStack;

class FlowGraphNode;
class FlowGraphEdgeList;
class BranchList;
class ReaderBitVector;
struct EHRegion;

struct RuntimeFilterParams : JITFilterCommonParams {
  GenIR *This;
};

typedef struct {
  uint32_t NumGcPtrs;
  uint8_t GCLayout[0];
} GCLayoutStruct;

#define GCLS_NUMGCPTRS(X) ((X)->NumGcPtrs)
#define GCLS_GCLAYOUT(X) ((X)->GCLayout)

#define GCLAYOUT_BASE_PTR 1
#define GCLAYOUT_INTERIOR_PTR 2

#ifdef CC_PEVERIFY
extern HRESULT VerLastError;
#endif

// Structure used to pass argument information from rdrCall
// to GenCall.
struct CallArgTriple {
  IRNode *ArgNode;
  CorInfoType ArgType;
  CORINFO_CLASS_HANDLE ArgClass;
};

struct FlowGraphNodeList {
  FlowGraphNode *Block;
  FlowGraphNodeList *Next;
};

struct FlowGraphNodeWorkList {
  FlowGraphNode *Block;
  FlowGraphNodeWorkList *Next;
  FlowGraphNode *Parent;
};

class VerifyWorkList;
class VerificationState;

typedef enum {
  Reader_AlignNatural = (uint8_t)~0, // Default natural alignment
  Reader_AlignUnknown = 0,
  Reader_Align1 = 1,
  Reader_Align2 = 2,
  Reader_Align4 = 4,
  Reader_Align8 = 8,
} ReaderAlignType;

// Used to denote a special symbol when creating syms
typedef enum {
  Reader_NotSpecialSymbol = 0,
  Reader_ThisPtr,
  Reader_UnmodifiedThisPtr,
  Reader_VarArgsToken,
  Reader_InstParam,
  Reader_SecurityObject,
  Reader_GenericsContext
} ReaderSpecialSymbolType;

// Types of pointers.
typedef enum {
  Reader_PtrNotGc = 0,
  Reader_PtrGcBase,
  Reader_PtrGcInterior
} ReaderPtrType;

typedef enum {
  Reader_LocalVerificationException,
  Reader_GlobalVerificationException,
} ReaderExceptionType;

typedef enum {
  MonitorTryFinallyRegion = 0,
  SecurityCalloutTryFinallyRegion
} TryFinallyRegionType;

class ReaderException {
public:
  ReaderExceptionType Type;
};

class FlowGraphNodeOffsetList;

// The TryRegion graph allows us to build a region tree that captures the
// lexical information from the EIT before we begin reading the MSIL opcodes.
// Thus we build the try-region tree, then the flow graph, then fill in the
// the flow graph nodes with IL.

struct EHRegion;
struct EHRegionList;
struct FgData;

//
// Reader Stack
//

typedef struct IRNodeListElement *IRNodeList;

// A node for the push down list we call the reader stack. The
// intent is that the stack is only accessed via push and pop.
class ReaderStackNode;
class ReaderStackIterator;
class ReaderBase;

class ReaderStack {
public:
  virtual IRNode *pop(void) = 0;
  virtual void push(IRNode *NewVal, IRNode **NewIR) = 0;
  virtual void clearStack(void) = 0;
  virtual bool empty(void) = 0;
  virtual void assertEmpty(void) = 0;
  virtual uint32_t depth() = 0;

  // For iteration, implement/use as needed by the client
  virtual IRNode *getIterator(ReaderStackIterator **) = 0;
  virtual IRNode *iteratorGetNext(ReaderStackIterator **) = 0;
  virtual void iteratorReplace(ReaderStackIterator **, IRNode *) = 0;
  virtual IRNode *getReverseIterator(ReaderStackIterator **) = 0;
  virtual IRNode *getReverseIteratorFromDepth(ReaderStackIterator **,
                                              uint32_t Depth) = 0;
  virtual IRNode *reverseIteratorGetNext(ReaderStackIterator **) = 0;

#if defined(_DEBUG)
  virtual void print() = 0;
#endif

  // Creates stack copy, used in non-empty stacks across flow, etc.
  virtual ReaderStack *copy() = 0;
};

class ReaderCallTargetData {
  friend class ReaderBase;

private:
  ReaderBase *Reader;
  mdToken LoadFtnToken;
  bool HasThisPtr;
  bool IsJmp;
  bool IsTailCall;
  bool IsRecursiveTailCall;
  bool IsUnmarkedTailCall;
  bool AreClassAttribsValid;
  bool IsCallInfoValid;
  bool IsCallVirt;
  bool IsCallI;
  bool IsIndirect;
  bool IsNewObj;
  bool NeedsNullCheck;
  bool UsesMethodDesc;
  bool IsOptimizedDelegateCtor;
  bool IsReadonlyCall;

  CorInfoIntrinsics CorIntrinsicId;
  uint32_t TargetMethodAttribs;
  uint32_t TargetClassAttribs;
  CORINFO_METHOD_HANDLE TargetMethodHandle;
  CORINFO_CLASS_HANDLE TargetClassHandle;
  CORINFO_SIG_INFO SigInfo;
  CORINFO_RESOLVED_TOKEN ResolvedToken;
  CORINFO_RESOLVED_TOKEN ResolvedConstraintToken;
  CORINFO_CALL_INFO CallInfo;
  DelegateCtorArgs *CtorArgs;

  IRNode *TargetMethodHandleNode;
  IRNode *TargetClassHandleNode;
  IRNode *IndirectionCellNode;
  IRNode *CallTargetNode;

#if defined(_DEBUG)
  char TargetName[MAX_CLASSNAME_LENGTH];
#endif

  void setIndirectionCellNode(IRNode *Node) { IndirectionCellNode = Node; }
  void fillTargetInfo(mdToken TargetToken, mdToken ConstraintToken,
                      CORINFO_CONTEXT_HANDLE Context,
                      CORINFO_MODULE_HANDLE Scope, CORINFO_METHOD_HANDLE Caller,
                      uint32_t MsilOffset);

  void init(ReaderBase *Reader, mdToken TargetToken, mdToken ConstraintToken,
            mdToken LoadFtnToken, bool IsTailCall, bool IsUnmarkedTailCall,
            bool IsReadonlyCall, ReaderBaseNS::CallOpcode Opcode,
            uint32_t MsilOffset, CORINFO_CONTEXT_HANDLE Context,
            CORINFO_MODULE_HANDLE Scope, CORINFO_METHOD_HANDLE Caller);

public:
  CORINFO_RESOLVED_TOKEN *getResolvedToken() { return &ResolvedToken; }
  mdToken getMethodToken() { return ResolvedToken.token; }
  mdToken getConstraintToken() { return ResolvedConstraintToken.token; }
  CORINFO_RESOLVED_TOKEN *getResolvedConstraintToken() {
    return &ResolvedConstraintToken;
  }
  CORINFO_METHOD_HANDLE getMethodHandle() { return TargetMethodHandle; }
  uint32_t getMethodAttribs() { return TargetMethodAttribs; };
  CORINFO_SIG_INFO *getSigInfo() { return &SigInfo; };
  CORINFO_CALL_INFO *getCallInfo() {
    return IsCallInfoValid ? &CallInfo : NULL;
  }
  IRNode *getIndirectionCellNode() { return IndirectionCellNode; }
  IRNode *getCallTargetNode() { return CallTargetNode; }

  uint32_t getClassAttribs();
  CORINFO_CLASS_HANDLE getClassHandle();

  CORINFO_CONTEXT_HANDLE getExactContext();
  bool getClassHandleNodeRequiresRuntimeLookup();
  bool getTypeContextNodeRequiresRuntimeLookup();
  bool getMethodHandleNodeRequiresRuntimeLookup();
  bool getCallTargetNodeRequiresRuntimeLookup();

  IRNode *getMethodHandleNode(IRNode **NewIR);
  IRNode *getClassHandleNode(IRNode **NewIR);
  IRNode *getTypeContextNode(IRNode **NewIR);

  IRNode *applyThisTransform(IRNode *ThisIR, IRNode **NewIR);

  bool hasThis() { return HasThisPtr; }
  bool isJmp() { return IsJmp; }
  bool isTailCall() { return IsTailCall; }
  bool isRecursiveTailCall() { return IsRecursiveTailCall; }
  bool isUnmarkedTailCall() { return IsUnmarkedTailCall; }
  bool isStubDispatch() { return IndirectionCellNode ? true : false; }
  bool isIndirect() { return IsIndirect; }
  bool isCallI() { return IsCallI; }
  bool isTrueDirect() { return getCallInfo()->kind == CORINFO_CALL; }
  bool isNewObj() { return IsNewObj; }
  bool isCallVirt() { return IsCallVirt; }
  bool needsNullCheck() { return NeedsNullCheck; }
  bool usesMethodDesc() { return UsesMethodDesc; }
  bool isReadOnlyCall() { return IsReadonlyCall; }

  mdToken getLoadFtnToken() { return LoadFtnToken; }
  void setLoadFtnToken(mdToken Token) { LoadFtnToken = Token; }

  bool isBasicCall() {
    return !IsIndirect && !IsNewObj && !isJmp() && isTrueDirect();
  }
  CorInfoIntrinsics getCorInstrinsic() { return CorIntrinsicId; }

  bool recordCommonTailCallChecks(bool CanTailCall) {
    if (!CanTailCall) {
      IsTailCall = false;
      IsRecursiveTailCall = false;
      IsUnmarkedTailCall = false;
      return false;
    }
    return true;
  }

  // Return NULL if we either can't know the call target statically, and if we
  // have a method handle
  // it is only representative.  Otherwise it returns the same as
  // getMethodHandle.  Think of a virtual
  // call, where we have the baseclass or interface method handle, *NOT* the
  // actual target.
  CORINFO_METHOD_HANDLE getKnownMethodHandle() {
    if (IsCallI || !IsCallInfoValid || (CallInfo.kind != CORINFO_CALL))
      return NULL;
    return TargetMethodHandle;
  }

  void setOptimizedDelegateCtor(CORINFO_METHOD_HANDLE NewTargetMethodHandle);
  DelegateCtorArgs *getDelegateCtorData() { return CtorArgs; }

  void resetReader(ReaderBase *Reader);
};

// Interface to GenIR defined EHRegion structure
// Implementation Supplied by Jit Client
EHRegionList *rgnListGetNext(EHRegionList *EhRegionList);
void rgnListSetNext(EHRegionList *EhRegionList, EHRegionList *Next);
EHRegion *rgnListGetRgn(EHRegionList *EhRegionList);
void rgnListSetRgn(EHRegionList *EhRegionList, EHRegion *Rgn);
ReaderBaseNS::RegionKind rgnGetRegionType(EHRegion *EhRegion);
void rgnSetRegionType(EHRegion *EhRegion, ReaderBaseNS::RegionKind Type);
uint32_t rgnGetStartMSILOffset(EHRegion *EhRegion);
void rgnSetStartMSILOffset(EHRegion *EhRegion, uint32_t Offset);
uint32_t rgnGetEndMSILOffset(EHRegion *EhRegion);
void rgnSetEndMSILOffset(EHRegion *EhRegion, uint32_t Offset);
IRNode *rgnGetHead(EHRegion *EhRegion);
void rgnSetHead(EHRegion *EhRegion, IRNode *Head);
IRNode *rgnGetLast(EHRegion *EhRegion);
void rgnSetLast(EHRegion *EhRegion, IRNode *Last);
bool rgnGetIsLive(EHRegion *EhRegion);
void rgnSetIsLive(EHRegion *EhRegion, bool Live);
void rgnSetParent(EHRegion *EhRegion, EHRegion *Parent);
EHRegion *rgnGetParent(EHRegion *EhRegion);
void rgnSetChildList(EHRegion *EhRegion, EHRegionList *Children);
EHRegionList *rgnGetChildList(EHRegion *EhRegion);
bool rgnGetHasNonLocalFlow(EHRegion *EhRegion);
void rgnSetHasNonLocalFlow(EHRegion *EhRegion, bool NonLocalFlow);
IRNode *rgnGetEndOfClauses(EHRegion *EhRegion);
void rgnSetEndOfClauses(EHRegion *EhRegion, IRNode *Node);
IRNode *rgnGetTryBodyEnd(EHRegion *EhRegion);
void rgnSetTryBodyEnd(EHRegion *EhRegion, IRNode *Node);
ReaderBaseNS::TryKind rgnGetTryType(EHRegion *EhRegion);
void rgnSetTryType(EHRegion *EhRegion, ReaderBaseNS::TryKind Type);
int rgnGetTryCanonicalExitOffset(EHRegion *TryRegion);
void rgnSetTryCanonicalExitOffset(EHRegion *TryRegion, int32_t Offset);
EHRegion *rgnGetExceptFilterRegion(EHRegion *EhRegion);
void rgnSetExceptFilterRegion(EHRegion *EhRegion, EHRegion *FilterRegion);
EHRegion *rgnGetExceptTryRegion(EHRegion *EhRegion);
void rgnSetExceptTryRegion(EHRegion *EhRegion, EHRegion *TryRegion);
bool rgnGetExceptUsesExCode(EHRegion *EhRegion);
void rgnSetExceptUsesExCode(EHRegion *EhRegion, bool UsesExceptionCode);
EHRegion *rgnGetFilterTryRegion(EHRegion *EhRegion);
void rgnSetFilterTryRegion(EHRegion *EhRegion, EHRegion *TryRegion);
EHRegion *rgnGetFilterHandlerRegion(EHRegion *EhRegion);
void rgnSetFilterHandlerRegion(EHRegion *EhRegion, EHRegion *Handler);
EHRegion *rgnGetFinallyTryRegion(EHRegion *FinallyRegion);
void rgnSetFinallyTryRegion(EHRegion *FinallyRegion, EHRegion *TryRegion);
bool rgnGetFinallyEndIsReachable(EHRegion *FinallyRegion);
void rgnSetFinallyEndIsReachable(EHRegion *FinallyRegion, bool IsReachable);
EHRegion *rgnGetFaultTryRegion(EHRegion *FinallyRegion);
void rgnSetFaultTryRegion(EHRegion *FinallyRegion, EHRegion *TryRegion);
EHRegion *rgnGetCatchTryRegion(EHRegion *CatchRegion);
void rgnSetCatchTryRegion(EHRegion *CatchRegion, EHRegion *TryRegion);
mdToken rgnGetCatchClassToken(EHRegion *CatchRegion);
void rgnSetCatchClassToken(EHRegion *CatchRegion, mdToken Token);

// Interface to GenIR defined Flow Graph structures.
// Implementation Supplied by Jit Client
EHRegion *fgNodeGetRegion(FlowGraphNode *FgNode);
void fgNodeSetRegion(FlowGraphNode *FgNode, EHRegion *EhRegion);
FlowGraphEdgeList *fgNodeGetSuccessorList(FlowGraphNode *FgNode);
FlowGraphEdgeList *fgNodeGetPredecessorList(FlowGraphNode *FgNode);

// Get the special block-start placekeeping node
IRNode *fgNodeGetStartIRNode(FlowGraphNode *FgNode);

// Get the first non-placekeeping node in block
IRNode *fgNodeGetStartInsertIRNode(FlowGraphNode *FgNode);

// Get the last non-placekeeping node in block
IRNode *fgNodeGetEndInsertIRNode(FlowGraphNode *FgNode);

IRNode *fgNodeGetEndIRInsertionPoint(FlowGraphNode *FgNode);

GlobalVerifyData *fgNodeGetGlobalVerifyData(FlowGraphNode *Fg);
void fgNodeSetGlobalVerifyData(FlowGraphNode *Fg, GlobalVerifyData *GvData);

uint32_t fgNodeGetBlockNum(FlowGraphNode *Fg);

FlowGraphEdgeList *fgEdgeListGetNextSuccessor(FlowGraphEdgeList *FgEdge);
FlowGraphEdgeList *fgEdgeListGetNextPredecessor(FlowGraphEdgeList *FgEdge);
FlowGraphNode *fgEdgeListGetSource(FlowGraphEdgeList *FgEdge);
FlowGraphNode *fgEdgeListGetSink(FlowGraphEdgeList *FgEdge);
bool fgEdgeListIsNominal(FlowGraphEdgeList *FgEdge);
#ifdef CC_PEVERIFY
void fgEdgeListMakeFake(FlowGraphEdgeList *FgEdge);
#endif

FlowGraphEdgeList *fgEdgeListGetNextSuccessorActual(FlowGraphEdgeList *FgEdge);
FlowGraphEdgeList *
fgEdgeListGetNextPredecessorActual(FlowGraphEdgeList *FgEdge);
FlowGraphEdgeList *fgNodeGetSuccessorListActual(FlowGraphNode *Fg);
FlowGraphEdgeList *fgNodeGetPredecessorListActual(FlowGraphNode *Fg);

// Interface to GenIR defined IRNode structure
// Implementation Supplied by Jit Client

IRNode *irNodeGetNext(IRNode *Node);
bool irNodeIsBranch(IRNode *Node);

IRNode *irNodeGetInsertPointAfterMSILOffset(IRNode *Node, uint32_t Offset);
IRNode *irNodeGetInsertPointBeforeMSILOffset(IRNode *Node, uint32_t Offset);
IRNode *
irNodeGetFirstLabelOrInstrNodeInEnclosingBlock(IRNode *HandlerStartNode);
uint32_t irNodeGetMSILOffset(IRNode *Node);
void irNodeLabelSetMSILOffset(IRNode *Node, uint32_t Offset);
void irNodeBranchSetMSILOffset(IRNode *BranchNode, uint32_t Offset);
void irNodeExceptSetMSILOffset(IRNode *BranchNode, uint32_t Offset);
void irNodeInsertBefore(IRNode *InsertionPointTuple, IRNode *NewNode);
void irNodeInsertAfter(IRNode *InsertionPointTuple, IRNode *NewNode);
void irNodeSetRegion(IRNode *Node, EHRegion *Region);
EHRegion *irNodeGetRegion(IRNode *Node);
FlowGraphNode *irNodeGetEnclosingBlock(IRNode *Node);
bool irNodeIsLabel(IRNode *Node);
bool irNodeIsEHFlowAnnotation(IRNode *Node);
bool irNodeIsHandlerFlowAnnotation(IRNode *Node);

// Interface to GenIR defined BranchList structure
// Implementation Supplied by Jit Client.
BranchList *branchListGetNext(BranchList *BranchList);
IRNode *branchListGetIRNode(BranchList *BranchList);

struct VerificationBranchInfo {
  uint32_t SrcOffset;
  uint32_t TargetOffset;
  IRNode *BranchOp;
  bool IsLeave;

  VerificationBranchInfo *Next;
};

ReaderBaseNS::OPCODE parseMSILOpcode(uint8_t *ILCursor, uint8_t **OperandCursor,
                                     uint32_t *Increment, ReaderBase *Reader);
ReaderBaseNS::OPCODE
parseMSILOpcodeSafely(uint8_t *ILInput, uint32_t CurretOffset,
                      uint32_t ILInputSize, uint8_t **Operand,
                      uint32_t *NextOffset, ReaderBase *Reader,
                      bool ReportError);

ReaderBaseNS::CallOpcode remapCallOpcode(ReaderBaseNS::OPCODE Opcode);

struct ReadBytesForFlowGraphNodeHelperParam {
  ReaderBase *This;
  ReaderException *Excep;
  FlowGraphNode *Fg;
  bool IsVerifyOnly;
  IRNode **NewIR; // Used in a trace pr post process
  uint32_t CurrentOffset;
  bool LocalFault;
  bool HasFallThrough;
  VerificationState *VState;
  bool VerifiedEndBlock;
};

class LoadTracker;

static const int32_t SizeOfCEECall = 5;

class ReaderBase {
  friend class ReaderCallTargetData;

public:
  // Public because it is read and written for inlining support.
  CORINFO_METHOD_INFO *MethodInfo;

  // Normally the same as m_methodInfo->ftn, except for when inlining
  // shared generic methods and the call site provides instantiation
  // information.
  CORINFO_CONTEXT_HANDLE ExactContext;

  // This is the root method being compiled not any inlinee
  CORINFO_METHOD_HANDLE MethodBeingCompiled;

  // The reader's operand stack. Public because of inlining and debug prints
  ReaderStack *ReaderOperandStack;

  // Public for debug printing
  EHRegion *CurrentRegion;

  FlowGraphNode *CurrentFgNode;

  bool HasLocAlloc;
  uint32_t CurrInstrOffset; // current instruction IL offset

private:
  // Private data (not available to derived client class)
  ICorJitInfo *JitInfo;
  uint32_t Flags; // original flags that were passed to compileMethod

  // SEQUENCE POINT Info
  ReaderBitVector *CustomSequencePoints;

  // EH Info
  CORINFO_EH_CLAUSE *EhClauseInfo; // raw eh clause info
  EHRegion *EhRegionTree;
  EHRegionList *AllRegionList;

  // Fg Info - unused after fg is built

  // NodeOffsetListArray is an ordered array of FlowGraphNodeOffsetList*.
  // Each entry is a pointer to a FlowGraphNodeOffsetList which will contain at
  // most LABEL_LIST_ARRAY_STRIDE elements. The FlowGraphNodeOffsetLists
  // are maintained in order, which helps later replacement of temp branch
  // targets with real ones.
  FlowGraphNodeOffsetList **NodeOffsetListArray;
  uint32_t NodeOffsetListArraySize;

  VerificationBranchInfo *BranchesToVerify;

  // Block array, maps fg node blocknum to optional block data
  FgData **BlockArray;

protected:
  // how we track the pushes/pops to/from the stack across a basic block
  LoadTracker *LoadTracking;
  uint32_t CurrentBranchDepth;

  // Verification Info
public:
  bool VerificationNeeded;
  bool IsVerifiableCode; // valid only if VerificationNeeded is set
  bool VerHasCircularConstraints;
  bool NeedsRuntimeCallout;

private:
  bool VerTrackObjCtorInitState;
  bool VerThisInitialized;
  uint32_t NumVerifyParams;
  uint32_t NumVerifyAutos;
  bool ThisPtrModified;
  VerType *ParamVerifyMap;
  VerType *AutoVerifyMap;
  mdToken VerLastToken;
  mdToken LastLoadToken;
  ReaderBitVector *LegalTargetOffsets;

#ifdef CC_PEVERIFY
protected:
  // PEverify needs to squirrel away some info on method init to use later
  void *PEVerifyErrorHandler;
  void *PEVerifyThis;

public:
#define ERROR_MSG_SIZE 4096
  WCHAR ExtendedErrorMessage[ERROR_MSG_SIZE];
#endif
protected:
private:
  // Global Verification Info
  uint16_t *GvStackPop;
  uint16_t *GvStackPush;
  GlobalVerifyData *GvWorklistHead;
  GlobalVerifyData *GvWorklistTail;

public:
  bool AreInlining;
  ReaderBase(ICorJitInfo *CorJitInfo, CORINFO_METHOD_INFO *MethodInfo,
             uint32_t Flags);

  // Main Reader Entry
  void msilToIR(void);

  // Call CreateSym for each param and auto. Also return function bytecode
  // start and length.
  void initParamsAndAutos(uint32_t NumParam, uint32_t NumAuto);
  void handleNonEmptyStack(FlowGraphNode *Fg, IRNode **NewIR, bool *FmbAssign);

  // Needed by inlining so public
  FlowGraphNode *buildFlowGraph(FlowGraphNode **FgTail);
  FlowGraphNode *fgSplitBlock(FlowGraphNode *Block, uint32_t Offset,
                              IRNode *Node);
  CORINFO_ARG_LIST_HANDLE argListNext(CORINFO_ARG_LIST_HANDLE ArgListHandle,
                                      CORINFO_SIG_INFO *Sig,
                                      CorInfoType *CorType = NULL,
                                      CORINFO_CLASS_HANDLE *Class = NULL,
                                      bool *IsPinned = NULL);
  void buildUpParams(uint32_t NumParams);
  void buildUpAutos(uint32_t NumAutos);
#if defined(_DEBUG)
  // Debug-only reader function to print range of MSIL.
  void printMSIL(uint8_t *Buf, uint32_t StartOffset, uint32_t EndOffset);
#endif

  void getMSILInstrStackDelta(ReaderBaseNS::OPCODE Opcode, uint8_t *Operand,
                              uint16_t *Pop, uint16_t *Push);

private:
  bool isUnmarkedTailCall(uint8_t *ILInput, uint32_t ILInputSize,
                          uint32_t NextOffset, mdToken Token);
  bool isUnmarkedTailCallHelper(uint8_t *ILInput, uint32_t ILInputSize,
                                uint32_t NextOffset, mdToken Token);
  bool checkExplicitTailCall(uint32_t ILOffset, bool AllowPop);

  // Reduce given block from MSIL to IR
  void readBytesForFlowGraphNode(FlowGraphNode *Fg, bool IsVerifyOnly);
  void
  readBytesForFlowGraphNode_Helper(ReadBytesForFlowGraphNodeHelperParam *Param);

public:
  void initVerifyInfo(void);

private:
  void setupBlockForEH(IRNode **NewIR);

  bool isOffsetInstrStart(uint32_t Offset);

  // SEQUENCE POINTS
  void getCustomSequencePoints();

  //
  // FlowGraph
  //

  FlowGraphNode *fgBuildBasicBlocksFromBytes(uint8_t *Buffer,
                                             uint32_t BufferSize);
  void fgBuildPhase1(FlowGraphNode *Fg, uint8_t *Buffer, uint32_t BufferSize);
  void fgAttachGlobalVerifyData(FlowGraphNode *HeadBlock);
  void fgAddArcs(FlowGraphNode *HeadBlock);
  IRNode *fgAddCaseToCaseListHelper(IRNode *SwitchNode, IRNode *LabelNode,
                                    uint32_t Element);
  FlowGraphNodeWorkList *
  fgAppendUnvisitedSuccToWorklist(FlowGraphNodeWorkList *Worklist,
                                  FlowGraphNode *CurrBlock);
  void fgDeleteBlockAndNodes(FlowGraphNode *Block);
  void fgEnsureEnclosingRegionBeginsWithLabel(IRNode *HandlerStartNode);
  EHRegion *fgGetRegionFromMSILOffset(uint32_t Offset);
  FlowGraphNode *fgReplaceBranchTarget(uint32_t Offset,
                                       FlowGraphNode *TempBranchTarget,
                                       FlowGraphNode *StartBlock);
  void fgReplaceBranchTargets(void);
  void fgInsertTryEnd(EHRegion *EhRegion);
  void fgInsertBeginRegionExceptionNode(uint32_t Offset, IRNode *EHNode);
  void fgInsertEndRegionExceptionNode(uint32_t Offset, IRNode *EHNode);
  void fgInsertEHAnnotations(EHRegion *Region);
  IRNode *fgMakeBranchHelper(IRNode *LabelNode, IRNode *BlockNode,
                             uint32_t Offset, bool IsConditional,
                             bool IsNominal);
  IRNode *fgMakeEndFinallyHelper(IRNode *BlockNode, uint32_t Offset,
                                 bool IsLexicalEnd);
  void fgRemoveUnusedBlocks(FlowGraphNode *FgHead, FlowGraphNode *FgTail);
  uint32_t fgGetRegionCanonicalExitOffset(EHRegion *Region);
  int32_t *FgGetRegionCanonicalExitOffsetBuff;

  // DomInfo - get and set properties of dominators
  void initBlockArray(uint32_t BlockCount);
  void *domInfoGetInfoFromDominator(
      FlowGraphNode *Fg, CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2,
      bool *Key3, bool RequireSameRegion,
      void *(FgData::*Pmfn)(CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2,
                            bool *Key3));

  FgData *domInfoGetBlockData(FlowGraphNode *Fg, bool DoCreate);
  IRNode *domInfoDominatorDefinesSharedStaticBase(FlowGraphNode *Fg,
                                                  CorInfoHelpFunc &HelperID,
                                                  CORINFO_CLASS_HANDLE Class,
                                                  bool *NoCtor);
  void domInfoRecordSharedStaticBaseDefine(FlowGraphNode *Fg,
                                           CorInfoHelpFunc HelperID,
                                           CORINFO_CLASS_HANDLE Class,
                                           IRNode *BasePtr);
  bool domInfoDominatorHasClassInit(FlowGraphNode *Fg,
                                    CORINFO_CLASS_HANDLE Class);
  void domInfoRecordClassInit(FlowGraphNode *Fg, CORINFO_CLASS_HANDLE Class);

  FlowGraphNodeOffsetList *fgAddNodeMSILOffset(FlowGraphNode **Node,
                                               uint32_t TargetOffset);
  bool fgLeaveIsNonLocal(FlowGraphNode *Fg, uint32_t LeaveOffset,
                         uint32_t LeaveTarget, bool *EndsWithNonLocalGoto);

  // =============================================================================
  // =============================================================================
  // =======    EIT Verification ===============================================
  // =============================================================================
  // =============================================================================

  // these types are for EIT verification only
  struct EHNodeDescriptor;
  struct EITVerBasicBlock;
  struct EHBlockDescriptor;
  typedef UINT32 ILOffset;

  EHNodeDescriptor *EhnTree; // root of the tree comprising the EHnodes.
  EHNodeDescriptor *EhnNext; // root of the tree comprising the EHnodes.
  EITVerBasicBlock *VerBasicBlockList;
  UINT VerBasicBlockCount;
  EHBlockDescriptor *CompBasicBlockTab;
  ILOffset VerInstrStartOffset;
  ReaderBaseNS::OPCODE VerInstrOpcode;

  void verifyEIT(); // the entry point
  EITVerBasicBlock *verEITAddBlock(ILOffset Start, ILOffset End);
  EITVerBasicBlock *verLookupBasicBlock(UINT32 X);

  void verInitEHTree(uint32_t NumEHClauses);
  void verInsertEhNode(CORINFO_EH_CLAUSE *Clause,
                       EHBlockDescriptor *HandlerTab);
  void verInsertEhNodeInTree(EHNodeDescriptor **Root, EHNodeDescriptor *Node);
  void verInsertEhNodeParent(EHNodeDescriptor **Root, EHNodeDescriptor *Node);
  void verCheckNestingLevel(EHNodeDescriptor *Root);
  void verDispHandlerTab();

  inline ILOffset ebdTryEndOffset(EHBlockDescriptor *EhBlock);
  inline uint32_t ebdTryEndBlockNum(EHBlockDescriptor *EhBlock);
  inline ILOffset ebdHndEndOffset(EHBlockDescriptor *EhBlock);
  inline uint32_t ebdHndEndBlockNum(EHBlockDescriptor *EhBlock);

  // =============================================================================
  // =======  EHRegion Builder =============================================
  // =============================================================================

  void rgnCreateRegionTree(void);
  void rgnPushRegionChild(EHRegion *Parent, EHRegion *Child);

public:
  EHRegion *rgnMakeRegion(ReaderBaseNS::RegionKind Type, EHRegion *Parent,
                          EHRegion *RegionRoot, EHRegionList **AllRegionList);

private:
  // //////////////////////////////////////////////////////////////////////
  //                           Verification methods
  //
  // Opcode specific verification routines that "throw(verErr)" to insert
  // throw into the native code stream. You need the flowgraph node for the
  // throw object, the opcode for factoring and the curPtr to pick up the
  // operand to any MSIL opcodes like the token for CEE_CALL etc.
  //
  // NOTE These could all be factored out into another class
  // //////////////////////////////////////////////////////////////////////

public:
  void verifyCompatibleWith(const VerType &A, const VerType &B);
  void verifyEqual(const VerType &A, const VerType &B);
  void verifyEqualNotEquivalent(const VerType &A, const VerType &B);
  void verifyAndReportFound(int32_t Cond, const VerType &Type, HRESULT Message);
  void verifyAndReportFound(int32_t Cond, const VerType &Type,
                            const char *Message);
  void verifyIsNumberType(const VerType &Type);
  void verifyIsIntegerType(const VerType &Type);
  void verifyIsObjRef(const VerType &Type);
  void verifyIsByref(const VerType &Type);
  void verifyIsBoxable(const VerType &Type);
  void verifyIsNotUnmanaged(const VerType &Type);
  void verifyTypeIsValid(const VerType &Type);

#ifndef CC_PEVERIFY
  void printVerificationErrorMessage(VerErrType Type, const char *Message,
                                     const VerType *Expected,
                                     const VerType *Encountered, mdToken Token,
                                     bool AndThrow);
  void verifyOrReturn(int32_t Cond, const char *Message);
  void gverifyOrReturn(int32_t Cond, const char *Message);
  void verGlobalError(const char *Message);
#else
  void printVerificationErrorMessage(VerErrType Type, const char *Message,
                                     const VerType *Expected,
                                     const VerType *Encountered, mdToken Token,
                                     bool AndThrow);
  void printVerificationErrorMessage(VerErrType Type, HRESULT Message,
                                     const VerType *Expected,
                                     const VerType *Encountered, mdToken Token,
                                     bool AndThrow);
  void verifyOrReturn(int32_t Cond, HRESULT Code);
  void gverifyOrReturn(int32_t Cond, HRESULT Message);
  void verGlobalError(HRESULT Message);
  // @todo get rid of
  void verifyOrReturn(int32_t Cond, const char *Message);
  void gverifyOrReturn(int32_t Cond, const char *Message);
  void verGlobalError(const char *Message);
#endif

  struct JITFilterParam {
    ICorJitInfo *JitInfo;
    EXCEPTION_POINTERS ExceptionPointers;
  };
  static LONG eeJITFilter(PEXCEPTION_POINTERS ExceptionPointersPtr,
                          void *Param);

protected:
  void clearStack(IRNode **NewIR);

  // Client defined function to initialize verification state.
  void verifyNeedsVerification();
  VerificationState *verifyInitializeBlock(FlowGraphNode *, uint32_t ILOffset);
  void verPropEHInitFlow(FlowGraphNode *Block);
  void verPropHandlerInitFlow(FlowGraphNode *Block);

  VerificationState *verCreateNewVState(uint32_t MaxStack, uint32_t NumLocals,
                                        bool InitLocals, InitState InitState);
  void verifyFinishBlock(VerificationState *VState, FlowGraphNode *);
  void verifyPropCtorInitToSucc(InitState CurrentState,
                                FlowGraphNode *Successor, char *Reason);
  void verifyPropCtorInitThroughBadBlock(FlowGraphNode *Block);
  FlowGraphNode *verifyGetRegionBlock(EHRegion *Region);
  void verifyEnqueueBlock(GlobalVerifyData *GvSuccessor);
  FlowGraphNode *verifyFindFaultHandlerBlock(VerificationState *VState,
                                             EHRegion *TryRegion);

  void verInitCurrentState();

  void verifyRecordBranchForVerification(IRNode *Branch, uint32_t SourceOffset,
                                         uint32_t TargetOffset, bool IsLeave);

  void verifyRecordLocalType(uint32_t Num, CorInfoType Type,
                             CORINFO_CLASS_HANDLE Class);
  void verifyRecordParamType(uint32_t Num, CorInfoType Type,
                             CORINFO_CLASS_HANDLE Class, bool MakeByRef,
                             bool IsThis);
  void verifyRecordParamType(uint32_t Num, CORINFO_SIG_INFO *Sig,
                             CORINFO_ARG_LIST_HANDLE Args);
  void verifyRecordLocalType(uint32_t Num, CORINFO_SIG_INFO *Sig,
                             CORINFO_ARG_LIST_HANDLE Args);
  void verifyPushExceptionObject(VerificationState *VState, mdToken);
  void verifyFieldAccess(VerificationState *VState, ReaderBaseNS::OPCODE Opcode,
                         CORINFO_RESOLVED_TOKEN *ResolvedToken);
  bool verIsCallToInitThisPtr(CORINFO_CLASS_HANDLE Context,
                              CORINFO_CLASS_HANDLE Target);
  void verifyLoadElemA(VerificationState *VState, bool HasReadOnlyPrefix,
                       CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyLoadElem(VerificationState *VState, ReaderBaseNS::OPCODE Opcode,
                      CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyLoadConstant(VerificationState *VState,
                          ReaderBaseNS::OPCODE Opcode);
  void verifyStoreObj(VerificationState *VState,
                      CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyLoadObj(VerificationState *VState,
                     CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyStloc(VerificationState *VState, uint32_t LocalNumber);
  void verifyIsInst(VerificationState *VState,
                    CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyCastClass(VerificationState *VState,
                       CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyBox(VerificationState *VState,
                 CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyLoadAddr(VerificationState *VState);
  void verifyLoadToken(VerificationState *VState,
                       CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyUnbox(VerificationState *VState,
                   CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyStoreElemRef(VerificationState *VState);
  void verifyLdarg(VerificationState *VState, uint32_t LocalNumber,
                   ReaderBaseNS::OPCODE Opcode);
  void verifyStarg(VerificationState *VState, uint32_t LocalNumber);
  void verifyLdloc(VerificationState *VState, uint32_t LocalNumber,
                   ReaderBaseNS::OPCODE Opcode);
  void verifyStoreElem(VerificationState *VState, ReaderBaseNS::StElemOpcode,
                       CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyLoadLen(VerificationState *VState);
  void verifyDup(VerificationState *VState, const uint8_t *CodeAddress);
  void verifyEndFilter(VerificationState *VState, uint32_t ILOffset);
  void verifyInitObj(VerificationState *VState,
                     CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyCall(VerificationState *VState, ReaderBaseNS::OPCODE Opcode,
                  bool IsTailCall, bool IsReadOnlyCall, bool IsConstraintCall,
                  bool IsThisPossiblyModified, mdToken ConstraintTypeRef,
                  mdToken Token);
  void verifyCpObj(VerificationState *VState,
                   CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyNewObj(VerificationState *VState, ReaderBaseNS::OPCODE Opcode,
                    bool SsTail, CORINFO_RESOLVED_TOKEN *ResolvedToken,
                    const uint8_t *CodeAddress);
  void verifyBoolBranch(VerificationState *VState, uint32_t NextOffset,
                        uint32_t TargetOffset);
  void verifyLoadNull(VerificationState *VState);
  void verifyLoadStr(VerificationState *VState, mdToken Token);
  void verifyIntegerBinary(VerificationState *VState);
  void verifyBinary(VerificationState *VState, ReaderBaseNS::OPCODE Opcode);
  void verifyShift(VerificationState *VState);
  void verifyReturn(VerificationState *VState, EHRegion *Region);
  void verifyEndFinally(VerificationState *VState);
  void verifyThrow(VerificationState *VState);
  void verifyLoadFtn(VerificationState *VState, ReaderBaseNS::OPCODE Opcode,
                     CORINFO_RESOLVED_TOKEN *ResolvedToken,
                     const uint8_t *CodeAddress, CORINFO_CALL_INFO *CallInfo);
  void verifyNewArr(VerificationState *VState,
                    CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyLoadIndirect(VerificationState *VState,
                          ReaderBaseNS::LdIndirOpcode Opcode);
  void verifyStoreIndir(VerificationState *VState,
                        ReaderBaseNS::StIndirOpcode Opcode);
  void verifyConvert(VerificationState *VState,
                     ReaderBaseNS::ConvOpcode Opcode);
  void verifyCompare(VerificationState *VState, ReaderBaseNS::OPCODE Opcode);
  void verifyUnary(VerificationState *VState, ReaderBaseNS::UnaryOpcode Opcode);
  void verifyPop(VerificationState *VState);
  void verifyArgList(VerificationState *VState);
  void verifyCkFinite(VerificationState *VState);
  void verifyFailure(VerificationState *VState);
  void verifyToken(mdToken Token);
  void verifyRefAnyVal(VerificationState *VState,
                       CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyRefAnyType(VerificationState *VState);
  void verifyUnboxAny(VerificationState *VState,
                      CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifySwitch(VerificationState *VState);
  void verifyMkRefAny(VerificationState *VState,
                      CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifySizeOf(VerificationState *VState,
                    CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void verifyRethrow(VerificationState *VState, EHRegion *Region);
  void verifyTail(VerificationState *VState, EHRegion *Region);
  void verifyConstrained(VerificationState *VState, mdToken TypeDefOrRefOrSpec);
  void verifyReadOnly(VerificationState *VState);
  void verifyVolatile(VerificationState *VState);
  void verifyUnaligned(VerificationState *VState, ReaderAlignType Alignment);
  void verifyPrefixConsumed(VerificationState *VState,
                            ReaderBaseNS::OPCODE Opcode);
  void verifyLeave(VerificationState *VState);

  void verifyBranchTarget(VerificationState *VState,
                          FlowGraphNode *CurrentFGNode, EHRegion *SourceRegion,
                          uint32_t TargetOffset, bool IsLeave);
  void verifyReturnFlow(uint32_t SourceOffset);

  void verifyFallThrough(VerificationState *VState, FlowGraphNode *Fg);

  bool verCheckDelegateCreation(ReaderBaseNS::OPCODE Opcode,
                                VerificationState *VState,
                                const uint8_t *CodeAddress,
                                mdMemberRef &TargetMemberRef,
                                VerType FunctionType, VerType ObjectType);

  void verVerifyCall(ReaderBaseNS::OPCODE Opcode,
                     const CORINFO_RESOLVED_TOKEN *ResolvedToken,
                     const CORINFO_CALL_INFO *CallInfo, bool IsTailCall,
                     const uint8_t *CodeAddress, VerificationState *VState);

  void verifyIsMethodToken(mdToken Token);
  void verifyIsCallToken(mdToken Token);
  void verVerifyField(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                      const CORINFO_FIELD_INFO &FieldInfo,
                      const VerType *ThisType, bool IsMutator);
  bool verIsValueClass(CORINFO_CLASS_HANDLE Class);
  bool verIsBoxedValueType(const VerType &Type);
  static bool verIsCallToken(mdToken Token);
  bool verIsValClassWithStackPtr(CORINFO_CLASS_HANDLE Class);
  bool verIsGenericTypeVar(CORINFO_CLASS_HANDLE Class);
  void verDumpType(const VerType &Type);
  bool verNeedsCtorTrack();
  void verifyIsClassToken(mdToken Token);
  void verifyIsFieldToken(mdToken Token);

  VerType verVerifySTIND(const VerType &Pointer, const VerType &Value,
                         TITypes InstrType);
  VerType verVerifyLDIND(const VerType &Pointer, TITypes InstrType);

  // methods
  VerType verGetArrayElemType(VerType Type);
  VerType verMakeTypeInfo(CORINFO_CLASS_HANDLE Class);
  VerType verMakeTypeInfo(CorInfoType TheCorInfoType,
                          CORINFO_CLASS_HANDLE Class);
  VerType verParseArgSigToTypeInfo(CORINFO_SIG_INFO *Sig,
                                   CORINFO_ARG_LIST_HANDLE Args);

  CORINFO_CLASS_HANDLE
  getTokenTypeAsHandle(CORINFO_RESOLVED_TOKEN *ResolvedToken) {
    return JitInfo->getTokenTypeAsHandle(ResolvedToken);
  }
  void verCheckClassAccess(CORINFO_RESOLVED_TOKEN *ResolvedToken);

  void eeGetMethodSig(CORINFO_METHOD_HANDLE Method, CORINFO_SIG_INFO *SigRet,
                      bool GiveUp, CORINFO_CLASS_HANDLE Owwner = NULL);
  void eeGetCallSiteSig(uint32_t SigToken, CORINFO_MODULE_HANDLE Scope,
                        CORINFO_CONTEXT_HANDLE Context,
                        CORINFO_SIG_INFO *SigRet, bool GiveUp = true);

  void verifyIsSDArray(const VerType &Type);
  bool verIsByRefLike(const VerType &Type);
  bool verIsSafeToReturnByRef(const VerType &Type);
  bool verIsBoxable(const VerType &Type);

public:
  // ///////////////////////////////////////////////////////////////////////////
  //                           VOS opcode methods
  // These methods are available to help implement codegen for cee opcodes.
  // These methods are implemented using the optional client methods declared
  // at the end of this file.
  // ///////////////////////////////////////////////////////////////////////////

  IRNode *rdrCall(ReaderCallTargetData *CallTargetData,
                  ReaderBaseNS::CallOpcode Opcode, IRNode **CallNode,
                  IRNode **NewIR);

  void makeReaderCallTargetDataForNewObj(ReaderCallTargetData *CallTargetData,
                                         mdToken TargetToken,
                                         mdToken LoadFtnToken) {
    CallTargetData->init(this, TargetToken, mdTokenNil, LoadFtnToken, false,
                         false, false, ReaderBaseNS::NewObj, 0,
                         getCurrentContext(), getCurrentModuleHandle(),
                         getCurrentMethodHandle());
  }
  void makeReaderCallTargetDataForNewObj(ReaderCallTargetData *CallTargetData,
                                         mdToken TargetToken,
                                         mdToken LoadFtnToken,
                                         CORINFO_CONTEXT_HANDLE Context,
                                         CORINFO_MODULE_HANDLE Scope,
                                         CORINFO_METHOD_HANDLE Caller) {
    CallTargetData->init(this, TargetToken, mdTokenNil, LoadFtnToken, false,
                         false, false, ReaderBaseNS::NewObj, 0, Context, Scope,
                         Caller);
  }

  void makeReaderCallTargetDataForJmp(ReaderCallTargetData *CallTargetData,
                                      mdToken TargetToken) {
    CallTargetData->init(this, TargetToken, mdTokenNil, mdTokenNil, false,
                         false, false, ReaderBaseNS::Jmp, 0,
                         getCurrentContext(), getCurrentModuleHandle(),
                         getCurrentMethodHandle());
  }

  void makeReaderCallTargetDataForCall(ReaderCallTargetData *CallTargetData,
                                       mdToken TargetToken,
                                       mdToken ConstraintToken, bool IsTailCall,
                                       bool IsUnmarkedTailCall,
                                       bool IsReadonlyCall,
                                       ReaderBaseNS::CallOpcode Opcode,
                                       uint32_t MsilOffset) {
    CallTargetData->init(this, TargetToken, ConstraintToken, mdTokenNil,
                         IsTailCall, IsUnmarkedTailCall, IsReadonlyCall, Opcode,
                         MsilOffset, getCurrentContext(),
                         getCurrentModuleHandle(), getCurrentMethodHandle());
  }

  void makeReaderCallTargetDataForCall(
      ReaderCallTargetData *CallTargetData, mdToken TargetToken,
      mdToken ConstraintToken, bool IsReadonlyCall,
      ReaderBaseNS::CallOpcode Opcode, CORINFO_CONTEXT_HANDLE Context,
      CORINFO_MODULE_HANDLE Scope, CORINFO_METHOD_HANDLE Caller) {
    CallTargetData->init(this, TargetToken, ConstraintToken, mdTokenNil, false,
                         false, IsReadonlyCall, Opcode, 0, Context, Scope,
                         Caller);
  }

private:
  void rdrMakeCallTargetNode(ReaderCallTargetData *CallTargetData,
                             IRNode **ThisPointer, IRNode **NewIR);

  IRNode *rdrMakeLdFtnTargetNode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                 CORINFO_CALL_INFO *CallInfo, IRNode **NewIR);

  IRNode *rdrGetDirectCallTarget(ReaderCallTargetData *CallTargetData,
                                 IRNode **NewIR);

  IRNode *rdrGetDirectCallTarget(CORINFO_METHOD_HANDLE Method,
                                 mdToken MethodToken, bool NeedsNullCheck,
                                 bool CanMakeDirectCall, bool &UsesMethodDesc,
                                 IRNode **NewIR);
  IRNode *
  rdrGetCodePointerLookupCallTarget(ReaderCallTargetData *CallTargetData,
                                    IRNode **NewIR);

  IRNode *rdrGetCodePointerLookupCallTarget(CORINFO_CALL_INFO *CallInfo,
                                            bool &IsIndirect, IRNode **NewIR);

  IRNode *rdrGetIndirectVirtualCallTarget(ReaderCallTargetData *CallTargetData,
                                          IRNode **ThisPointer, IRNode **NewIR);

  IRNode *rdrGetVirtualStubCallTarget(ReaderCallTargetData *CallTargetData,
                                      IRNode **NewIR);

  IRNode *rdrGetVirtualTableCallTarget(ReaderCallTargetData *CallTargetData,
                                       IRNode **ThisPointer, IRNode **NewIR);

  // Delegate invoke and delegate construct optimizations
  bool rdrCallIsDelegateInvoke(ReaderCallTargetData *CallTargetData);
  bool rdrCallIsDelegateConstruct(ReaderCallTargetData *CallTargetData);
#ifdef FEATURE_CORECLR
  void rdrInsertCalloutForDelegate(CORINFO_CLASS_HANDLE DelegateType,
                                   CORINFO_METHOD_HANDLE CalleeMethod,
                                   mdToken MethodToken, IRNode **NewIR);
#endif // FEATURE_CORECLR
  IRNode *rdrGetDelegateInvokeTarget(ReaderCallTargetData *CallTargetData,
                                     IRNode **ThisPtr, IRNode **NewIR);

public:
  void
  rdrCallFieldHelper(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                     CorInfoHelpFunc HelperId, bool IsLoad,
                     IRNode *Dst, // dst node if this is a load, otherwise NULL
                     IRNode *Obj, IRNode *Value, ReaderAlignType Alignment,
                     bool IsVolatile, IRNode **NewIR);
  void rdrCallWriteBarrierHelper(IRNode *Arg1, IRNode *Arg2,
                                 ReaderAlignType Alignment, bool IsVolatile,
                                 IRNode **NewIR,
                                 CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                 bool IsNonValueClass, bool IsValueIsPointer,
                                 bool IsFieldToken, bool IsUnchecked);
  void rdrCallWriteBarrierHelperForReturnValue(IRNode *Arg1, IRNode *Arg2,
                                               IRNode **NewIR, mdToken Token);
  IRNode *rdrGetFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             CORINFO_FIELD_INFO *FieldInfo, IRNode *Obj,
                             bool BaseIsGCObj, bool MustNullCheck,
                             IRNode **NewIR);
  IRNode *rdrGetStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                   CORINFO_FIELD_INFO *FieldInfo,
                                   IRNode **NewIR);
  IRNode *rdrCallGetStaticBase(CORINFO_CLASS_HANDLE Class, mdToken ClassToken,
                               CorInfoHelpFunc HelperId, bool NoCtor,
                               bool CanMoveUp, IRNode *Dst, IRNode **NewIR);

public:
  // //////////////////////////////////////////////////////////////////////////
  // Metadata Accessors
  // //////////////////////////////////////////////////////////////////////////

  // routines to map token to handle.
  void resolveToken(mdToken Token, CorInfoTokenKind TokenType,
                    CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void resolveToken(mdToken Token, CORINFO_CONTEXT_HANDLE Context,
                    CORINFO_MODULE_HANDLE Scope, CorInfoTokenKind TokenType,
                    CORINFO_RESOLVED_TOKEN *ResolvedToken);

  InfoAccessType constructStringLiteral(mdToken Token, void **Info);

  void *getEmbedModuleDomainIDForStatics(CORINFO_CLASS_HANDLE Class,
                                         bool *IsIndirect);
  void *getEmbedClassDomainID(CORINFO_CLASS_HANDLE Class, bool *IsIndirect);

  CORINFO_METHOD_HANDLE embedMethodHandle(CORINFO_METHOD_HANDLE Method,
                                          bool *IsIndirect);
  CORINFO_CLASS_HANDLE embedClassHandle(CORINFO_CLASS_HANDLE Class,
                                        bool *IsIndirect);
  CORINFO_FIELD_HANDLE embedFieldHandle(CORINFO_FIELD_HANDLE Field,
                                        bool *IsIndirect);

  void embedGenericHandle(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                          bool ShouldEmbedParent,
                          CORINFO_GENERICHANDLE_RESULT *Result);

  void getCallSiteSignature(CORINFO_METHOD_HANDLE Method, mdToken Token,
                            CORINFO_SIG_INFO *Sig, bool *HasThis);
  void getCallSiteSignature(CORINFO_METHOD_HANDLE Method, mdToken Token,
                            CORINFO_SIG_INFO *Sig, bool *HasThis,
                            CORINFO_CONTEXT_HANDLE Context,
                            CORINFO_MODULE_HANDLE Scope);

  // Gets a CALL_INFO with all virutal call info filled in
  void getCallInfo(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                   CORINFO_RESOLVED_TOKEN *ConstrainedResolvedToken,
                   CORINFO_CALLINFO_FLAGS Flags, CORINFO_CALL_INFO *Result);

  void getCallInfo(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                   CORINFO_RESOLVED_TOKEN *ConstrainedResolvedToken,
                   CORINFO_CALLINFO_FLAGS Flags, CORINFO_CALL_INFO *Result,
                   CORINFO_METHOD_HANDLE Caller);

  uint32_t getClassNumInstanceFields(CORINFO_CLASS_HANDLE Class);
  CORINFO_FIELD_HANDLE getFieldInClass(CORINFO_CLASS_HANDLE Class,
                                       uint32_t Ordinal);
  CorInfoType getFieldInfo(CORINFO_CLASS_HANDLE Class, uint32_t Ordinal,
                           uint32_t *FieldOffset,
                           CORINFO_CLASS_HANDLE *FieldClass);
  void getFieldInfo(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                    CORINFO_ACCESS_FLAGS AccessFlags,
                    CORINFO_FIELD_INFO *FieldInfo);
  CorInfoIsAccessAllowedResult
  canAccessClass(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                 CORINFO_METHOD_HANDLE Caller,
                 CORINFO_HELPER_DESC *ThrowHelper);

  // Properties of current method.
  bool isZeroInitLocals(void);
  uint32_t getCurrentMethodNumAutos(void);
  CORINFO_METHOD_HANDLE getCurrentMethodHandle(void);
  CORINFO_CLASS_HANDLE getCurrentMethodClass(void);
  CORINFO_CONTEXT_HANDLE getCurrentContext(void);
  uint32_t getCurrentMethodHash(void);
  uint32_t getCurrentMethodAttribs(void);
  const char *getCurrentMethodName(const char **ModuleName);
  void getCurrentMethodSigData(CorInfoCallConv *Conv, CorInfoType *ReturnType,
                               CORINFO_CLASS_HANDLE *ReturnClass,
                               int32_t *TotalILArgs, bool *IsVarArg,
                               bool *HasThis, uint8_t *RetSig);

  // Get entry point for function (used *only* by direct calls)
  void getFunctionEntryPoint(CORINFO_METHOD_HANDLE Function,
                             CORINFO_CONST_LOOKUP *Result,
                             CORINFO_ACCESS_FLAGS AccessFlags);

  // Get entry point for function (used by ldftn, ldvirtftn)
  void getFunctionFixedEntryPoint(CORINFO_METHOD_HANDLE Function,
                                  CORINFO_CONST_LOOKUP *Result);

  //
  // Module
  //
  CORINFO_MODULE_HANDLE getCurrentModuleHandle(void);

  //
  // Properties of current jitinfo.
  // These functions assume the context of the current module and method info.
  //

  // Finds name of MemberRef or MethodDef token
  void findNameOfToken(CORINFO_MODULE_HANDLE Scope, mdToken Token, char *Buffer,
                       size_t BufferSize);
  void findNameOfToken(mdToken Token, char *Buffer, size_t BufferSize);

  //
  // class
  //
public:
  CORINFO_CLASS_HANDLE getMethodClass(CORINFO_METHOD_HANDLE Handle);
  void getMethodVTableOffset(CORINFO_METHOD_HANDLE Handle,
                             uint32_t *OffsetOfIndirection,
                             uint32_t *OffsetAfterIndirection);
  const char *getClassName(CORINFO_CLASS_HANDLE Class);
  int32_t appendClassName(char16_t **Buffer, int32_t *BufferLen,
                          CORINFO_CLASS_HANDLE Class, bool IncludeNamespace,
                          bool FullInst, bool IncludeAssembly);
  GCLayoutStruct *getClassGCLayout(CORINFO_CLASS_HANDLE Class);
  uint32_t getClassAttribs(CORINFO_CLASS_HANDLE Class);
  uint32_t getClassSize(CORINFO_CLASS_HANDLE Class);
  CorInfoType getClassType(CORINFO_CLASS_HANDLE Class);
  void getClassType(CORINFO_CLASS_HANDLE Class, uint32_t Attribs,
                    CorInfoType *CorInfoType, uint32_t *Size);
  bool canInlineTypeCheckWithObjectVTable(CORINFO_CLASS_HANDLE Class);
  bool accessStaticFieldRequiresClassConstructor(CORINFO_FIELD_HANDLE);
  void classMustBeLoadedBeforeCodeIsRun(CORINFO_CLASS_HANDLE Handle);
  CorInfoInitClassResult initClass(CORINFO_FIELD_HANDLE Field,
                                   CORINFO_METHOD_HANDLE Method,
                                   CORINFO_CONTEXT_HANDLE Context,
                                   bool Speculative = false);

  // Class Alignment
private:
  uint32_t getClassAlignmentRequirement(CORINFO_CLASS_HANDLE);
  void *getMethodSync(bool *IsIndirect);

public:
  ReaderAlignType getMinimumClassAlignment(CORINFO_CLASS_HANDLE Class,
                                           ReaderAlignType Alignment);

  CorInfoHelpFunc getNewArrHelper(CORINFO_CLASS_HANDLE ElementType);

  void *getAddrOfCaptureThreadGlobal(bool *IsIndirect);

  //
  // field
  //
public:
  const char *getFieldName(CORINFO_FIELD_HANDLE, const char **ModuleName);
  CORINFO_CLASS_HANDLE getFieldClass(CORINFO_FIELD_HANDLE);
  CorInfoType getFieldType(CORINFO_FIELD_HANDLE, CORINFO_CLASS_HANDLE *Class,
                           CORINFO_CLASS_HANDLE Owner = NULL);
  CorInfoHelpFunc getSharedCCtorHelper(CORINFO_CLASS_HANDLE);
  CORINFO_CLASS_HANDLE getTypeForBox(CORINFO_CLASS_HANDLE Class);
  CorInfoHelpFunc getBoxHelper(CORINFO_CLASS_HANDLE);
  CorInfoHelpFunc getUnBoxHelper(CORINFO_CLASS_HANDLE);
  uint32_t getFieldOffset(CORINFO_FIELD_HANDLE);

  void *getStaticFieldAddress(CORINFO_FIELD_HANDLE Field, bool *IsIndirect);

  //
  // method
  //
  const char *getMethodName(CORINFO_METHOD_HANDLE, const char **ModuleName);
  uint32_t getMethodAttribs(CORINFO_METHOD_HANDLE Handle);
  void setMethodAttribs(CORINFO_METHOD_HANDLE Handle,
                        CorInfoMethodRuntimeFlags Flag);

  bool checkMethodModifier(CORINFO_METHOD_HANDLE Method, LPCSTR Modifier,
                           bool IsOptional);
  mdToken getMethodDefFromMethod(CORINFO_METHOD_HANDLE Handle);
  void getMethodSig(CORINFO_METHOD_HANDLE Handle, CORINFO_SIG_INFO *Sig);
  const char *getMethodRefInfo(CORINFO_METHOD_HANDLE Handle,
                               CorInfoCallConv *Conv, CorInfoType *CorType,
                               CORINFO_CLASS_HANDLE *RetTypeClass,
                               const char **ModuleName);
  void getMethodSigData(CorInfoCallConv *Conv, CorInfoType *ReturnType,
                        CORINFO_CLASS_HANDLE *ReturnClass,
                        uint32_t *TotalILArgs, bool *IsVarArg, bool *HasThis,
                        uint8_t *RetSig);
  void getMethodInfo(CORINFO_METHOD_HANDLE Handle, CORINFO_METHOD_INFO *Info);
  void methodMustBeLoadedBeforeCodeIsRun(CORINFO_METHOD_HANDLE Handle);

  bool isPrimitiveType(CORINFO_CLASS_HANDLE Handle);
  static bool isPrimitiveType(CorInfoType CorType);

  void handleClassAccess(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode **NewIR);
  void handleMemberAccess(CorInfoIsAccessAllowedResult AccessAllowed,
                          const CORINFO_HELPER_DESC &AccessHelper,
                          IRNode **NewIR);
  void handleMemberAccessWorker(CorInfoIsAccessAllowedResult AccessAllowed,
                                const CORINFO_HELPER_DESC &AccessHelper,
                                IRNode **NewIR);
  void
  handleMemberAccessForVerification(CorInfoIsAccessAllowedResult AccessAllowed,
                                    const CORINFO_HELPER_DESC &AccessHelper,
#ifdef CC_PEVERIFY
                                    HRESULT HResult
#else
                                    const char *HResult
#endif // CC_PEVERIFY
                                    );
  void insertHelperCall(const CORINFO_HELPER_DESC &HelperCallDesc,
                        IRNode **NewIR);
  bool canTailCall(CORINFO_METHOD_HANDLE DeclaredTarget,
                   CORINFO_METHOD_HANDLE ExactTarget, bool IsTailPrefix);
  CorInfoInline canInline(CORINFO_METHOD_HANDLE Caller,
                          CORINFO_METHOD_HANDLE Target, uint32_t *Restrictions);
  CORINFO_ARG_LIST_HANDLE getArgNext(CORINFO_ARG_LIST_HANDLE Args);
  CorInfoTypeWithMod getArgType(CORINFO_SIG_INFO *Sig,
                                CORINFO_ARG_LIST_HANDLE Args,
                                CORINFO_CLASS_HANDLE *TypeRet);
  CORINFO_CLASS_HANDLE getArgClass(CORINFO_SIG_INFO *Sig,
                                   CORINFO_ARG_LIST_HANDLE Args);
  CORINFO_CLASS_HANDLE getBuiltinClass(CorInfoClassId ClassId);
  CorInfoType getChildType(CORINFO_CLASS_HANDLE Class,
                           CORINFO_CLASS_HANDLE *ClassRet);
  bool isSDArray(CORINFO_CLASS_HANDLE Class);
  uint32_t getArrayRank(CORINFO_CLASS_HANDLE Class);

  void *getHelperDescr(CorInfoHelpFunc HelpFuncId, bool *IsIndirect);
  CorInfoHelpFunc getNewHelper(CORINFO_RESOLVED_TOKEN *ResolvedToken);

  void *getVarArgsHandle(CORINFO_SIG_INFO *Sig, bool *IsIndirect);
  bool canGetVarArgsHandle(CORINFO_SIG_INFO *Sig);

  void *getJustMyCodeHandle(CORINFO_METHOD_HANDLE Handle, bool *IsIndirect);

  void *getCookieForPInvokeCalliSig(CORINFO_SIG_INFO *SigTarget,
                                    bool *IsIndirect);
  bool canGetCookieForPInvokeCalliSig(CORINFO_SIG_INFO *SigTarget);
  void *getAddressOfPInvokeFixup(CORINFO_METHOD_HANDLE Method,
                                 InfoAccessType *AccessType);
  void *getPInvokeUnmanagedTarget(CORINFO_METHOD_HANDLE Method);

  bool pInvokeMarshalingRequired(CORINFO_METHOD_HANDLE Method,
                                 CORINFO_SIG_INFO *Sig);

  // Get a node that can be passed to the sync method helpers.
  IRNode *rdrGetCritSect(IRNode **NewIR);

  //
  // Support for filtering runtime-thrown exceptions
  //
  static int runtimeFilter(struct _EXCEPTION_POINTERS *ExceptionPointersPtr,
                           void *Param);
  void runtimeHandleException(struct _EXCEPTION_POINTERS *ExceptionPointersPtr);

  // ////////////////////////////////////////////////////////////////////
  //             IL generation methods supplied by the clients
  //
  // All pure virtual routines must be implemented by the client, non-pure
  // virtual routines have a default implementation in the reader, but can
  // be overloaded if necessary.
  // /////////////////////////////////////////////////////////////////////

  // MSIL Routines - client defined routines that are invoked by the reader.
  //                 One will be called for each msil opcode.

  virtual uint32_t getPointerByteSize() = 0;

  virtual void opcodeDebugPrint(uint8_t *Buffer, uint32_t StartOffset,
                                uint32_t EndOffset) = 0;

  // Used for testing, client can force verification.
  virtual bool verForceVerification(void) = 0;

  virtual bool abs(IRNode *Arg1, IRNode **RetVal, IRNode **NewIR) = 0;

  virtual IRNode *argList(IRNode **NewIR) = 0;
  virtual IRNode *instParam(IRNode **NewIR) = 0;
  virtual IRNode *secretParam(IRNode **NewIR) = 0;
  virtual IRNode *thisObj(IRNode **NewIR) = 0;
  virtual void boolBranch(ReaderBaseNS::BoolBranchOpcode Opcode, IRNode *Arg1,
                          IRNode **NewIR) = 0;
  virtual IRNode *box(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                      IRNode **NewIR, uint32_t *NextOffset = NULL,
                      VerificationState *VState = NULL);
  virtual IRNode *binaryOp(ReaderBaseNS::BinaryOpcode Opcode, IRNode *Arg1,
                           IRNode *Arg2, IRNode **NewIR) = 0;
  virtual void branch(IRNode **NewIR) = 0;
  virtual void breakOpcode(IRNode **NewIR);
  virtual IRNode *call(ReaderBaseNS::CallOpcode Opcode, mdToken Token,
                       mdToken ConstraintTypeRef, mdToken LoadFtnToken,
                       bool IsReadOnlyPrefix, bool IsTailCallPrefix,
                       bool IsUnmarkedTailCall, uint32_t CurrentOffset,
                       bool *IsRecursiveTailCall, IRNode **NewIR) = 0;
  virtual IRNode *castClass(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode *ObjRefNode, IRNode **NewIR);
  virtual IRNode *isInst(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                         IRNode *ObjRefNode, IRNode **NewIR);
  virtual IRNode *castOp(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                         IRNode *ObjRefNode, IRNode **NewIR,
                         CorInfoHelpFunc HelperId) = 0;

  virtual IRNode *ckFinite(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual IRNode *cmp(ReaderBaseNS::CmpOpcode Opcode, IRNode *Arg1,
                      IRNode *Arg2, IRNode **NewIR) = 0;
  virtual void condBranch(ReaderBaseNS::CondBranchOpcode Opcode, IRNode *Arg1,
                          IRNode *Arg2, IRNode **NewIR) = 0;
  virtual IRNode *conv(ReaderBaseNS::ConvOpcode Opcode, IRNode *Arg1,
                       IRNode **NewIR) = 0;
  virtual void cpBlk(IRNode *ByteCount, IRNode *SourceAddress,
                     IRNode *DestinationAddress, ReaderAlignType Alignment,
                     bool IsVolatile, IRNode **NewIR);
  virtual void cpObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                     IRNode *Arg2, ReaderAlignType Alignment, bool IsVolatile,
                     IRNode **NewIR);
  virtual void dup(IRNode *Opr, IRNode **Result1, IRNode **Result2,
                   IRNode **NewIR) = 0;
  virtual void endFilter(IRNode *Arg1, IRNode **NewIR) = 0;

  virtual FlowGraphNode *fgNodeGetNext(FlowGraphNode *FgNode) = 0;
  virtual uint32_t fgNodeGetStartMSILOffset(FlowGraphNode *Fg) = 0;
  virtual void fgNodeSetStartMSILOffset(FlowGraphNode *Fg, uint32_t Offset) = 0;
  virtual uint32_t fgNodeGetEndMSILOffset(FlowGraphNode *Fg) = 0;
  virtual void fgNodeSetEndMSILOffset(FlowGraphNode *FgNode,
                                      uint32_t Offset) = 0;

  virtual bool fgNodeIsVisited(FlowGraphNode *FgNode) = 0;
  virtual void fgNodeSetVisited(FlowGraphNode *FgNode, bool IsVisited) = 0;
  virtual void fgNodeSetOperandStack(FlowGraphNode *Fg, ReaderStack *Stack) = 0;
  virtual ReaderStack *fgNodeGetOperandStack(FlowGraphNode *Fg) = 0;

  virtual IRNode *getStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                        IRNode **NewIR) = 0;
  virtual void initBlk(IRNode *NumBytes, IRNode *ValuePerByte,
                       IRNode *DestinationAddress, ReaderAlignType Alignment,
                       bool IsVolatile, IRNode **NewIR);
  virtual void initObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg2,
                       IRNode **NewIR);
  virtual void insertThrow(CorInfoHelpFunc ThrowHelper, uint32_t Offset,
                           IRNode **NewIR);
  virtual void jmp(ReaderBaseNS::CallOpcode Opcode, mdToken Token, bool HasThis,
                   bool HasVarArg, IRNode **NewIR) = 0;

  virtual void leave(uint32_t TargetOffset, bool IsNonLocal,
                     bool EndsWithNonLocalGoto, IRNode **NewIR) = 0;
  virtual IRNode *loadArg(uint32_t ArgOrdinal, bool IsJmp, IRNode **NewIR) = 0;
  virtual IRNode *loadLocal(uint32_t ArgOrdinal, IRNode **NewIR) = 0;
  virtual IRNode *loadArgAddress(uint32_t ArgOrdinal, IRNode **NewIR) = 0;
  virtual IRNode *loadLocalAddress(uint32_t LocOrdinal, IRNode **NewIR) = 0;
  virtual IRNode *loadConstantI4(int32_t Constant, IRNode **NewIR) = 0;
  virtual IRNode *loadConstantI8(int64_t Constant, IRNode **NewIR) = 0;
  virtual IRNode *loadConstantI(size_t Constant, IRNode **NewIR) = 0;
  virtual IRNode *loadConstantR4(float Value, IRNode **NewIR) = 0;
  virtual IRNode *loadConstantR8(double Value, IRNode **NewIR) = 0;
  virtual IRNode *loadElem(ReaderBaseNS::LdElemOpcode Opcode,
                           CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                           IRNode *Arg2, IRNode **NewIR) = 0;
  virtual IRNode *loadElemA(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                            IRNode *Arg2, bool IsReadOnly, IRNode **NewIR) = 0;
  virtual IRNode *loadField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                            ReaderAlignType Alignment, bool IsVolatile,
                            IRNode **NewIR) = 0;
  virtual IRNode *loadIndir(ReaderBaseNS::LdIndirOpcode Opcode, IRNode *Address,
                            ReaderAlignType Alignement, bool IsVolatile,
                            bool IsInterfReadOnly, IRNode **NewIR);
  virtual IRNode *loadNull(IRNode **NewIR) = 0;
  virtual IRNode *localAlloc(IRNode *Arg, bool IsZeroInit, IRNode **NewIR) = 0;
  virtual IRNode *loadFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                   IRNode *Obj, IRNode **NewIR) = 0;
  virtual IRNode *loadLen(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual bool arrayAddress(CORINFO_SIG_INFO *Aig, IRNode **RetVal,
                            IRNode **NewIR) = 0;
  virtual IRNode *loadStringLen(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual IRNode *getTypeFromHandle(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual IRNode *getValueFromRuntimeHandle(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual IRNode *arrayGetDimLength(IRNode *Arg1, IRNode *Arg2,
                                    CORINFO_CALL_INFO *CallInfo,
                                    IRNode **NewIR) = 0;
  virtual IRNode *loadObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                          ReaderAlignType AlignmentPrefix, bool IsVolatile,
                          bool IsField, IRNode **NewIR);
  virtual IRNode *loadAndBox(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             IRNode *Address, ReaderAlignType AlignmentPrefix,
                             IRNode **NewIR) = 0;
  virtual IRNode *convertHandle(IRNode *GetTokenNumeric,
                                CorInfoHelpFunc HelperID,
                                CORINFO_CLASS_HANDLE Class, IRNode **NewIR) = 0;
  virtual void
  convertTypeHandleLookupHelperToIntrinsic(bool CanCompareToGetType,
                                           IRNode *IR) = 0;

  virtual IRNode *loadStaticField(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                  bool IsVolatile, IRNode **NewIR) = 0;
  virtual IRNode *loadStr(mdToken Token, IRNode **NewIR) = 0;
  virtual IRNode *loadToken(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode **NewIR);
  virtual IRNode *loadVirtFunc(IRNode *Arg1,
                               CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               CORINFO_CALL_INFO *CallInfo, IRNode **NewIR) = 0;
  virtual IRNode *loadPrimitiveType(IRNode *Address, CorInfoType CorType,
                                    ReaderAlignType Slignment, bool IsVolatile,
                                    bool IsInterfConst, IRNode **NewIR) = 0;
  virtual IRNode *loadNonPrimitiveObj(IRNode *Address,
                                      CORINFO_CLASS_HANDLE Class,
                                      ReaderAlignType Alignment,
                                      bool IsVolatile, IRNode **NewIR) = 0;
  virtual IRNode *makeRefAny(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             IRNode *Object, IRNode **NewIR) = 0;
  virtual IRNode *newArr(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                         IRNode **NewIR) = 0;
  virtual IRNode *newObj(mdToken Token, mdToken LoadFtnToken,
                         uint32_t CurrentOffset, IRNode **NewIR) = 0;
  virtual void pop(IRNode *Opr, IRNode **NewIR) = 0;
  virtual IRNode *refAnyType(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual IRNode *refAnyVal(IRNode *Val, CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode **NewIR);
  virtual void rethrow(IRNode **NewIR) = 0;
  virtual void returnOpcode(IRNode *Opr, bool IsSynchronousMethod,
                            IRNode **NewIR) = 0;
  virtual IRNode *shift(ReaderBaseNS::ShiftOpcode Opcode, IRNode *ShiftAmount,
                        IRNode *ShiftOperand, IRNode **NewIR) = 0;
  virtual IRNode *sizeofOpcode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               IRNode **NewIR) = 0;
  virtual void storeArg(uint32_t LocOrdinal, IRNode *Arg1,
                        ReaderAlignType Alignment, bool IsVolatile,
                        IRNode **NewIR) = 0;
  virtual void storeElem(ReaderBaseNS::StElemOpcode Opcode,
                         CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                         IRNode *Arg2, IRNode *Arg3, IRNode **NewIR) = 0;
  virtual void storeElemRefAny(IRNode *Value, IRNode *Index, IRNode *Obj,
                               IRNode **NewIR);
  virtual void storeField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                          IRNode *Arg2, ReaderAlignType Alignment,
                          bool IsVolatile, IRNode **NewIR) = 0;
  virtual void storeIndir(ReaderBaseNS::StIndirOpcode Opcode, IRNode *Arg1,
                          IRNode *Arg2, ReaderAlignType Alignment,
                          bool IsVolatile, IRNode **NewIR);
  virtual void storePrimitiveType(IRNode *Value, IRNode *Address,
                                  CorInfoType CorType,
                                  ReaderAlignType Alignment, bool IsVolatile,
                                  IRNode **NewIR) = 0;
  virtual void storeLocal(uint32_t LocOrdinal, IRNode *Arg1,
                          ReaderAlignType Alignment, bool IsVolatile,
                          IRNode **NewIR) = 0;
  virtual void storeObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                        IRNode *Arg2, ReaderAlignType Alignment,
                        bool IsVolatile, bool IsField, IRNode **NewIR);
  virtual void storeStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                                IRNode *ValueToStore, bool IsVolatile,
                                IRNode **NewIR) = 0;
  virtual IRNode *stringGetChar(IRNode *Arg1, IRNode *Arg2, IRNode **NewIR) = 0;
  virtual bool sqrt(IRNode *Arg1, IRNode **RetVal, IRNode **NewIR) = 0;

  virtual bool interlockedIntrinsicBinOp(IRNode *Arg1, IRNode *Arg2,
                                         IRNode **RetVal,
                                         CorInfoIntrinsics IntrinsicID,
                                         IRNode **NewIR) = 0;
  virtual bool interlockedCmpXchg(IRNode *Arg1, IRNode *Arg2, IRNode *Arg3,
                                  IRNode **RetVal,
                                  CorInfoIntrinsics IntrinsicID,
                                  IRNode **NewIR) = 0;
  virtual bool memoryBarrier(IRNode **NewIR) = 0;
  virtual void switchOpcode(IRNode *Opr, IRNode **NewIR) = 0;
  virtual void throwOpcode(IRNode *Arg1, IRNode **NewIR) = 0;
  virtual IRNode *unaryOp(ReaderBaseNS::UnaryOpcode Opcode, IRNode *Arg1,
                          IRNode **NewIR) = 0;
  virtual IRNode *unbox(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg2,
                        IRNode **NewIR, bool AndLoad, ReaderAlignType Alignment,
                        bool IsVolatile) = 0;

  virtual IRNode *unboxAny(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                           ReaderAlignType Alignment, bool IsVolatilePrefix,
                           IRNode **NewIR);
  virtual void nop(IRNode **NewIR) = 0;

  virtual void insertIBCAnnotations() = 0;
  virtual IRNode *insertIBCAnnotation(FlowGraphNode *Node, uint32_t Count,
                                      uint32_t Offset) = 0;

  // Insert class constructor
  virtual void insertClassConstructor(IRNode **NewIR);

  //
  // REQUIRED Client Helper Routines.
  //

  // Base calls to alert client it needs a security check
  virtual void methodNeedsSecurityCheck() = 0;

  // Base calls to alert client it needs keep generics context alive
  virtual void
  methodNeedsToKeepAliveGenericsContext(bool KeepGenericsCtxtAlive) = 0;

  // Called to instantiate an empty reader stack.
  virtual ReaderStack *createStack(uint32_t MaxStack, ReaderBase *Reader) = 0;

  // Called when reader begins processing method.
  virtual void readerPrePass(uint8_t *Buffer, uint32_t NumBytes) = 0;

  // Called between building the flow graph and inserting the IR
  virtual void readerMiddlePass(void) = 0;

  // Called when reader has finished processing method.
  virtual void readerPostPass(bool IsImportOnly) = 0;

  // Called at the start of block processing
  virtual void beginFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrentOffset,
                                  bool IsVerifyOnly) = 0;
  // Called at the end of block processing.
  virtual void endFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrentOffset,
                                IRNode **NewIR) = 0;

  // Used to maintain operand stack.
  virtual void maintainOperandStack(IRNode **Opr1, IRNode **Opr2,
                                    FlowGraphNode *CurrentBlock,
                                    IRNode **NewIR) = 0;
  virtual void assignToSuccessorStackNode(FlowGraphNode *, IRNode *Destination,
                                          IRNode *Source, IRNode **NewIR,
                                          bool *) = 0;
  //    virtual ReaderStackNode* copyStackList(ReaderStackNode* stack) =
  //    0;
  virtual bool typesCompatible(IRNode *Src1, IRNode *Src2) = 0;

  virtual void removeStackInterference(IRNode **NewIR) = 0;

  virtual void removeStackInterferenceForLocalStore(uint32_t Opcode,
                                                    uint32_t Ordinal,
                                                    IRNode **NewIR) = 0;

  // Remove all IRNodes from block (for verification error processing.)
  virtual void clearCurrentBlock(IRNode **NewIR) = 0;

  // Called when an assert occurs (debug only)
  static void debugError(const char *Filename, uint32_t LineNumber,
                         const char *Message);

  // Notify client of alignment problem
  virtual void verifyStaticAlignment(void *Pointer, CorInfoType CorType,
                                     uint32_t MinClassAlign) = 0;

  // non-debug fatal error (verification badcode, jit can't continue, etc...)
  static void fatal(int Errnum);

  // Query the runtime/compiler about code-generation information
  virtual bool generateDebugCode() { return false; }
  virtual bool generateDebugInfo() { return false; }
  virtual bool generateDebugEnC() { return false; }

  // Allocate temporary (Reader lifetime) memory
  virtual void *getTempMemory(size_t Bytes) = 0;

  // Allocate procedure-lifetime memory
  virtual void *getProcMemory(size_t Bytes) = 0;

  virtual EHRegion *rgnAllocateRegion() = 0;
  virtual EHRegionList *rgnAllocateRegionList() = 0;

  //
  // REQUIRED Flow and Region Graph Manipulation Routines
  //
  virtual FlowGraphNode *fgPrePhase(FlowGraphNode *Fg) = 0;
  virtual void fgPostPhase(void) = 0;
  virtual FlowGraphNode *fgGetHeadBlock(void) = 0;
  virtual FlowGraphNode *fgGetTailBlock(void) = 0;
  virtual FlowGraphNode *fgNodeGetIDom(FlowGraphNode *Fg) = 0;

  virtual IRNode *fgNodeFindStartLabel(FlowGraphNode *Block) = 0;

  virtual BranchList *fgGetLabelBranchList(IRNode *LabelNode) = 0;

  virtual void insertHandlerAnnotation(EHRegion *HandlerRegion) = 0;
  virtual void insertRegionAnnotation(IRNode *RegionStartNode,
                                      IRNode *RegionEndNode) = 0;
  virtual void fgAddLabelToBranchList(IRNode *LabelNode,
                                      IRNode *BranchNode) = 0;
  virtual void fgAddArc(IRNode *BranchNode, FlowGraphNode *Source,
                        FlowGraphNode *Sink) = 0;
  virtual bool fgBlockHasFallThrough(FlowGraphNode *Block) = 0;
  virtual void fgDeleteBlock(FlowGraphNode *Block) = 0;
  virtual void fgDeleteEdge(FlowGraphEdgeList *Arc) = 0;
  virtual void fgDeleteNodesFromBlock(FlowGraphNode *Block) = 0;

  // Returns true iff client considers the JMP recursive and wants a
  // loop back-edge rather than a forward edge to the exit label.
  virtual bool fgOptRecurse(mdToken Token) = 0;

  // Returns true iff client considers the CALL/JMP recursive and wants a
  // loop back-edge rather than a forward edge to the exit label.
  virtual bool fgOptRecurse(ReaderCallTargetData *CallTargetData) = 0;

  // Returns true if node (the start of a new eh region) cannot be the start of
  // a block.
  virtual bool fgEHRegionStartRequiresBlockSplit(IRNode *Node) = 0;

  virtual bool fgIsExceptRegionStartNode(IRNode *Node) = 0;
  virtual FlowGraphNode *fgSplitBlock(FlowGraphNode *Block, IRNode *Node) = 0;
  virtual void fgSetBlockToRegion(FlowGraphNode *Block, EHRegion *Region,
                                  uint32_t LastOffset) = 0;
  virtual IRNode *fgMakeBranch(IRNode *LabelNode, IRNode *BlockNode,
                               uint32_t CurrentOffset, bool IsConditional,
                               bool IsNominal) = 0;
  virtual IRNode *fgMakeEndFinally(IRNode *BlockNode, uint32_t CurrentOffset,
                                   bool IsLexicalEnd) = 0;

  // turns an unconditional branch to the entry label into a fall-through
  // or a branch to the exit label, depending on whether it was a recursive
  // jmp or tail.call.
  virtual void fgRevertRecursiveBranch(IRNode *BranchNode) = 0;

  virtual IRNode *fgMakeSwitch(IRNode *DefaultLabel, IRNode *Node) = 0;
  virtual IRNode *fgMakeThrow(IRNode *Node) = 0;
  virtual IRNode *fgAddCaseToCaseList(IRNode *SwitchNode, IRNode *LabelNode,
                                      uint32_t Element) = 0;
  virtual void insertEHAnnotationNode(IRNode *InsertionPointNode,
                                      IRNode *Node) = 0;
  virtual FlowGraphNode *makeFlowGraphNode(uint32_t TargetOffset,
                                           EHRegion *Region) = 0;
  virtual void markAsEHLabel(IRNode *LabelNode) = 0;
  virtual IRNode *makeTryEndNode(void) = 0;
  virtual IRNode *makeRegionStartNode(ReaderBaseNS::RegionKind RegionType) = 0;
  virtual IRNode *makeRegionEndNode(ReaderBaseNS::RegionKind RegionType) = 0;
  virtual void fgCleanupTryEnd(EHRegion *Region){};

  // Hook to permit client to record call information returns true if the call
  // is a recursive tail
  // call and thus should be turned into a loop
  virtual bool fgCall(ReaderBaseNS::OPCODE Opcode, mdToken Token,
                      mdToken ConstraintToken, uint32_t ILOffset, IRNode *Block,
                      bool CanInline, bool IsTailCall, bool IsUnmarkedTailCall,
                      bool IsReadOnly) = 0;

  // Replace all uses of oldNode in the IR with newNode and delete oldNode.
  virtual void replaceFlowGraphNodeUses(FlowGraphNode *OldNode,
                                        FlowGraphNode *NewNode) = 0;
  virtual IRNode *findBlockSplitPointAfterNode(IRNode *Node) = 0;
  virtual IRNode *exitLabel(void) = 0;
  virtual IRNode *entryLabel(void) = 0;

  // Function is passed a try region, and is expected to return the first label
  // or instruction
  // after the region.
  virtual IRNode *findTryRegionEndOfClauses(EHRegion *TryRegion) = 0;

  virtual bool isCall(IRNode **NewIR) = 0;
  virtual bool isRegionStartBlock(FlowGraphNode *Fg) = 0;
  virtual bool isRegionEndBlock(FlowGraphNode *Fg) = 0;

  // Create a symbol node that will be used to represent the stack-incoming
  // exception object
  // upon entry to funclets.
  virtual IRNode *makeExceptionObject(IRNode **NewIR) = 0;

  // //////////////////////////////////////////////////////////////////////////
  // Client Supplied Helper Routines, required by VOS support
  // //////////////////////////////////////////////////////////////////////////

  // Asks GenIR to make operand value accessible by address, and return a node
  // that references
  // the incoming operand by address.
  virtual IRNode *addressOfLeaf(IRNode *Leaf, IRNode **NewIR) = 0;
  virtual IRNode *addressOfValue(IRNode *Leaf, IRNode **NewIR) = 0;

  // Helper callback used by rdrCall to emit call code.
  virtual IRNode *genCall(ReaderCallTargetData *CallTargetDaTA,
                          CallArgTriple *Args, uint32_t NumArgs,
                          IRNode **CallNode, IRNode **NewIR) = 0;

  virtual bool canMakeDirectCall(ReaderCallTargetData *CallTargetData) = 0;

  // Generate call to helper
  virtual IRNode *callHelper(CorInfoHelpFunc HelperID, IRNode *Dst,
                             IRNode **NewIR, IRNode *Arg1 = NULL,
                             IRNode *Arg2 = NULL, IRNode *Arg3 = NULL,
                             IRNode *Arg4 = NULL,
                             ReaderAlignType Alignment = Reader_AlignUnknown,
                             bool IsVolatile = false, bool NoCtor = false,
                             bool CanMoveUp = false) = 0;

  // Generate special generics helper that might need to insert flow
  virtual IRNode *callRuntimeHandleHelper(CorInfoHelpFunc Helper, IRNode *Arg1,
                                          IRNode *Arg2, IRNode *NullCheckArg,
                                          IRNode **NewIR) = 0;

  virtual IRNode *convertToHelperArgumentType(IRNode *Opr,
                                              uint32_t DestinationSize,
                                              IRNode **NewIR) = 0;

  virtual IRNode *genNullCheck(IRNode *Node, IRNode **NewIR) = 0;

  virtual void
  createSym(uint32_t Num, bool IsAuto, CorInfoType CorType,
            CORINFO_CLASS_HANDLE Class, bool IsPinned,
            ReaderSpecialSymbolType Type = Reader_NotSpecialSymbol) = 0;

  virtual IRNode *derefAddress(IRNode *Address, bool DestIsGCPtr, bool IsConst,
                               IRNode **NewIR) = 0;

  virtual IRNode *conditionalDerefAddress(IRNode *Address, IRNode **NewIR) = 0;

  virtual IRNode *getHelperCallAddress(CorInfoHelpFunc HelperId,
                                       IRNode **NewIR) = 0;

  virtual IRNode *simpleFieldAddress(IRNode *BaseAddress,
                                     CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                     CORINFO_FIELD_INFO *FieldInfo,
                                     IRNode **NewIR) = 0;

  virtual IRNode *handleToIRNode(mdToken Token, void *EmbedHandle,
                                 void *RealHandle, bool IsIndirect,
                                 bool IsReadOnly, bool IsRelocatable,
                                 bool IsCallTarget, IRNode **NewIR,
                                 bool IsFrozenObject = false) = 0;

  // Create an operand that will be used to hold a pointer.
  virtual IRNode *makePtrDstGCOperand(bool IsInteriorGC) = 0;
  virtual IRNode *makePtrNode(ReaderPtrType PointerType = Reader_PtrNotGc) = 0;
  virtual IRNode *makeStackTypeNode(IRNode *Node) = 0;
  virtual IRNode *makeCallReturnNode(CORINFO_SIG_INFO *Sig,
                                     uint32_t *HiddenMBParamSize,
                                     GCLayoutStruct **GCLayout) = 0;

  virtual IRNode *makeDirectCallTargetNode(CORINFO_METHOD_HANDLE Method,
                                           void *CodeAddress) = 0;

  // Called once region tree has been built.
  virtual void setEHInfo(EHRegion *EhRegionTree,
                         EHRegionList *EhRegionList) = 0;

  // Line number info
  virtual void sequencePoint(int32_t Offset, ReaderBaseNS::OPCODE PrevOp,
                             IRNode **NewIR);
  virtual void setSequencePoint(uint32_t, ICorDebugInfo::SourceTypes,
                                IRNode **NewIR) = 0;
  virtual bool needSequencePoints() = 0;

  // Used to turn token into handle/IRNode
  virtual IRNode *
  genericTokenToNode(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode **NewIR,
                     bool EmbedParent = false, bool MustRestoreHandle = false,
                     CORINFO_GENERIC_HANDLE *StaticHandle = NULL,
                     bool *IsRuntimeLookup = NULL, bool NeedsResult = true);

  virtual IRNode *runtimeLookupToNode(CORINFO_RUNTIME_LOOKUP_KIND Kind,
                                      CORINFO_RUNTIME_LOOKUP *Lookup,
                                      IRNode **NewIR);

  // Used to expand multidimensional array access intrinsics
  virtual bool arrayGet(CORINFO_SIG_INFO *Sig, IRNode **RetVal,
                        IRNode **NewIR) = 0;
  virtual bool arraySet(CORINFO_SIG_INFO *Sig, IRNode **NewIR) = 0;

#if !defined(NDEBUG)
  virtual void dbDumpFunction(void) = 0;
  virtual void dbPrintIRNode(IRNode *NewIR) = 0;
  virtual void dbPrintFGNode(FlowGraphNode *Fg) = 0;
  virtual void dbPrintEHRegion(EHRegion *Eh) = 0;
  virtual uint32_t dbGetFuncHash(void) = 0;
#endif

  static bool rdrIsMethodVirtual(uint32_t MethodAttribs);

private:
  ///////////////////////////////////////////////////////////////////////
  // Last field in structure.
  char DummyLastBaseField;
  // Fields after this one will not be initialized in the constructor.
  ///////////////////////////////////////////////////////////////////////

  // Deferred NYI map for Leave instructions (temporary)
  std::map<uint32_t, const char *> NyiLeaveMap;
};

// this is a helper class for tracking what is loaded onto the stack so
// when we come across calls, we can tell whether we are passing "this"
// in as the first parameter
class LoadTracker {
#define CHECK_VALID(Cond)                                                      \
  if (!(this->IsValidProgram = (this->IsValidProgram && (Cond))))              \
    return;

private:
  enum LTNodeType { LT_Other, LT_This, LT_Constant, LT_Address, LT_Retval };

  struct LTNode {
    LTNodeType Type : 4;
    uint32_t CandidateILOffset : 28;
  };

  LTNode *Stack;
  uint32_t Top;
  uint32_t MaxStack;
  bool IsValidProgram;

public:
  void init(ReaderBase *Reader, uint32_t MaxStack) {
    this->Stack = (LTNode *)Reader->getProcMemory(sizeof(LTNode) * MaxStack);
    this->MaxStack = MaxStack;
    this->Top = 0;
    this->IsValidProgram = 1;
  }

  void reset() {
    this->Top = 0;
    this->IsValidProgram = 1;
  }

  void loadThis() {
    CHECK_VALID(this->Top < this->MaxStack);
    this->Stack[this->Top].Type = LT_This;
    this->Stack[this->Top++].CandidateILOffset = 0;
  }

  void loadOther(uint32_t Count) {
    CHECK_VALID(this->Top + Count <= this->MaxStack);
    for (uint32_t I = this->Top; I < (this->Top + Count); I++) {
      this->Stack[I].Type = LT_Other;
      this->Stack[I].CandidateILOffset = 0;
    }
    this->Top += Count;
  }

  void loadOther() { this->loadOther(1); }

  void loadConstant() {
    CHECK_VALID(this->Top < this->MaxStack);
    this->Stack[this->Top].Type = LT_Constant;
    this->Stack[this->Top++].CandidateILOffset = 0;
  }

  void loadAddress() {
    CHECK_VALID(this->Top < this->MaxStack);
    this->Stack[this->Top].Type = LT_Address;
    this->Stack[this->Top++].CandidateILOffset = 0;
  }

  void loadRetVal(uint32_t CandMSILOffset) {
    CHECK_VALID(this->Top < this->MaxStack);
    this->Stack[this->Top].Type = LT_Retval;
    this->Stack[this->Top++].CandidateILOffset = CandMSILOffset;
  }

  void unload(uint32_t N) {
    CHECK_VALID(true);
    if (this->Top <= N)
      this->Top = 0;
    else
      this->Top -= N;
  }

  void unload() { this->unload(1); }

  bool checkFirstParamIsThis(uint32_t NumArgs) {
    return this->IsValidProgram && (this->Top >= NumArgs) &&
           (this->Stack[this->Top - NumArgs].Type == LT_This);
  }

  uint32_t numConstantParams(uint32_t NumArgs) {
    uint32_t RetVal = 0;
    if (this->IsValidProgram) {
      // if by scanning we find any constant arguments return true
      // (this could be changed to a percentage?)
      for (uint32_t I = 0; I < NumArgs && I < this->Top; I++) {
        if (this->Stack[this->Top - I - 1].Type == LT_Constant)
          RetVal++;
      }
    }
    return RetVal;
  }

  uint32_t numAddressParams(uint32_t NumArgs) {
    uint32_t RetVal = 0;
    if (this->IsValidProgram) {
      // if by scanning we find any constant arguments return true
      // (this could be changed to a percentage?)
      for (uint32_t I = 0; I < NumArgs && I < this->Top; I++) {
        if (this->Stack[this->Top - I - 1].Type == LT_Address)
          RetVal++;
      }
    }
    return RetVal;
  }

  uint32_t offsetIsRetVal(uint32_t Offset) {
    return this->IsValidProgram && (this->Top >= Offset) &&
           (this->Stack[this->Top - Offset].Type == LT_Retval);
  }

  uint32_t offsetGetMSILOffset(uint32_t Offset) {
    return this->Stack[this->Top - Offset].CandidateILOffset;
  }

#if !defined(NODEBUG)
  void prState();
#endif

#undef CHECK_VALID
};

// --------------------------------------------------------------
// Functions for reading typed values from the bytecode.
// --------------------------------------------------------------

#ifdef BIGENDIAN
// For big endian machines, flip the bits around.
#define IL_LOADTYPE(ILCursor__, Type)                                          \
  {                                                                            \
    Type RetVal;                                                               \
    for (uint32_t I = 0; I < sizeof(Type); ++I)                                \
      ((uint8_t *)&RetVal)[I] =                                                \
          ((uint8_t *)(ILCursor__))[sizeof(Type) - I - 1];                     \
    return RetVal;                                                             \
  }
#else
#define IL_LOADTYPE(ILCursor__, Type)                                          \
  {                                                                            \
    Type RetVal = *(UNALIGNED Type *)ILCursor__;                               \
    return RetVal;                                                             \
  }
#endif

inline int8_t readInt8(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, int8_t); }

inline uint8_t readUInt8(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, uint8_t); }

inline int16_t readInt16(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, int16_t); }

inline uint16_t readUInt16(uint8_t *ILCursor) {
  IL_LOADTYPE(ILCursor, uint16_t);
}

inline int32_t readInt32(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, int32_t); }

inline uint32_t readUInt32(uint8_t *ILCursor) {
  IL_LOADTYPE(ILCursor, uint32_t);
}

inline int64_t readInt64(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, int64_t); }

inline mdToken readToken(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, mdToken); }

inline float readF32(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, float); }

inline double readF64(uint8_t *ILCursor) { IL_LOADTYPE(ILCursor, double); }

inline void *readPtr(uint8_t *ILCursor) {
  typedef void *PtrType;
  IL_LOADTYPE(ILCursor, PtrType);
}

class NotYetImplementedException {
private:
  const char *TheReason;

public:
  NotYetImplementedException(const char *Reason = "") : TheReason(Reason) {}
  const char *reason() { return TheReason; }
};

#endif // MSIL_READER_H
