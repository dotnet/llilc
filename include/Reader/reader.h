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
#include <vector>

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

#ifdef CC_PEVERIFY
extern HRESULT VerLastError;
#endif

// Forward declarations for client defined structures
class GenIR;  // Compiler dependent IR production
class IRNode; // Your compiler intermediate representation
class ReaderStack;
class FlowGraphNode;
class FlowGraphEdgeList;
class BranchList;
class ReaderBitVector;
struct EHRegion;
class VerifyWorkList;
class VerificationState;
class FlowGraphNodeOffsetList;

/// \brief Exception information for the Jit's exception filter
///
/// The jit may need to examine propagating exceptions via a filter to
/// determine if the jit needs to take any special actions. This struct
/// provides some extra context for the jit to consider when filtering.
struct RuntimeFilterParams {
  EXCEPTION_POINTERS ExceptionPointers; ///< Exception context information
  GenIR *This;                          ///< Additional data
};

/// \brief GC information for value classes
///
/// This structure describes which fields in a value class are gc pointers.
/// The jit needs to know this information so it can report gc pointer fields
/// for stack-allocated value classes in the GC info. However, we also encode
/// this information in LLVM type and our intention is to use those types to
/// drive the GC info reporting.
///
/// The value class is logically viewed as an array of pointer-sized elements
/// (note GC fields are guaranteed by the EE to be suitably aligned so that
/// this view is sensible). \p GCLayout[i] is nonzero if there is a gc pointer
/// at the corresponding offset.
///
/// By convention we only create these structures when the value class
/// actually has GC pointers, so \p NumGCPtrs should be nonzero.
struct GCLayout {
  uint32_t NumGCPointers; ///< Total number of gc pointers to report
  uint8_t GCPointers[0];  ///< Array indicating location of the gc pointers
};

/// \brief Structure used to pass argument information from rdrCall to GenCall.
///
/// ReaderBase (which implements \p rdrCall) doesn't know how the derived
/// Reader (which implements \p GenCall) will represent type information, so it
/// uses \p ArgType and \p ArgClass to describe the type of the argument, and
/// \p ArgNode to describe its value.
struct CallArgTriple {
  IRNode *ArgNode;               ///< Opaque pointer to IR for argument value
  CorInfoType ArgType;           ///< Low-level type of the argument
  CORINFO_CLASS_HANDLE ArgClass; ///< Extra type info for pointers and similar
};

/// Structure representing a linked list of flow graph nodes
struct FlowGraphNodeList {
  FlowGraphNode *Block;    ///< Head node in the list
  FlowGraphNodeList *Next; ///< Pointer to next list cell
};

/// Structure representing a linked list of flow graph nodes and
/// for each node, a related node.
struct FlowGraphNodeWorkList {
  FlowGraphNode *Block;        ///< Head node in the list
  FlowGraphNodeWorkList *Next; ///< Pointer to next list cell
  FlowGraphNode *Parent;       ///< Related node
};

/// \brief Enum describing pointer alignment.
enum ReaderAlignType {
  Reader_AlignNatural = (uint8_t)~0, ///< Default natural alignment
  Reader_AlignUnknown = 0,           ///< Unknown alignment
  Reader_Align1 = 1,                 ///< Byte alignment
  Reader_Align2 = 2,                 ///< Word alignment
  Reader_Align4 = 4,                 ///< DWord alignment
  Reader_Align8 = 8                  ///< QWord alignment
};

/// \brief Special symbol types
///
/// These are used to describe locals or parameters that have special
/// meaning during code generation.
enum ReaderSpecialSymbolType {
  Reader_NotSpecialSymbol = 0, ///< Nothing special
  Reader_ThisPtr,              ///< Current this pointer for method
  Reader_UnmodifiedThisPtr,    ///< This pointer param passed to method
  Reader_VarArgsToken,         ///< Special param for varargs support
  Reader_InstParam,            ///< Special param for shared generics
  Reader_SecurityObject,       ///< Local used for security checking
  Reader_GenericsContext       ///< Local holding shared generics context
};

/// \brief Types of pointers
///
/// The reader sometimes needs to create new temporaries or locals to hold
/// particular types of pointers. This enum is used to describe the kind of
/// pointer desired.
enum ReaderPtrType {
  Reader_PtrNotGc = 0,
  Reader_PtrGcBase,
  Reader_PtrGcInterior
};

/// \brief Exception types for verification
///
/// When reading MSIL, different kinds of exceptions can be thrown, and this
/// enum decribes the possibilities.
enum ReaderExceptionType {
  Reader_LocalVerificationException,  ///< Verifier local check failed
  Reader_GlobalVerificationException, ///< Verifier global check failed
};

/// Common base class for reader exceptions
class ReaderException {
public:
  ReaderExceptionType Type; ///< Type of the exception
};

// The TryRegion graph allows us to build a region tree that captures the
// lexical information from the EIT before we begin reading the MSIL opcodes.
// Thus we build the try-region tree, then the flow graph, then fill in the
// the flow graph nodes with IL.

struct EHRegion;
struct EHRegionList;
struct FgData;
class ReaderBase; // Forward declaration

#pragma region Reader Operand Stack

/// \brief A stack of IRNode pointers representing the MSIL operand stack.
///
/// The MSIL instruction set operates on a stack machine. Instructions
/// with operands may take them from the stack (if not some kind of
/// immediate) and the results of instruction are pushed on the operand stack.
/// The MSIL operands are translated by the reader into IRNodes.
/// The operand stack is represented by a stack of pointers to the
/// IRNodes for the operands.
///
/// ReaderStack is an abstract class but we do specify that its state is
/// expressed using an std::vector<IRNode*>. So some of the simple
/// operations are implemented in this class. The remaining methods are
/// left to be implemented by a derived class.

class ReaderStack {
protected:
  std::vector<IRNode *> Stack;

public:
  /// \brief Mutable iterator to elements of the stack from bottom to top.
  ///
  /// This is the same as the underlying vector iterator.
  typedef std::vector<IRNode *>::iterator iterator;

  /// \brief Pop the top element off the operand stack.
  ///
  /// \return The top element of the stack.
  /// \pre The stack is not empty
  /// \post The top element of the stack has been removed.
  virtual IRNode *pop() = 0;

  /// \brief Push \p NewVal onto the operand stack.
  ///
  /// \param NewVal The value to be pushed.
  /// \pre NewVal != nullptr
  virtual void push(IRNode *NewVal) = 0;

  /// \brief Make the operand stack empty.
  ///
  /// \post The stack is empty.
  void clearStack() { Stack.clear(); }

  /// \brief Test whether the stack is empty.
  ///
  /// \return True if the stack is empty
  bool empty() { return Stack.empty(); }

  /// \brief If the stack is not empty, cause an assertion failure.
  virtual void assertEmpty() = 0;

  /// \brief get the number of operands on the operand stack.
  ///
  /// \return The number of elements in the stack.
  uint32_t size() { return Stack.size(); }

  /// \brief Return begin iterator for iterating from bottom to top of stack.
  iterator begin() { return Stack.begin(); }

  /// \brief Return begin iterator for iterating from bottom to top of stack.
  iterator end() { return Stack.end(); }

#if defined(_DEBUG)
  /// \brief Print the contents of the operand stack onto the debug output.
  virtual void print() = 0;
#endif

  /// \brief Returns a copy of this operand stack.
  virtual ReaderStack *copy() = 0;
};

#pragma endregion

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
    return IsCallInfoValid ? &CallInfo : nullptr;
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

  IRNode *getMethodHandleNode();
  IRNode *getClassHandleNode();
  IRNode *getTypeContextNode();

  IRNode *applyThisTransform(IRNode *ThisIR);

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
  bool isOptimizedDelegateCtor() { return IsOptimizedDelegateCtor; }
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

  // Return nullptr if we either can't know the call target statically, and if
  // we have a method handle it is only representative.  Otherwise it returns
  // the same as getMethodHandle.  Think of a virtual call, where we have the
  // baseclass or interface method handle, *NOT* the actual target.
  CORINFO_METHOD_HANDLE getKnownMethodHandle() {
    if (IsCallI || !IsCallInfoValid || (CallInfo.kind != CORINFO_CALL))
      return nullptr;
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

/// \name Client IR interface
///
///@{

/// \brief Iterate forward through the client IR
///
/// Within a basic block, the client IR nodes will form a linear sequence.
/// This method advances from one node to the next in that sequence.
///
/// \param Node    The current IR node
/// \returns       The next IR node
IRNode *irNodeGetNext(IRNode *Node);

/// \brief Searching forward, find the first IR node with an MSIL offset
/// greater or equal to the indicated offset.
///
/// Ask the client to scan forward through the IR starting \p Node, searching
/// for a Node whose MSIL offset is greater than or equal to the provided
/// \p Offset.
///
/// \param Node        The starting node for the search
/// \param Offset      The MSIL offset
/// \returns           The first IR node that is at/after the indicated offset
IRNode *irNodeGetInsertPointAfterMSILOffset(IRNode *Node, uint32_t Offset);

/// \brief Searching backwards, find the first IR node with an MSIL offset less
/// than the indicated offset.
///
/// Ask the client to scan backwards through the IR starting \p Node, searching
/// for a Node whose MSIL offset is less than the provided \p Offset.
///
/// \param Node        The starting node for the search
/// \param Offset      The MSIL offset
/// \returns           The first IR node that is before the indicated offset
IRNode *irNodeGetInsertPointBeforeMSILOffset(IRNode *Node, uint32_t Offset);

/// Get the first IR node in the indicated IR node's block
///
/// \param HandlerStartNode   The indicated IR node
/// \returns                  The first IR node in the same block
IRNode *
irNodeGetFirstLabelOrInstrNodeInEnclosingBlock(IRNode *HandlerStartNode);

/// Get the MSIL offset for the indicated IR node
///
/// \param Node    The indicated IR node
/// \returns       MSIL offset for this IR node
uint32_t irNodeGetMSILOffset(IRNode *Node);

/// Set the MSIL offset for this IR node
///
/// \param Node      The node in question
/// \param Offset    The MSIL offset to use
void irNodeLabelSetMSILOffset(IRNode *Node, uint32_t Offset);

/// Set the MSIL offset for this branch IR node
///
/// \param BranchNode      The node in question
/// \param Offset          The MSIL offset to use
void irNodeBranchSetMSILOffset(IRNode *BranchNode, uint32_t Offset);

/// Set the MSIL offset for this exception branch IR node.
///
/// \param BranchNode      The node in question
/// \param Offset          The MSIL offset to use
void irNodeExceptSetMSILOffset(IRNode *BranchNode, uint32_t Offset);

/// Insert an IR node before another IR node
///
/// \param InsertionPoint    Existing IR to use as insertion point
/// \param NewNode           New IR to insert before \p InsertionPoint
void irNodeInsertBefore(IRNode *InsertionPoint, IRNode *NewNode);

/// Insert an IR node after another IR node
///
/// \param InsertionPoint    Existing IR to use as insertion point
/// \param NewNode           New IR to insert after \p InsertionPoint
void irNodeInsertAfter(IRNode *InsertionPoint, IRNode *NewNode);

/// Set the EH region for an IR node
///
/// \param Node        The IR node of interest
/// \param Region      The EH region to associate with the \p Node
void irNodeSetRegion(IRNode *Node, EHRegion *Region);

/// Get the EH region for an IR node
///
/// \param Node       The IR node of interest
/// \returns          The EH region associated with \p Node
EHRegion *irNodeGetRegion(IRNode *Node);

/// Get the flow graph node for an IR node
///
/// \param Node       The IR node of interest
/// \returns          The flow graph node containing \p Node
FlowGraphNode *irNodeGetEnclosingBlock(IRNode *Node);

/// Determine if an IR node is a label
///
/// \param Node       The IR node of interest
/// \returns          True iff \p Node is a label
bool irNodeIsLabel(IRNode *Node);

/// Determine if this  IR node is a branch
///
/// \param Node   The  IR node to examine
/// \returns      True iff \p Node is a branch
bool irNodeIsBranch(IRNode *Node);

/// Determine if an IR node is an EH flow annotation
///
/// \param Node       The IR node of interest
/// \returns          True iff \p Node is an EH flow annotation
bool irNodeIsEHFlowAnnotation(IRNode *Node);

/// Determine if an IR node is an EH handler flow annotation
///
/// \param Node       The IR node of interest
/// \returns          True iff \p Node is an EH handler flow annotation
bool irNodeIsHandlerFlowAnnotation(IRNode *Node);

///@}

/// \name Client BranchList interface
/// Used by \p fgFixRecursiveEdges to undo branches added by the optimistic
/// recursive tail call transformation. Implementation supplied by the client.
///@{

/// Get the next branch list item
///
/// \param BranchList    Current list item
/// \returns             Next list item
BranchList *branchListGetNext(BranchList *BranchList);

/// Get the client IR for a branch list item
///
/// \param BranchList    Current list item
/// \returns             Client IR for the item
IRNode *branchListGetIRNode(BranchList *BranchList);

///@}

/// Record information about a branch for verification
struct VerificationBranchInfo {
  uint32_t SrcOffset;           ///< MSIL offset of the branch
  uint32_t TargetOffset;        ///< MSIL offset of the branch target
  IRNode *BranchOp;             ///< Client IR for the branch
  bool IsLeave;                 ///< True if branch is from a leave opcode
  VerificationBranchInfo *Next; ///< Next branch to verify
};

/// Translate a call opcode from the general MSIL opcode enumeration into
/// the call-specific opcode enumeration.
/// \param Opcode     MSIL opcode
/// \returns          MSIL call opcode
ReaderBaseNS::CallOpcode remapCallOpcode(ReaderBaseNS::OPCODE Opcode);

/// \brief Parameters needed for converting MSIL to client IR for
/// a particular flow graph node.
///
/// The MSIL conversion for a flow graph node happens within a protected region
/// set up by \p readBytesForFlowGraphNode. This struct is used to pass state
/// information back and forth to the helper method.
struct ReadBytesForFlowGraphNodeHelperParam {
  ReaderBase *This;          ///< The base reader instance
  ReaderException *Excep;    ///< Captured exception state
  FlowGraphNode *Fg;         ///< The flow graph node being processed
  bool IsVerifyOnly;         ///< True if the reader is just verifying
  uint32_t CurrentOffset;    ///< Offset within the IL stream
  bool LocalFault;           ///< True if the current node failed
                             ///< verification
  bool HasFallThrough;       ///< True if the control flows off the end
                             ///< of this node into the next one
  VerificationState *VState; ///< Verifier state for the node
  bool VerifiedEndBlock;     ///< True if we've verified the block has
                             ///< an appropriate ending
};

static const int32_t SizeOfCEECall = 5; ///< size of MSIL call plus operand

/// \brief \p ReaderBase is an abstract base class for tools that need to
/// both model MSIL and interact with the CoreCLR ExecutionEngine or some
/// similar repository of knowledge.
///
/// \p ReaderBase is intended to be used as a traversal agent through
/// MSIL for a derived \p Reader class (aka *Client*) with more specialized
/// requirements.
///
/// For instance an MSIL Verifier or a JIT might derive from \p ReaderBase
/// and provide suitable implementations of the companion classes.
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

  /// True if this method contains the 'localloc' MSIL opcode.
  bool HasLocAlloc;

  /// True if the client has optimistically transformed tail.
  /// recursion into a branch.
  bool HasOptimisticTailRecursionTransform;

  /// The current instruction's IL offset.
  uint32_t CurrInstrOffset;

  /// The next instruction's IL offset.
  uint32_t NextInstrOffset;

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
  uint32_t CurrentBranchDepth;

  // \brief Indicates that null checks use explicit compare+branch IR sequences
  //
  // Compiling with this set to false isn't really supported (the generated IR
  // would not have sufficient EH annotations), but it is provided as a mock
  // configuration flag to facilitate experimenting with what the IR/codegen
  // could look like with null checks folded onto loads/stores.
  static const bool UseExplicitNullChecks = true;

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

  /// \brief Constructor
  ///
  /// Initializes the base part of the reader object, setting all fields to
  /// zero, and then initializing key fields from the parameters.
  ///
  /// \param CorJitInfo    The jit interface for this method
  /// \param MethodInfo    The method description provided by the EE
  /// \param Flags         Flags indicating various options for processing.
  ///                      \see \p CorJitFlag for more details.
  ReaderBase(ICorJitInfo *CorJitInfo, CORINFO_METHOD_INFO *MethodInfo,
             uint32_t Flags);

  /// \brief Main entry point for the reader
  ///
  /// This method orchestrates client processing of the MSIL for the method
  /// specified by the constructor parameters. The processing runs in a series
  /// of steps:
  /// 1. Pre pass to allow the client to do initialization
  /// 2. Setup: get sequents points, verify the exception information,
  ///    build a region tree for exception handling, look for special keep
  ///    alive parameters
  /// 3. Build a flow graph for the method. Client is invoked on the explicit
  ////   control flow inducing instructions in the MSIL.
  /// 4. Middle pass to allow the client to process the flow graph
  /// 5. Depth-first preorder traversal of the blocks in the flow graph. Client
  ///    is invoked on each instruction in the block.
  /// 6. Flow graph cleanup
  /// 7. Final pass to allow the client a chance to finish up
  void msilToIR();

  /// \brief Set up parameters and locals (autos)
  ///
  /// Uses the method and local signatures and information from the EE to
  /// direct client processing for the method parameters and the local
  /// variables of the method.
  ///
  /// \params NumParams   Number of parameters to the method. Note this may
  ///                     include implicit parameters like the this pointer.
  /// \params NumAutos    Number of locals described in the local signature.
  void initParamsAndAutos(uint32_t NumParam, uint32_t NumAuto);

  /// \brief Set up parameters
  ///
  /// Uses the method signature and information from the EE to direct the
  /// client to set up processing for method parameters.
  ///
  /// \params NumParams   Number of parameters to the method. Note this may
  ///                     include implicit parameters like the this pointer.
  void buildUpParams(uint32_t NumParams);

  /// \brief Set up locals (autos)
  ///
  /// Uses the local signature to direct the client to set up processing
  /// for local variables in the method.
  ///
  /// \params NumAutos   Number of locals described in the signature.
  void buildUpAutos(uint32_t NumAutos);

  /// \brief Process the next element (argument or local) in a signature
  ///
  /// Utility routine used by \p buildUpParams and \p buildUpLocals to iterate
  /// through a signature and obtain more detailed information about each
  /// element.
  ///
  /// \params ArgListHandle  Handle for the current element of the signature
  /// \param Sig             The signature being iterated over.
  /// \param CorType [out]   Optional; the CorInfoType of the current element.
  /// \param Class [out]     Optional; the class handle of the current element.
  /// \param IsPinned [out]  Optional; true if the current element is pinned.
  ///
  /// \returns Handle for the next element of the signature.
  CORINFO_ARG_LIST_HANDLE argListNext(CORINFO_ARG_LIST_HANDLE ArgListHandle,
                                      CORINFO_SIG_INFO *Sig,
                                      CorInfoType *CorType = nullptr,
                                      CORINFO_CLASS_HANDLE *Class = nullptr,
                                      bool *IsPinned = nullptr);

  /// \brief Build the flow graph for the method
  ///
  /// Create a flow graph for the method. This determines which range of
  /// MSIL instructions will lie within each node in the flow graph,
  /// and also builds associations between the nodes and the EH regions.
  ///
  /// \param FgTail [out] The last flow graph node in the graph.
  /// \returns The first flow graph node in the graph.
  FlowGraphNode *buildFlowGraph(FlowGraphNode **FgTail);

  /// \brief Split a flow graph node (aka block)
  ///
  /// Break the indicated \p Block into two blocks, with the divsion
  /// happening at the indicated MSIL \p Offset. The client method
  /// \p fgSplitBlock is invoked to allow the client to update its model
  /// of the code as necessary.
  ///
  /// \param Block   The flow graph mode to split
  /// \param Offset  The MSIL offset of the split point. Must be within
  ///                the MSIL range for the block.
  /// \param IRNode  The IR node corresponding to the split point.
  /// \returns       The new node.
  FlowGraphNode *fgSplitBlock(FlowGraphNode *Block, uint32_t Offset,
                              IRNode *Node);

#if defined(_DEBUG)
  /// \brief Debug-only reader function to print range of MSIL.
  ///
  /// Print the MSIL in the buffer for the given range. Output emitted via
  /// \p dbPrint().
  ///
  /// \param Buf           Buffer containing MSIL bytecode
  /// \param StartOffset   Initial offset for the range to print
  /// \param EndOffset     Ending offset for the range to print
  void printMSIL(uint8_t *Buf, uint32_t StartOffset, uint32_t EndOffset);
#endif

  /// \brief Determine the effect of this instruction on the operand stack
  ///
  /// Many MSIL instructions push or pop operands from the stack, or both pop
  /// and push operands. This method determines the net number of pushes and
  /// pops for a particular instruction.
  ///
  /// \param Opcode     The MSIL opcode for the instruction
  /// \param Operand    For call opcodes with signature tokens, pointer to the
  ///                   token value in the IL stream
  /// \param Pop [out]  Number of operands popped from the stack
  /// \param Push [out] Number of operands pushed onto the stack
  void getMSILInstrStackDelta(ReaderBaseNS::OPCODE Opcode, uint8_t *Operand,
                              uint16_t *Pop, uint16_t *Push);

private:
  /// \brief Determine if a call instruction is a candidate to be a tail call
  ///
  /// The client may decide to give special tail-call treatment to calls that
  /// are followed closely by returns, even if the calls are not marked with
  /// the tail prefix. This method determines if such treatment is possible.
  ///
  /// \param ILInput       Pointer to the start of the MSIL bytecode stream
  /// \param ILInputSize   Length of the MSIL bytecode stream
  /// \param NextOffset    Offset into the stream just past the call
  /// \param Token         Token value for calls that have sig tokens
  ///
  /// \returns             True if treating this call as a tail call is
  ///                      reasonble.
  bool isUnmarkedTailCall(uint8_t *ILInput, uint32_t ILInputSize,
                          uint32_t NextOffset, mdToken Token);

  /// \brief Helper method called from \p isUnmarkedTailCall
  ///
  /// \param ILInput       Pointer to the start of the MSIL bytecode stream
  /// \param ILInputSize   Length of the MSIL bytecode stream
  /// \param NextOffset    Offset into the stream just past the call
  /// \param Token         Token value for calls that have sig tokens
  ///
  /// \returns             True if treating this call as a tail call is
  ///                      reasonble.
  bool isUnmarkedTailCallHelper(uint8_t *ILInput, uint32_t ILInputSize,
                                uint32_t NextOffset, mdToken Token);

  /// \brief Check if the current instruction is a valid explicit tail call
  ///
  /// Verify that this call is a valid explicit tail call. The call must be
  /// closely followed by a return.
  ///
  /// \param ILOffset       Offset of the call instruction in the IL stream
  /// \param AllowPop       true if it is acceptable for the call to be
  ///                       followed by a single pop before reaching the
  ///                       return.
  /// \returns              True if the current instruction is a valid explicit
  ///                       tail call.
  bool checkExplicitTailCall(uint32_t ILOffset, bool AllowPop);

  /// \brief Convert the MSIL for this flow graph node into the client IR
  ///
  /// Orchestrates client processing the MSIL in this flow graph node,
  /// filling in the block contents. This outer method sets up a parameter
  /// block for its helper method and the invokes its helper within a protected
  /// region so that various errors can be caught and handled appropriately.
  ///
  /// \param Fg                 The flow graph node to process.
  /// \param IsVerifyOnly       True if the reader is only verifying the MSIL.
  void readBytesForFlowGraphNode(FlowGraphNode *Fg, bool IsVerifyOnly);

  /// \brief Helper method for \p readBytesForFlowGraphNode
  ///
  /// Helper method that orchestrates client processing the MSIL in a flow
  /// graph node specified by the parameters.
  ///
  /// \param Param              Encapsulated state from the main method
  /// \param IsVerifyOnly       True if the reader is only verifying the MSIL.
  void
  readBytesForFlowGraphNodeHelper(ReadBytesForFlowGraphNodeHelperParam *Param);

private:
  /// \brief Perform special processing for blocks that start EH regions.
  ///
  /// Uses the \p CurrentFgNode to determine which block to process. Ensures
  /// the operand stack and debugger info is in the proper state for blocks
  /// that start EH regions.
  void setupBlockForEH();

  /// \brief Check if this offset is the start of an MSIL instruction.
  ///
  /// Helper used to check whether branch targets and similar are referring
  /// to the start of instructions.
  ///
  /// \param Offset     Offset into the MSIL stream
  /// \returns          True if \pO Offset is the start of an MSIL instruction.
  bool isOffsetInstrStart(uint32_t Offset);

  // \brief Get custom sequence points
  ///
  /// This method checks with the EE to see if there are any debugger-specified
  /// sequence points in the method. The results of this call is a bit vector
  /// of offsets, stored in \p CustomSequencePoints. The native offsets that
  /// correspond to these sequence points must be reported back in debug
  /// information.
  void getCustomSequencePoints();

  /// \name FlowGraph
  ///{@

  /// \brief Build the flow graph for the method.
  ///
  /// Walks the MSIL in the buffer, looking for explicit control flow
  /// instructions (branches and similar). Uses these to instruct the client
  /// to produce a flow graph describing the method's control flow.
  ///
  /// \param Buffer      Buffer of MSIL bytecodes
  /// \param BufferSize  Length of the buffer in bytes
  /// \returns           Head node of the flow graph.
  FlowGraphNode *fgBuildBasicBlocksFromBytes(uint8_t *Buffer,
                                             uint32_t BufferSize);

  /// \brief First pass of flow graph construction
  ///
  /// This pass determines the legal starting points of all MSIL instructions,
  /// locates all block-ending instructions (branches and similar) and all
  /// branch targets, and builds blocks that end with the block-ending
  /// instructions and have appropriate flow graph edges to successor blocks.
  ///
  /// \param Fg          Nominal entry node for the method
  /// \param Buffer      Buffer of MSIL bytecodes
  /// \param BufferSize  Length of the buffer in bytes
  void fgBuildPhase1(FlowGraphNode *Fg, uint8_t *Buffer, uint32_t BufferSize);

  /// Create initial global verification data for all blocks in the flow graph
  ///
  /// \param HeadBlock   Initial block in the flow graph
  void fgAttachGlobalVerifyData(FlowGraphNode *HeadBlock);

  /// Perform special case repair for recursive tail call and localloc
  ///
  /// The flow graph builder can optimistically describe recursive tail
  /// calls as branches back to the start of the method. However this must
  /// be undone if the method being compiled contains a localloc.
  ///
  /// \param HeadBlock   Initial block in the flow graph.
  void fgFixRecursiveEdges(FlowGraphNode *HeadBlock);

  /// Helper method for building up the cases of a switch.
  ///
  /// \param SwitchNode    The client IR representing the switch
  /// \param LabelNode     The client IR representing the case target
  /// \param Element       The switch value for this case
  /// \returns             The case node added to the switch.
  IRNode *fgAddCaseToCaseListHelper(IRNode *SwitchNode, IRNode *LabelNode,
                                    uint32_t Element);

  /// \brief Add the unvisited successors of this block to the worklist
  ///
  /// This method scans all the successor blocks of \p CurrBlock, and
  /// if there are any, creates new work list nodes for these successors,
  /// marks them as visited, and (despite the method name) prepends them,
  /// returning a new worklist head node.
  ///
  /// \param Worklist       The current worklist of unvisited blocks
  /// \param CurrBlock      The block to examine for unvisited successors
  /// \returns              Updated list of unvisited blocks.
  FlowGraphNodeWorkList *
  fgAppendUnvisitedSuccToWorklist(FlowGraphNodeWorkList *Worklist,
                                  FlowGraphNode *CurrBlock);

  /// \brief Remove this flow graph node and associated client IRNodes.
  ///
  /// This method removes \p Block from the flow graph along with all
  /// incoming and outgoing edges. It also invokes \p fgDeleteNodesFromBlock
  /// to enable the client to remove any IRNodes.
  ///
  /// \param Block          The block to delete
  void fgDeleteBlockAndNodes(FlowGraphNode *Block);

  /// \brief Ensure the start of the current region includes all labels
  ///        found right at the start of the region.
  ///
  /// Make sure that any instruction that targets this region via a
  /// label is targeting the inside of the region, by moving the region
  /// entry up before the labels as necessary.
  ///
  /// \param HandlerStartNode      First instruction in the region.
  void fgEnsureEnclosingRegionBeginsWithLabel(IRNode *HandlerStartNode);

  /// \brief Get the innermost region that contains this MSIL offset
  ///
  /// Scan the EH regions of the method looking for the smallest region
  /// that contains this MSIL offset.
  ///
  /// \param Offset        The MSIL offset in question
  /// \returns             Pointer to the innermost region, or nullptr if none
  EHRegion *fgGetRegionFromMSILOffset(uint32_t Offset);

  /// \brief Update the branches with temporary targets at this offset.
  ///
  /// When the flow graph is initially built, branches target temporary
  /// graph nodes. This function updates all of the branches that target
  /// a particular temporary node with the actual node representing the
  /// target in the flow graph.
  ///
  /// \param Offset             The MSIL offset of the branch target
  /// \param TempBranchTarget   The placeholder node for the target
  /// \param StartBlock         A node at an offset less than the target.
  /// \returns                  The updated target block
  FlowGraphNode *fgReplaceBranchTarget(uint32_t Offset,
                                       FlowGraphNode *TempBranchTarget,
                                       FlowGraphNode *StartBlock);

  /// \brief Update all branches with temporary targets
  ///
  /// Walks the list of branch target offsets, invoking
  /// \p fgReplaceBranchTarget to update all branches with that target to
  /// refer to the proper blocks.
  void fgReplaceBranchTargets();

  /// Process the end of a try region
  /// \param EHRegion   The region to process.
  void fgInsertTryEnd(EHRegion *EhRegion);

  /// Place client IR into the start of the EH region that begins at Offset.
  ///
  /// \param Offset        MSIL offset of the start of an EH region
  /// \param EHNode        Client IR to insert into the block that
  ///                      starts that region.
  void fgInsertBeginRegionExceptionNode(uint32_t Offset, IRNode *EHNode);

  /// Place client IR into the end of the EH region that ends at Offset.
  ///
  /// \param Offset        MSIL offset of the end of an EH region
  /// \param EHNode        Client IR to insert into the block that
  ///                      ends that region.
  void fgInsertEndRegionExceptionNode(uint32_t Offset, IRNode *EHNode);

  /// Add client IR for an EH region and all contained regions.
  ///
  /// \param Region    Root region to process
  void fgInsertEHAnnotations(EHRegion *Region);

  /// \brief Create branch IR
  ///
  /// Have the client create a branch to \p LabelNode from \p BlockNode.
  ///
  /// \param LabelNode       target block of the branch
  /// \param BlockNode       source block for the branch
  /// \param Offset          MSIL offset of the branch target
  /// \param IsConditional   true if this is a conditional branch
  /// \param IsNominal       true if this is a nominal branch
  /// \returns               IRNode for the branch created
  IRNode *fgMakeBranchHelper(IRNode *LabelNode, IRNode *BlockNode,
                             uint32_t Offset, bool IsConditional,
                             bool IsNominal);

  /// \brief Create IR for an endfinally instruction
  ///
  /// Have the client create IR for an endfinally instruction. Note
  /// there can be more than one of these in a finally region.
  ///
  /// \param BlockNode     the block that is the end of the finally
  /// \param CurentOffset  msil offset for the endfinally instruction
  /// \param IsLexicalEnd  true if the endfinally is at the end of the finally
  IRNode *fgMakeEndFinallyHelper(IRNode *BlockNode, uint32_t Offset,
                                 bool IsLexicalEnd);

  /// \brief Remove all unreachable blocks
  ///
  /// Walk the flow graph and remove any block (except the tail block)
  /// that cannot be reached from the head block. Blocks are removed by
  /// calling \p fgDeleteBlockAndNodes.
  ///
  /// \param FgHead     the head block of the flow graph
  /// \param FgTail     the tail block of the flow graph
  void fgRemoveUnusedBlocks(FlowGraphNode *FgHead, FlowGraphNode *FgTail);

  /// Find the canonical landing point for leaves from an EH region
  ///
  /// \param Region    The region in question.
  /// \returns         The MSIL offset of the canonical landing point
  uint32_t fgGetRegionCanonicalExitOffset(EHRegion *Region);

  /// Buffer used by \p fgGetRegionCanonicalExitOffset for cases where
  /// there are large amounts of MSIL for the method.
  int32_t *FgGetRegionCanonicalExitOffsetBuff;

  /// \brief Create or return a flow graph node for the indicated offset
  ///
  /// This method sees if there is an existing flow graph node that begins at
  /// the indicated target. If so, \p Node is set to this block. If not, a
  /// temporary block is allocated to use as a target, and an entry is added
  /// to the \p NodeOffsetListArray so a subsequent pass can update the
  /// temporary target blocks to real target blocks.
  ///
  /// \param Node [out]          Node to use as the branch target
  /// \param TargetOffset        MSIL offset of the branch target
  /// \returns                   List node of target in offset list
  FlowGraphNodeOffsetList *fgAddNodeMSILOffset(FlowGraphNode **Node,
                                               uint32_t TargetOffset);

  /// Determine if a leave exits the enclosing EH region in a non-local manner.
  ///
  /// \param Fg                            flow graph node containing the leave
  /// \param LeaveOffset                   MSIL offset of the leave
  /// \param LeaveTarget                   MSIL offset of the leave's target
  /// \param EndsWithNonLocalGoto [out]    Target of the leave is not the
  ///                                      canonical exit offset of the region
  ///
  /// \returns True if this is a nonlocal leave
  bool fgLeaveIsNonLocal(FlowGraphNode *Fg, uint32_t LeaveOffset,
                         uint32_t LeaveTarget, bool *EndsWithNonLocalGoto);

  ///@}

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
  void clearStack();

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
                      bool GiveUp, CORINFO_CLASS_HANDLE Owwner = nullptr);
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
                  ReaderBaseNS::CallOpcode Opcode, IRNode **CallNode);

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
                             IRNode **ThisPointer);

  IRNode *rdrMakeLdFtnTargetNode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                 CORINFO_CALL_INFO *CallInfo);

  IRNode *rdrGetDirectCallTarget(ReaderCallTargetData *CallTargetData);

  IRNode *rdrGetDirectCallTarget(CORINFO_METHOD_HANDLE Method,
                                 mdToken MethodToken, bool NeedsNullCheck,
                                 bool CanMakeDirectCall, bool &UsesMethodDesc);
  IRNode *
  rdrGetCodePointerLookupCallTarget(ReaderCallTargetData *CallTargetData);

  IRNode *rdrGetCodePointerLookupCallTarget(CORINFO_CALL_INFO *CallInfo,
                                            bool &IsIndirect);

  IRNode *rdrGetIndirectVirtualCallTarget(ReaderCallTargetData *CallTargetData,
                                          IRNode **ThisPointer);

  IRNode *rdrGetVirtualStubCallTarget(ReaderCallTargetData *CallTargetData);

  IRNode *rdrGetVirtualTableCallTarget(ReaderCallTargetData *CallTargetData,
                                       IRNode **ThisPointer);

  // Delegate invoke and delegate construct optimizations
  bool rdrCallIsDelegateInvoke(ReaderCallTargetData *CallTargetData);
  bool rdrCallIsDelegateConstruct(ReaderCallTargetData *CallTargetData);
#ifdef FEATURE_CORECLR
  void rdrInsertCalloutForDelegate(CORINFO_CLASS_HANDLE DelegateType,
                                   CORINFO_METHOD_HANDLE CalleeMethod,
                                   mdToken MethodToken);
#endif // FEATURE_CORECLR
  IRNode *rdrGetDelegateInvokeTarget(ReaderCallTargetData *CallTargetData,
                                     IRNode **ThisPtr);

public:
  void rdrCallFieldHelper(
      CORINFO_RESOLVED_TOKEN *ResolvedToken, CorInfoHelpFunc HelperId,
      bool IsLoad,
      IRNode *Dst, // dst node if this is a load, otherwise nullptr
      IRNode *Obj, IRNode *Value, ReaderAlignType Alignment, bool IsVolatile);
  void rdrCallWriteBarrierHelper(IRNode *Arg1, IRNode *Arg2,
                                 ReaderAlignType Alignment, bool IsVolatile,

                                 CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                 bool IsNonValueClass, bool IsValueIsPointer,
                                 bool IsFieldToken, bool IsUnchecked);
  void rdrCallWriteBarrierHelperForReturnValue(IRNode *Arg1, IRNode *Arg2,
                                               mdToken Token);
  IRNode *rdrGetFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             CORINFO_FIELD_INFO *FieldInfo, IRNode *Obj,
                             bool BaseIsGCObj, bool MustNullCheck);
  IRNode *rdrGetStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                   CORINFO_FIELD_INFO *FieldInfo);
  IRNode *rdrCallGetStaticBase(CORINFO_CLASS_HANDLE Class, mdToken ClassToken,
                               CorInfoHelpFunc HelperId, bool NoCtor,
                               bool CanMoveUp, IRNode *Dst);

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
  GCLayout *getClassGCLayout(CORINFO_CLASS_HANDLE Class);
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
                           CORINFO_CLASS_HANDLE Owner = nullptr);
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

  void handleClassAccess(CORINFO_RESOLVED_TOKEN *ResolvedToken);
  void handleMemberAccess(CorInfoIsAccessAllowedResult AccessAllowed,
                          const CORINFO_HELPER_DESC &AccessHelper);
  void handleMemberAccessWorker(CorInfoIsAccessAllowedResult AccessAllowed,
                                const CORINFO_HELPER_DESC &AccessHelper);
  void
  handleMemberAccessForVerification(CorInfoIsAccessAllowedResult AccessAllowed,
                                    const CORINFO_HELPER_DESC &AccessHelper,
#ifdef CC_PEVERIFY
                                    HRESULT HResult
#else
                                    const char *HResult
#endif // CC_PEVERIFY
                                    );
  void insertHelperCall(const CORINFO_HELPER_DESC &HelperCallDesc);
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
  IRNode *rdrGetCritSect();

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

  virtual bool abs(IRNode *Arg1, IRNode **RetVal) = 0;

  virtual IRNode *argList() = 0;
  virtual IRNode *instParam() = 0;
  virtual IRNode *secretParam() = 0;
  virtual IRNode *thisObj() = 0;
  virtual void boolBranch(ReaderBaseNS::BoolBranchOpcode Opcode,
                          IRNode *Arg1) = 0;
  virtual IRNode *box(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                      uint32_t *NextOffset = nullptr,
                      VerificationState *VState = nullptr);
  virtual IRNode *binaryOp(ReaderBaseNS::BinaryOpcode Opcode, IRNode *Arg1,
                           IRNode *Arg2) = 0;
  virtual void branch() = 0;
  virtual void breakOpcode();
  virtual IRNode *call(ReaderBaseNS::CallOpcode Opcode, mdToken Token,
                       mdToken ConstraintTypeRef, mdToken LoadFtnToken,
                       bool IsReadOnlyPrefix, bool IsTailCallPrefix,
                       bool IsUnmarkedTailCall, uint32_t CurrentOffset,
                       bool *IsRecursiveTailCall) = 0;
  virtual IRNode *castClass(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode *ObjRefNode);
  virtual IRNode *isInst(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                         IRNode *ObjRefNode);
  virtual IRNode *castOp(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                         IRNode *ObjRefNode, CorInfoHelpFunc HelperId) = 0;

  virtual IRNode *ckFinite(IRNode *Arg1) = 0;
  virtual IRNode *cmp(ReaderBaseNS::CmpOpcode Opcode, IRNode *Arg1,
                      IRNode *Arg2) = 0;
  virtual void condBranch(ReaderBaseNS::CondBranchOpcode Opcode, IRNode *Arg1,
                          IRNode *Arg2) = 0;
  virtual IRNode *conv(ReaderBaseNS::ConvOpcode Opcode, IRNode *Arg1) = 0;
  virtual void cpBlk(IRNode *ByteCount, IRNode *SourceAddress,
                     IRNode *DestinationAddress, ReaderAlignType Alignment,
                     bool IsVolatile);
  virtual void cpObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                     IRNode *Arg2, ReaderAlignType Alignment, bool IsVolatile);
  virtual void dup(IRNode *Opr, IRNode **Result1, IRNode **Result2) = 0;
  virtual void endFilter(IRNode *Arg1) = 0;

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

  virtual IRNode *
  getStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken) = 0;
  virtual void initBlk(IRNode *NumBytes, IRNode *ValuePerByte,
                       IRNode *DestinationAddress, ReaderAlignType Alignment,
                       bool IsVolatile);
  virtual void initObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg2);
  virtual void insertThrow(CorInfoHelpFunc ThrowHelper, uint32_t Offset);
  virtual void jmp(ReaderBaseNS::CallOpcode Opcode, mdToken Token, bool HasThis,
                   bool HasVarArg) = 0;

  virtual void leave(uint32_t TargetOffset, bool IsNonLocal,
                     bool EndsWithNonLocalGoto) = 0;
  virtual IRNode *loadArg(uint32_t ArgOrdinal, bool IsJmp) = 0;
  virtual IRNode *loadLocal(uint32_t ArgOrdinal) = 0;
  virtual IRNode *loadArgAddress(uint32_t ArgOrdinal) = 0;
  virtual IRNode *loadLocalAddress(uint32_t LocOrdinal) = 0;
  virtual IRNode *loadConstantI4(int32_t Constant) = 0;
  virtual IRNode *loadConstantI8(int64_t Constant) = 0;
  virtual IRNode *loadConstantI(size_t Constant) = 0;
  virtual IRNode *loadConstantR4(float Value) = 0;
  virtual IRNode *loadConstantR8(double Value) = 0;
  virtual IRNode *loadElem(ReaderBaseNS::LdElemOpcode Opcode,
                           CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Index,
                           IRNode *Array) = 0;
  virtual IRNode *loadElemA(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode *Index, IRNode *Array, bool IsReadOnly) = 0;
  virtual IRNode *loadField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                            ReaderAlignType Alignment, bool IsVolatile) = 0;
  virtual IRNode *loadIndir(ReaderBaseNS::LdIndirOpcode Opcode, IRNode *Address,
                            ReaderAlignType Alignment, bool IsVolatile,
                            bool IsInterfReadOnly,
                            bool AddressMayBeNull = true);
  IRNode *loadIndirNonNull(ReaderBaseNS::LdIndirOpcode Opcode, IRNode *Address,
                           ReaderAlignType Alignment, bool IsVolatile,
                           bool IsInterfReadOnly) {
    return loadIndir(Opcode, Address, Alignment, IsVolatile, IsInterfReadOnly,
                     false);
  }
  virtual IRNode *loadNull() = 0;
  virtual IRNode *localAlloc(IRNode *Arg, bool IsZeroInit) = 0;
  virtual IRNode *loadFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                   IRNode *Obj) = 0;
  virtual IRNode *loadLen(IRNode *Array, bool ArrayMayBeNull = true) = 0;
  IRNode *loadLenNonNull(IRNode *Array) { return loadLen(Array, false); }
  virtual bool arrayAddress(CORINFO_SIG_INFO *Aig, IRNode **RetVal) = 0;
  virtual IRNode *loadStringLen(IRNode *Arg1) = 0;
  virtual IRNode *getTypeFromHandle(IRNode *Arg1) = 0;
  virtual IRNode *getValueFromRuntimeHandle(IRNode *Arg1) = 0;
  virtual IRNode *arrayGetDimLength(IRNode *Arg1, IRNode *Arg2,
                                    CORINFO_CALL_INFO *CallInfo) = 0;
  virtual IRNode *loadObj(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                          IRNode *Address, ReaderAlignType AlignmentPrefix,
                          bool IsVolatile, bool IsField,
                          bool AddressMayBeNull = true);
  IRNode *loadObjNonNull(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Address,
                         ReaderAlignType AlignmentPrefix, bool IsVolatile,
                         bool IsField) {
    return loadObj(ResolvedToken, Address, AlignmentPrefix, IsVolatile, IsField,
                   false);
  }
  virtual IRNode *loadAndBox(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             IRNode *Address,
                             ReaderAlignType AlignmentPrefix) = 0;
  virtual IRNode *convertHandle(IRNode *GetTokenNumeric,
                                CorInfoHelpFunc HelperID,
                                CORINFO_CLASS_HANDLE Class) = 0;
  virtual void
  convertTypeHandleLookupHelperToIntrinsic(bool CanCompareToGetType) = 0;

  virtual IRNode *loadStaticField(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                  bool IsVolatile) = 0;
  virtual IRNode *loadStr(mdToken Token) = 0;
  virtual IRNode *loadToken(CORINFO_RESOLVED_TOKEN *ResolvedToken);
  virtual IRNode *loadVirtFunc(IRNode *Arg1,
                               CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               CORINFO_CALL_INFO *CallInfo) = 0;
  virtual IRNode *loadPrimitiveType(IRNode *Address, CorInfoType CorType,
                                    ReaderAlignType Alignment, bool IsVolatile,
                                    bool IsInterfConst,
                                    bool AddressMayBeNull = true) = 0;
  IRNode *loadPrimitiveTypeNonNull(IRNode *Address, CorInfoType CorType,
                                   ReaderAlignType Alignment, bool IsVolatile,
                                   bool IsInterfConst) {
    return loadPrimitiveType(Address, CorType, Alignment, IsVolatile,
                             IsInterfConst, false);
  }
  virtual IRNode *loadNonPrimitiveObj(IRNode *Address,
                                      CORINFO_CLASS_HANDLE Class,
                                      ReaderAlignType Alignment,
                                      bool IsVolatile,
                                      bool AddressMayBeNull = true) = 0;
  IRNode *loadNonPrimitiveObjNonNull(IRNode *Address,
                                     CORINFO_CLASS_HANDLE Class,
                                     ReaderAlignType Alignment,
                                     bool IsVolatile) {
    return loadNonPrimitiveObj(Address, Class, Alignment, IsVolatile, false);
  }
  virtual IRNode *makeRefAny(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             IRNode *Object) = 0;
  virtual IRNode *newArr(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                         IRNode *Arg1) = 0;
  virtual IRNode *newObj(mdToken Token, mdToken LoadFtnToken,
                         uint32_t CurrentOffset) = 0;
  virtual void pop(IRNode *Opr) = 0;
  virtual IRNode *refAnyType(IRNode *Arg1) = 0;
  virtual IRNode *refAnyVal(IRNode *Val, CORINFO_RESOLVED_TOKEN *ResolvedToken);
  virtual void rethrow() = 0;
  virtual void returnOpcode(IRNode *Opr, bool IsSynchronousMethod) = 0;
  virtual IRNode *shift(ReaderBaseNS::ShiftOpcode Opcode, IRNode *ShiftAmount,
                        IRNode *ShiftOperand) = 0;
  virtual IRNode *sizeofOpcode(CORINFO_RESOLVED_TOKEN *ResolvedToken) = 0;
  virtual void storeArg(uint32_t LocOrdinal, IRNode *Arg1,
                        ReaderAlignType Alignment, bool IsVolatile) = 0;
  virtual void storeElem(ReaderBaseNS::StElemOpcode Opcode,
                         CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                         IRNode *Arg2, IRNode *Arg3) = 0;
  virtual void storeElemRefAny(IRNode *Value, IRNode *Index, IRNode *Obj);
  virtual void storeField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                          IRNode *Arg2, ReaderAlignType Alignment,
                          bool IsVolatile) = 0;
  virtual void storeIndir(ReaderBaseNS::StIndirOpcode Opcode, IRNode *Arg1,
                          IRNode *Arg2, ReaderAlignType Alignment,
                          bool IsVolatile, bool AddressMayBeNull = true);
  void storeIndirNonNull(ReaderBaseNS::StIndirOpcode Opcode, IRNode *Arg1,
                         IRNode *Arg2, ReaderAlignType Alignment,
                         bool IsVolatile) {
    storeIndir(Opcode, Arg1, Arg2, Alignment, IsVolatile, false);
  }
  virtual void storePrimitiveType(IRNode *Value, IRNode *Address,
                                  CorInfoType CorType,
                                  ReaderAlignType Alignment, bool IsVolatile,

                                  bool AddressMayBeNull = true) = 0;
  void storePrimitiveTypeNonNull(IRNode *Value, IRNode *Address,
                                 CorInfoType CorType, ReaderAlignType Alignment,
                                 bool IsVolatile) {
    storePrimitiveType(Value, Address, CorType, Alignment, IsVolatile, false);
  }
  virtual void storeLocal(uint32_t LocOrdinal, IRNode *Arg1,
                          ReaderAlignType Alignment, bool IsVolatile) = 0;
  virtual void storeObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Value,
                        IRNode *Address, ReaderAlignType Alignment,
                        bool IsVolatile, bool IsField,
                        bool AddressMayBeNull = true);
  void storeObjNonNull(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Value,
                       IRNode *Address, ReaderAlignType Alignment,
                       bool IsVolatile, bool IsField) {
    storeObj(ResolvedToken, Value, Address, Alignment, IsVolatile, IsField,
             false);
  }
  virtual void storeStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                                IRNode *ValueToStore, bool IsVolatile) = 0;
  virtual IRNode *stringGetChar(IRNode *Arg1, IRNode *Arg2) = 0;
  virtual bool sqrt(IRNode *Arg1, IRNode **RetVal) = 0;

  virtual bool interlockedIntrinsicBinOp(IRNode *Arg1, IRNode *Arg2,
                                         IRNode **RetVal,
                                         CorInfoIntrinsics IntrinsicID) = 0;
  virtual bool interlockedCmpXchg(IRNode *Arg1, IRNode *Arg2, IRNode *Arg3,
                                  IRNode **RetVal,
                                  CorInfoIntrinsics IntrinsicID) = 0;
  virtual bool memoryBarrier() = 0;
  virtual void switchOpcode(IRNode *Opr) = 0;
  virtual void throwOpcode(IRNode *Arg1) = 0;
  virtual IRNode *unaryOp(ReaderBaseNS::UnaryOpcode Opcode, IRNode *Arg1) = 0;
  virtual IRNode *unbox(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg2,
                        bool AndLoad, ReaderAlignType Alignment,
                        bool IsVolatile) = 0;

  virtual IRNode *unboxAny(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                           ReaderAlignType Alignment, bool IsVolatilePrefix);
  virtual void nop() = 0;

  virtual void insertIBCAnnotations() = 0;
  virtual IRNode *insertIBCAnnotation(FlowGraphNode *Node, uint32_t Count,
                                      uint32_t Offset) = 0;

  // Insert class constructor
  virtual void insertClassConstructor();

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
  virtual void endFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrentOffset) = 0;

  // Used to maintain operand stack.
  virtual void maintainOperandStack(IRNode **Opr1, IRNode **Opr2,
                                    FlowGraphNode *CurrentBlock) = 0;
  virtual void assignToSuccessorStackNode(FlowGraphNode *, IRNode *Destination,
                                          IRNode *Source, bool *) = 0;
  virtual bool typesCompatible(IRNode *Src1, IRNode *Src2) = 0;

  virtual void removeStackInterference() = 0;

  virtual void removeStackInterferenceForLocalStore(uint32_t Opcode,
                                                    uint32_t Ordinal) = 0;

  // Remove all IRNodes from block (for verification error processing.)
  virtual void clearCurrentBlock() = 0;

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

  virtual bool isCall() = 0;
  virtual bool isRegionStartBlock(FlowGraphNode *Fg) = 0;
  virtual bool isRegionEndBlock(FlowGraphNode *Fg) = 0;

  // Create a symbol node that will be used to represent the stack-incoming
  // exception object
  // upon entry to funclets.
  virtual IRNode *makeExceptionObject() = 0;

  // //////////////////////////////////////////////////////////////////////////
  // Client Supplied Helper Routines, required by VOS support
  // //////////////////////////////////////////////////////////////////////////

  // Asks GenIR to make operand value accessible by address, and return a node
  // that references
  // the incoming operand by address.
  virtual IRNode *addressOfLeaf(IRNode *Leaf) = 0;
  virtual IRNode *addressOfValue(IRNode *Leaf) = 0;

  // Helper callback used by rdrCall to emit call code.
  virtual IRNode *genCall(ReaderCallTargetData *CallTargetDaTA,
                          CallArgTriple *Args, uint32_t NumArgs,
                          IRNode **CallNode) = 0;

  virtual bool canMakeDirectCall(ReaderCallTargetData *CallTargetData) = 0;

  // Generate call to helper
  virtual IRNode *callHelper(CorInfoHelpFunc HelperID, IRNode *Dst,
                             IRNode *Arg1 = nullptr, IRNode *Arg2 = nullptr,
                             IRNode *Arg3 = nullptr, IRNode *Arg4 = nullptr,
                             ReaderAlignType Alignment = Reader_AlignUnknown,
                             bool IsVolatile = false, bool NoCtor = false,
                             bool CanMoveUp = false) = 0;

  // Generate special generics helper that might need to insert flow
  virtual IRNode *callRuntimeHandleHelper(CorInfoHelpFunc Helper, IRNode *Arg1,
                                          IRNode *Arg2,
                                          IRNode *NullCheckArg) = 0;

  virtual IRNode *convertToHelperArgumentType(IRNode *Opr,
                                              uint32_t DestinationSize) = 0;

  virtual IRNode *genNullCheck(IRNode *Node) = 0;

  virtual void
  createSym(uint32_t Num, bool IsAuto, CorInfoType CorType,
            CORINFO_CLASS_HANDLE Class, bool IsPinned,
            ReaderSpecialSymbolType Type = Reader_NotSpecialSymbol) = 0;

  virtual IRNode *derefAddress(IRNode *Address, bool DestIsGCPtr, bool IsConst,

                               bool AddressMayBeNull = true) = 0;
  IRNode *derefAddressNonNull(IRNode *Address, bool DestIsGCPtr, bool IsConst) {
    return derefAddress(Address, DestIsGCPtr, IsConst, false);
  }

  virtual IRNode *conditionalDerefAddress(IRNode *Address) = 0;

  virtual IRNode *getHelperCallAddress(CorInfoHelpFunc HelperId) = 0;

  virtual IRNode *simpleFieldAddress(IRNode *BaseAddress,
                                     CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                     CORINFO_FIELD_INFO *FieldInfo) = 0;

  virtual IRNode *handleToIRNode(mdToken Token, void *EmbedHandle,
                                 void *RealHandle, bool IsIndirect,
                                 bool IsReadOnly, bool IsRelocatable,
                                 bool IsCallTarget,
                                 bool IsFrozenObject = false) = 0;

  // Create an operand that will be used to hold a pointer.
  virtual IRNode *makePtrDstGCOperand(bool IsInteriorGC) = 0;
  virtual IRNode *makePtrNode(ReaderPtrType PointerType = Reader_PtrNotGc) = 0;
  virtual IRNode *makeStackTypeNode(IRNode *Node) = 0;
  virtual IRNode *makeCallReturnNode(CORINFO_SIG_INFO *Sig,
                                     uint32_t *HiddenMBParamSize,
                                     GCLayout **GCInfo) = 0;

  virtual IRNode *makeDirectCallTargetNode(CORINFO_METHOD_HANDLE Method,
                                           void *CodeAddress) = 0;

  // Called once region tree has been built.
  virtual void setEHInfo(EHRegion *EhRegionTree,
                         EHRegionList *EhRegionList) = 0;

  // Line number info
  virtual void sequencePoint(int32_t Offset, ReaderBaseNS::OPCODE PrevOp);
  virtual void setSequencePoint(uint32_t, ICorDebugInfo::SourceTypes) = 0;
  virtual bool needSequencePoints() = 0;

  // Used to turn token into handle/IRNode
  virtual IRNode *
  genericTokenToNode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                     bool EmbedParent = false, bool MustRestoreHandle = false,
                     CORINFO_GENERIC_HANDLE *StaticHandle = nullptr,
                     bool *IsRuntimeLookup = nullptr, bool NeedsResult = true);

  virtual IRNode *runtimeLookupToNode(CORINFO_RUNTIME_LOOKUP_KIND Kind,
                                      CORINFO_RUNTIME_LOOKUP *Lookup);

  // Used to expand multidimensional array access intrinsics
  virtual bool arrayGet(CORINFO_SIG_INFO *Sig, IRNode **RetVal) = 0;
  virtual bool arraySet(CORINFO_SIG_INFO *Sig) = 0;

#if !defined(NDEBUG)
  virtual void dbDumpFunction(void) = 0;
  virtual void dbPrintIRNode(IRNode *Instr) = 0;
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

/// \brief The exception that is thrown when a particular operation is not yet
///        supported by a sublass of ReaderBase.
class NotYetImplementedException {
private:
  const char *TheReason; ///< The message that explains the reason for this
                         ///< exception.

public:
  /// \brief Initializes a new instance of the NotYetImplementedException class
  ///        with a specified error message.
  ///
  /// \param Reason The message that explains the reason for this exception.
  NotYetImplementedException(const char *Reason = "") : TheReason(Reason) {}

  /// \brief Returns the message that explains the reason for this exception.
  ///
  /// \returns The message that explains the reason for this exception.
  const char *reason() { return TheReason; }
};

#endif // MSIL_READER_H
