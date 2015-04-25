//===------------------- include/Reader/readerir.h --------------*- C++ -*-===//
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
/// \brief Declares the GenIR class, which overrides ReaderBase to generate LLVM
/// IR from MSIL bytecode.
///
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_IR_H
#define MSIL_READER_IR_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallSite.h"
#include "reader.h"
#include "abi.h"
#include "abisignature.h"
#include "LLILCJit.h"

class ABISignature;
class ABICallSignature;
class ABIMethodSignature;

class FlowGraphNode : public llvm::BasicBlock {};

/// \brief Information associated with a Basic Block (aka Flow Graph Node)
///
/// This is used when processing MSIL to track the starting and ending
/// positions in the IL and the state of the operand stack.
struct FlowGraphNodeInfo {
public:
  /// Constructor
  FlowGraphNodeInfo() {
    StartMSILOffset = 0;
    EndMSILOffset = 0;
    IsVisited = false;
    TheReaderStack = nullptr;
    PropagatesOperandStack = true;
  };

  /// Byte Offset in the MSIL instruction stream to the first instruction
  /// in this basic block.
  uint32_t StartMSILOffset;

  /// Byte offset that is just past the last MSIL instruction of the Basic
  /// Block.
  uint32_t EndMSILOffset;

  /// In algorithms traversing the flow graph, used to track which basic blocks
  /// have been visited.
  bool IsVisited;

  /// Used to track what is on the operand stack on entry to the basic block.
  ReaderStack *TheReaderStack;

  /// true iff this basic block uses an operand stack and propagates it to the
  /// block's successors when it's not empty on exit.
  bool PropagatesOperandStack;
};

/// \brief Represent a node in the LLILC compiler intermediate representation.
///
/// The MSIL reader expects to use a type called IRNode to represent the
/// translation of the MSIL instructions. But the LLVM infrastructure uses
/// the llvm::Value type as the base class of its IR representation.
/// To reconcile these we make IRNode a class that merely derives from
/// llvm::Value.
class IRNode : public llvm::Value {};

/// \brief Abstract class representing a list of flow graph edges.
///
/// The list acts like an iterator which has a current position from which
/// source and sink nodes can be found.
class FlowGraphEdgeList {
public:
  /// Constructor
  FlowGraphEdgeList(){};

  /// Move the current location in the flow graph edge list to the next edge.
  virtual void moveNext() = 0;

  /// \return The sink (aka target or destination) node of the current edge.
  virtual FlowGraphNode *getSink() = 0;

  /// \return The source (aka From) node of the current edge.
  virtual FlowGraphNode *getSource() = 0;
};

/// \brief A list of predecessor edges of a given flow graph node.
///
/// This is used for iterating over the predecessors of a flow graph node.
/// After creating the predecessor edge list the getSource method is used
/// to get the first predecessor (if any). As long as the result of getSource()
/// is non-null, the moveNext() method may be used to advance to the next
/// predecessor edge. When getSource() returns null there are no more
/// predecessor edges (or predecessors).
/// \invariant The current edge iterator either points to a real edge or else
/// equals the end iterator meaning the list has been exhausted.
class FlowGraphPredecessorEdgeList : public FlowGraphEdgeList {
public:
  /// Construct a flow graph edge list for iterating over the predecessors
  /// of the \p Fg node.
  /// \param Fg The node whose predecessors are desired.
  /// \pre \p Fg != nullptr.
  /// \post **this** is a predecessor edge list representing the predecessors
  /// of \p Fg.
  FlowGraphPredecessorEdgeList(FlowGraphNode *Fg)
      : FlowGraphEdgeList(), PredIterator(Fg), PredIteratorEnd(Fg, true) {}

  /// Move the current location in the flow graph edge list to the next edge.
  /// \pre The current edge has not reached the end of the edge list.
  /// \post The current edge has been advanced to the next, or has possibly
  /// reached the end iterator (meaning no more predecessors).
  void moveNext() override { PredIterator++; }

  /// \return The sink of the current edge which will be \p Fg node.
  /// \pre The current edge has not reached the end of the edge list.
  FlowGraphNode *getSink() override {
    return (FlowGraphNode *)PredIterator.getUse().get();
  }

  /// \return The source of the current edge which will be one of the
  /// predecessors of the \p Fg node, unless the list has been exhausted in
  /// which case return nullptr.
  FlowGraphNode *getSource() override {
    return (PredIterator == PredIteratorEnd) ? nullptr
                                             : (FlowGraphNode *)*PredIterator;
  }

private:
  llvm::pred_iterator PredIterator;
  llvm::pred_iterator PredIteratorEnd;
};

/// \brief A list of successor edges of a given flow graph node.
///
/// This is used for iterating over the successors of a flow graph node.
/// After creating the successor edge list the getSink method is used
/// to get the first successor (if any). As long as the result of getSink()
/// is non-null, the moveNext() method may be used to advance to the next
/// successor edge. When getSink() returns null there are no more
/// successor edges (or successors).
/// \invariant The current edge iterator either points to a real edge or else
/// equals the end iterator meaning the list has been exhausted.
class FlowGraphSuccessorEdgeList : public FlowGraphEdgeList {
public:
  /// Construct a flow graph edge list for iterating over the successors
  /// of the \p Fg node.
  /// \param Fg The node whose successors are desired.
  /// \pre \p Fg != nullptr.
  /// \post **this** is a successor edge list representing the successors
  /// of \p Fg.
  FlowGraphSuccessorEdgeList(FlowGraphNode *Fg)
      : FlowGraphEdgeList(), SuccIterator(Fg->getTerminator()),
        SuccIteratorEnd(Fg->getTerminator(), true) {}

  /// Move the current location in the flow graph edge list to the next edge.
  /// \pre The current edge has not reached the end of the edge list.
  /// \post The current edge has been advanced to the next, or has possibly
  /// reached the end iterator (meaning no more successors).
  void moveNext() override { SuccIterator++; }

  /// \return The sink of the current edge which will be one of the successors
  /// of the \p Fg node, unless the list has been exhausted in which case
  /// return nullptr.
  FlowGraphNode *getSink() override {
    return (SuccIterator == SuccIteratorEnd) ? nullptr
                                             : (FlowGraphNode *)*SuccIterator;
  }

  /// \return The source of the current edge which will be \p Fg node.
  /// \pre The current edge has not reached the end of the edge list.
  FlowGraphNode *getSource() override {
    return (FlowGraphNode *)SuccIterator.getSource();
  }

private:
  llvm::succ_iterator SuccIterator;
  llvm::succ_iterator SuccIteratorEnd;
};

/// \brief A stack of IRNode pointers representing the MSIL operand stack.
///
/// The MSIL instruction set operates on a stack machine. Instructions
/// with operands may take them from the stack (if not some kind of
/// immediate) and the results of instruction are pushed on the operand stack.
/// The MSIL operands are translated by the reader into IRNodes.
/// The operand stack is represented by a stack of pointers to the
/// IRNodes for the operands.
///
/// This class completes the implementation of the ReaderStack base class.
class GenStack : public ReaderStack {
private:
  /// Owner of the operand stack.
  ReaderBase *Reader;

public:
  /// \brief Construct a GenStack with an initial capacity of \a MaxStack.
  ///
  /// \param MaxStack Suggested capacity for the stack. However the stack
  /// is allowed to grow larger than MaxStack as needed (possibly due to
  /// function inlining).
  /// \param Reader The ReaderBase that owns this operand stack. This is needed
  /// so that storage can be allocated from the lifetime of the reader.
  GenStack(uint32_t MaxStack, ReaderBase *Reader);

  /// \brief Pop the top element off the operand stack.
  ///
  /// \return The top element of the stack.
  /// \pre The stack is not empty
  /// \post The top element of the stack has been removed.
  IRNode *pop() override;

  /// \brief Push \p NewVal onto the operand stack.
  ///
  /// \param NewVal The value to be pushed.
  /// \pre NewVal != nullptr
  void push(IRNode *NewVal) override;

  /// \brief If the stack is not empty, cause an assertion failure.
  void assertEmpty() override;

#if !defined(NODEBUG)
  /// \brief Print the contents of the operand stack onto the debug output.
  void print() override;
#endif

  /// \brief Returns a copy of this operand stack.
  ReaderStack *copy() override;
};

class GenIR : public ReaderBase {
  friend class ABISignature;
  friend class ABICallSignature;
  friend class ABIMethodSignature;

public:
  GenIR(LLILCJitContext *JitContext,
        std::map<CORINFO_CLASS_HANDLE, llvm::Type *> *ClassTypeMap,
        std::map<llvm::Type *, CORINFO_CLASS_HANDLE> *ReverseClassTypeMap,
        std::map<CORINFO_CLASS_HANDLE, llvm::Type *> *BoxedTypeMap,
        std::map<std::tuple<CorInfoType, CORINFO_CLASS_HANDLE, uint32_t>,
                 llvm::Type *> *ArrayTypeMap,
        std::map<CORINFO_FIELD_HANDLE, uint32_t> *FieldIndexMap)
      : ReaderBase(JitContext->JitInfo, JitContext->MethodInfo,
                   JitContext->Flags),
        UnmanagedCallFrame(nullptr), ThreadPointer(nullptr) {
    this->JitContext = JitContext;
    this->ClassTypeMap = ClassTypeMap;
    this->ReverseClassTypeMap = ReverseClassTypeMap;
    this->BoxedTypeMap = BoxedTypeMap;
    this->ArrayTypeMap = ArrayTypeMap;
    this->FieldIndexMap = FieldIndexMap;
  }

  static bool isValidStackType(IRNode *Node);

  // ////////////////////////////////////////////////////////////////////
  //             IL generation methods supplied by the clients
  //
  // All pure virtual routines must be implemented by the client, non-pure
  // virtual routines have a default implementation in the reader, but can
  // be overloaded if necessary.
  // /////////////////////////////////////////////////////////////////////

  // MSIL Routines - client defined routines that are invoked by the reader.
  //                 One will be called for each msil opcode.

  uint32_t getPointerByteSize() override { return TargetPointerSizeInBits / 8; }

  void opcodeDebugPrint(uint8_t *Buf, unsigned StartOffset,
                        unsigned EndOffset) override {
    return;
  };

  // Used for testing, client can force verification.
  bool verForceVerification(void) override { return false; };

  bool abs(IRNode *Arg1, IRNode **RetVal) override;

  IRNode *argList() override;
  IRNode *instParam() override;

  IRNode *secretParam() override;
  IRNode *thisObj() override;

  void boolBranch(ReaderBaseNS::BoolBranchOpcode Opcode, IRNode *Arg1) override;

  IRNode *binaryOp(ReaderBaseNS::BinaryOpcode Opcode, IRNode *Arg1,
                   IRNode *Arg2) override;
  void branch() override;

  IRNode *call(ReaderBaseNS::CallOpcode Opcode, mdToken Token,
               mdToken ConstraintTypeRef, mdToken LoadFtnToken,
               bool HasReadOnlyPrefix, bool HasTailCallPrefix,
               bool IsUnmarkedTailCall, uint32_t CurrOffset,
               bool *RecursiveTailCall) override;

  IRNode *castOp(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *ObjRefNode,
                 CorInfoHelpFunc HelperId) override;

  IRNode *ckFinite(IRNode *Arg1) override {
    throw NotYetImplementedException("ckFinite");
  };
  IRNode *cmp(ReaderBaseNS::CmpOpcode Opode, IRNode *Arg1,
              IRNode *Arg2) override;
  void condBranch(ReaderBaseNS::CondBranchOpcode Opcode, IRNode *Arg1,
                  IRNode *Arg2) override;
  IRNode *conv(ReaderBaseNS::ConvOpcode Opcode, IRNode *Source) override;

  void dup(IRNode *Opr, IRNode **Result1, IRNode **Result2) override;
  void endFilter(IRNode *Arg1) override {
    throw NotYetImplementedException("endFilter");
  };

  FlowGraphNode *fgNodeGetNext(FlowGraphNode *FgNode) override;
  uint32_t fgNodeGetStartMSILOffset(FlowGraphNode *Fg) override;
  void fgNodeSetStartMSILOffset(FlowGraphNode *Fg, uint32_t Offset) override;
  uint32_t fgNodeGetEndMSILOffset(FlowGraphNode *Fg) override;
  void fgNodeSetEndMSILOffset(FlowGraphNode *FgNode, uint32_t Offset) override;

  bool fgNodeIsVisited(FlowGraphNode *FgNode) override;
  void fgNodeSetVisited(FlowGraphNode *FgNode, bool Visited) override;
  void fgNodeSetOperandStack(FlowGraphNode *Fg, ReaderStack *Stack) override;
  ReaderStack *fgNodeGetOperandStack(FlowGraphNode *Fg) override;

  IRNode *getStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken) override;

  void jmp(ReaderBaseNS::CallOpcode Opcode, mdToken Token, bool HasThis,
           bool HasVarArg) override {
    throw NotYetImplementedException("jmp");
  };

  virtual uint32_t updateLeaveOffset(uint32_t LeaveOffset, uint32_t NextOffset,
                                     FlowGraphNode *LeaveBlock,
                                     uint32_t TargetOffset) override;
  uint32_t updateLeaveOffset(EHRegion *Region, uint32_t LeaveOffset,
                             uint32_t NextOffset, FlowGraphNode *LeaveBlock,
                             uint32_t TargetOffset, bool &IsInHandler);
  void leave(uint32_t TargetOffset, bool IsNonLocal,
             bool EndsWithNonLocalGoto) override;
  IRNode *loadArg(uint32_t ArgOrdinal, bool IsJmp) override;
  IRNode *loadLocal(uint32_t ArgOrdinal) override;
  IRNode *loadArgAddress(uint32_t ArgOrdinal) override;
  IRNode *loadLocalAddress(uint32_t LocOrdinal) override;
  IRNode *loadConstantI4(int32_t Constant) override;
  IRNode *loadConstantI8(int64_t Constant) override;
  IRNode *loadConstantI(size_t Constant) override;
  IRNode *loadConstantR4(float Value) override;
  IRNode *loadConstantR8(double Value) override;
  IRNode *loadElem(ReaderBaseNS::LdElemOpcode Opcode,
                   CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Index,
                   IRNode *Array) override;
  IRNode *loadElemA(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Index,
                    IRNode *Array, bool IsReadOnly) override;
  IRNode *loadField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                    ReaderAlignType Alignment, bool IsVolatile) override;

  IRNode *loadNull() override;
  IRNode *localAlloc(IRNode *Arg, bool ZeroInit) override;
  IRNode *loadFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                           IRNode *Obj) override;

  IRNode *loadLen(IRNode *Array, bool ArrayMayBeNull = true) override;

  bool arrayAddress(CORINFO_SIG_INFO *Sig, IRNode **RetVal) override {
    throw NotYetImplementedException("arrayAddress");
  };
  IRNode *loadStringLen(IRNode *Arg1) override;

  IRNode *getTypeFromHandle(IRNode *HandleNode) override;

  IRNode *getValueFromRuntimeHandle(IRNode *Arg1) override {
    throw NotYetImplementedException("getValueFromRuntimeHandle");
  };
  IRNode *arrayGetDimLength(IRNode *Arg1, IRNode *Arg2,
                            CORINFO_CALL_INFO *CallInfo) override {
    throw NotYetImplementedException("arrayGetDimLength");
  };

  IRNode *loadAndBox(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Addr,
                     ReaderAlignType AlignmentPrefix) override {
    throw NotYetImplementedException("loadAndBox");
  };
  IRNode *convertHandle(IRNode *RuntimeTokenNode, CorInfoHelpFunc HelperID,
                        CORINFO_CLASS_HANDLE ClassHandle) override;
  void
  convertTypeHandleLookupHelperToIntrinsic(bool CanCompareToGetType) override {
    throw NotYetImplementedException(
        "convertTypeHandleLookupHelperToIntrinsic");
  };

  IRNode *loadStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                          bool IsVolatile) override;

  IRNode *stringLiteral(mdToken Token, void *StringHandle, InfoAccessType Iat);

  IRNode *loadStr(mdToken Token) override;

  IRNode *loadVirtFunc(IRNode *Arg1, CORINFO_RESOLVED_TOKEN *ResolvedToken,
                       CORINFO_CALL_INFO *CallInfo) override;

  IRNode *loadPrimitiveType(IRNode *Addr, CorInfoType CorInfoType,
                            ReaderAlignType Alignment, bool IsVolatile,
                            bool IsInterfConst,
                            bool AddressMayBeNull = true) override;

  IRNode *loadNonPrimitiveObj(IRNode *Addr, CORINFO_CLASS_HANDLE ClassHandle,
                              ReaderAlignType Alignment, bool IsVolatile,
                              bool AddressMayBeNull = true) override;

  IRNode *makeRefAny(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                     IRNode *Object) override {
    throw NotYetImplementedException("makeRefAny");
  };
  IRNode *newArr(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1) override;
  IRNode *newObj(mdToken Token, mdToken LoadFtnToken,
                 uint32_t CurrOffset) override;
  void pop(IRNode *Opr) override;
  IRNode *refAnyType(IRNode *Arg1) override {
    throw NotYetImplementedException("refAnyType");
  };

  void rethrow() override { throw NotYetImplementedException("rethrow"); };
  void returnOpcode(IRNode *Opr, bool IsSynchronizedMethod) override;
  IRNode *shift(ReaderBaseNS::ShiftOpcode Opcode, IRNode *ShiftAmount,
                IRNode *ShiftOperand) override;
  IRNode *sizeofOpcode(CORINFO_RESOLVED_TOKEN *ResolvedToken) override;
  void storeArg(uint32_t ArgOrdinal, IRNode *Arg1, ReaderAlignType Alignment,
                bool IsVolatile) override;
  void storeElem(ReaderBaseNS::StElemOpcode Opcode,
                 CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *ValueToStore,
                 IRNode *Index, IRNode *Array) override;

  void storeField(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg1,
                  IRNode *Arg2, ReaderAlignType Alignment,
                  bool IsVolatile) override;

  void storePrimitiveType(IRNode *Value, IRNode *Addr, CorInfoType CorInfoType,
                          ReaderAlignType Alignment, bool IsVolatile,
                          bool AddressMayBeNull = true) override;

  void storeLocal(uint32_t LocOrdinal, IRNode *Arg1, ReaderAlignType Alignment,
                  bool IsVolatile) override;
  void storeStaticField(CORINFO_RESOLVED_TOKEN *FieldToken,
                        IRNode *ValueToStore, bool IsVolatile) override;

  IRNode *stringGetChar(IRNode *Arg1, IRNode *Arg2) override;
  bool sqrt(IRNode *Argument, IRNode **Result) override;

  // The callTarget node is only required on IA64.
  bool interlockedIntrinsicBinOp(IRNode *Arg1, IRNode *Arg2, IRNode **RetVal,
                                 CorInfoIntrinsics IntrinsicID) override {
    throw NotYetImplementedException("interlockedIntrinsicBinOp");
  };

  bool interlockedCmpXchg(IRNode *Destination, IRNode *Exchange,
                          IRNode *Comparand, IRNode **Result,
                          CorInfoIntrinsics IntrinsicID) override;

  bool memoryBarrier() override;

  void switchOpcode(IRNode *Opr) override;

  void throwOpcode(IRNode *Arg1) override;
  IRNode *unaryOp(ReaderBaseNS::UnaryOpcode Opcode, IRNode *Arg1) override;
  IRNode *unbox(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg2,
                bool AndLoad, ReaderAlignType Alignment,
                bool IsVolatile) override;

  void nop() override;

  void insertIBCAnnotations() override;
  IRNode *insertIBCAnnotation(FlowGraphNode *Node, uint32_t Count,
                              uint32_t Offset) override {
    throw NotYetImplementedException("insertIBCAnnotation");
  };

  //
  // REQUIRED Client Helper Routines.
  //

  // Base calls to alert client it needs a security check
  void methodNeedsSecurityCheck() override { NeedsSecurityObject = true; }

  // Base calls to alert client it needs keep generics context alive
  void
  methodNeedsToKeepAliveGenericsContext(bool KeepGenericsCtxtAlive) override;

  // Called to instantiate an empty reader stack.
  ReaderStack *createStack() override;

  // Called when reader begins processing method.
  void readerPrePass(uint8_t *Buf, uint32_t NumBytes) override;

  // Called between building the flow graph and inserting the IR
  void readerMiddlePass(void) override;

  // Called when reader has finished processing method.
  void readerPostPass(bool IsImportOnly) override;

  // Called at the start of block processing
  void beginFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrOffset,
                          bool IsVerifyOnly) override;
  // Called at the end of block processing.
  void endFlowGraphNode(FlowGraphNode *Fg, uint32_t CurrOffset) override;

  // Used to maintain operand stack.
  void maintainOperandStack(FlowGraphNode *CurrentBlock) override;
  void assignToSuccessorStackNode(FlowGraphNode *, IRNode *Dst, IRNode *Src,

                                  bool *IsMultiByteAssign) override {
    throw NotYetImplementedException("assignToSuccessorStackNode");
  };
  bool typesCompatible(IRNode *Src1, IRNode *Src2) override {
    throw NotYetImplementedException("typesCompatible");
  };

  void removeStackInterference() override;

  void removeStackInterferenceForLocalStore(uint32_t Opcode,
                                            uint32_t Ordinal) override;

  // Remove all IRNodes from block (for verification error processing.)
  void clearCurrentBlock() override {
    throw NotYetImplementedException("clearCurrentBlock");
  };

  // Notify client of alignment problem
  void verifyStaticAlignment(void *Pointer, CorInfoType CorType,
                             unsigned MinClassAlign) override;

  // Allocate temporary (Reader lifetime) memory
  void *getTempMemory(size_t NumBytes) override;
  // Allocate procedure-lifetime memory
  void *getProcMemory(size_t NumBytes) override;

  EHRegion *rgnAllocateRegion() override;
  EHRegionList *rgnAllocateRegionList() override;

  //
  // REQUIRED Flow and Region Graph Manipulation Routines
  //
  FlowGraphNode *fgPrePhase(FlowGraphNode *Fg) override;
  void fgPostPhase(void) override;
  FlowGraphNode *fgGetHeadBlock(void) override;
  FlowGraphNode *fgGetTailBlock(void) override;
  FlowGraphNode *fgNodeGetIDom(FlowGraphNode *Fg) override;

  IRNode *fgNodeFindStartLabel(FlowGraphNode *Block) override;

  BranchList *fgGetLabelBranchList(IRNode *LabelNode) override {
    throw NotYetImplementedException("fgGetLabelBranchList");
  };

  void insertHandlerAnnotation(EHRegion *HandlerRegion) override {
    throw NotYetImplementedException("insertHandlerAnnotation");
  };
  void insertRegionAnnotation(IRNode *RegionStartNode,
                              IRNode *RegionEndNode) override {
    throw NotYetImplementedException("insertRegionAnnotation");
  };
  void fgAddLabelToBranchList(IRNode *LabelNode, IRNode *BranchNode) override;
  void fgAddArc(IRNode *BranchNode, FlowGraphNode *Source,
                FlowGraphNode *Sink) override {
    throw NotYetImplementedException("fgAddArc");
  };
  bool fgBlockHasFallThrough(FlowGraphNode *Block) override;

  void fgDeleteBlock(FlowGraphNode *Block) override;
  void fgDeleteEdge(FlowGraphEdgeList *Arc) override {
    throw NotYetImplementedException("fgDeleteEdge");
  };
  void fgDeleteNodesFromBlock(FlowGraphNode *Block) override;
  IRNode *fgNodeGetEndInsertIRNode(FlowGraphNode *FgNode) override;

  bool commonTailCallChecks(CORINFO_METHOD_HANDLE DeclaredMethod,
                            CORINFO_METHOD_HANDLE ExactMethod,
                            bool IsUnmarkedTailCall, bool SuppressMsgs);

  // Returns true iff client considers the JMP recursive and wants a
  // loop back-edge rather than a forward edge to the exit label.
  bool fgOptRecurse(mdToken Token) override;

  // Returns true iff client considers the CALL/JMP recursive and wants a
  // loop back-edge rather than a forward edge to the exit label.
  bool fgOptRecurse(ReaderCallTargetData *Data) override;

  // Returns true if node (the start of a new eh Region) cannot be the start of
  // a block.
  bool fgEHRegionStartRequiresBlockSplit(IRNode *Node) override {
    throw NotYetImplementedException("fgEHRegionStartRequiresBlockSplit");
  };

  bool fgIsExceptRegionStartNode(IRNode *Node) override {
    throw NotYetImplementedException("fgIsExceptRegionStartNode");
  };
  FlowGraphNode *fgSplitBlock(FlowGraphNode *Block, IRNode *Node) override;
  void fgSetBlockToRegion(FlowGraphNode *Block, EHRegion *Region,
                          uint32_t LastOffset) override {
    throw NotYetImplementedException("fgSetBlockToRegion");
  };
  IRNode *fgMakeBranch(IRNode *LabelNode, IRNode *InsertNode,
                       uint32_t CurrentOffset, bool IsConditional,
                       bool IsNominal) override;
  IRNode *fgMakeEndFinally(IRNode *InsertNode, EHRegion *FinallyRegion,
                           uint32_t CurrentOffset) override;

  // turns an unconditional branch to the entry label into a fall-through
  // or a branch to the exit label, depending on whether it was a recursive
  // jmp or tail.call.
  void fgRevertRecursiveBranch(IRNode *BranchNode) override {
    throw NotYetImplementedException("fgRevertRecursiveBranch");
  };

  IRNode *fgMakeSwitch(IRNode *DefaultLabel, IRNode *Insert) override;

  IRNode *fgMakeThrow(IRNode *Insert) override;
  IRNode *fgAddCaseToCaseList(IRNode *SwitchNode, IRNode *LabelNode,
                              unsigned Element) override;

  void insertEHAnnotationNode(IRNode *InsertionPointNode,
                              IRNode *InsertNode) override {
    throw NotYetImplementedException("insertEHAnnotationNode");
  };
  FlowGraphNode *makeFlowGraphNode(uint32_t TargetOffset,
                                   FlowGraphNode *PreviousNode,
                                   EHRegion *Region) override;
  void markAsEHLabel(IRNode *LabelNode) override {
    throw NotYetImplementedException("markAsEHLabel");
  };
  IRNode *makeTryEndNode(void) override {
    throw NotYetImplementedException("makeTryEndNode");
  };
  IRNode *makeRegionStartNode(ReaderBaseNS::RegionKind RegionType) override {
    throw NotYetImplementedException("makeRegionStartNode");
  };
  IRNode *makeRegionEndNode(ReaderBaseNS::RegionKind RegionType) override {
    throw NotYetImplementedException("makeRegionEndNode");
  };

  // Allow client to override reader's decision to optimize castclass/isinst
  bool disableCastClassOptimization();

  // Hook to permit client to record call information returns true if the call
  // is a recursive tail
  // call and thus should be turned into a loop
  bool fgCall(ReaderBaseNS::OPCODE Opcode, mdToken Token,
              mdToken ConstraintToken, unsigned MsilOffset, IRNode *Block,
              bool CanInline, bool IsTailCall, bool IsUnmarkedTailCall,
              bool IsReadOnly) override;

  // Replace all uses of oldNode in the IR with newNode and delete oldNode.
  void replaceFlowGraphNodeUses(FlowGraphNode *OldNode,
                                FlowGraphNode *NewNode) override;

  IRNode *findBlockSplitPointAfterNode(IRNode *Node) override;
  IRNode *exitLabel(void) override {
    throw NotYetImplementedException("exitLabel");
  };
  IRNode *entryLabel(void) override {
    throw NotYetImplementedException("entryLabel");
  };

  // Function is passed a try region, and is expected to return the first label
  // or instruction
  // after the region.
  IRNode *findTryRegionEndOfClauses(EHRegion *TryRegion) override {
    throw NotYetImplementedException("findTryRegionEndOfClauses");
  };

  bool isCall() override { throw NotYetImplementedException("isCall"); };
  bool isRegionStartBlock(FlowGraphNode *Fg) override {
    throw NotYetImplementedException("isRegionStartBlock");
  };
  bool isRegionEndBlock(FlowGraphNode *Fg) override {
    throw NotYetImplementedException("isRegionEndBlock");
  };

  // Create a symbol node that will be used to represent the stack-incoming
  // exception object
  // upon entry to funclets.
  IRNode *makeExceptionObject() override {
    throw NotYetImplementedException("makeExceptionObject");
  };

  // //////////////////////////////////////////////////////////////////////////
  // Client Supplied Helper Routines, required by VOS support
  // //////////////////////////////////////////////////////////////////////////

  // Asks GenIR to make operand value accessible by address, and return a node
  // that references the incoming operand by address.
  IRNode *addressOfLeaf(IRNode *Leaf) override;
  IRNode *addressOfValue(IRNode *Leaf) override;

  IRNode *genNewMDArrayCall(ReaderCallTargetData *CallTargetData,
                            std::vector<IRNode *> Args,
                            IRNode **CallNode) override;

  IRNode *genNewObjThisArg(ReaderCallTargetData *CallTargetData,
                           CorInfoType COrType,
                           CORINFO_CLASS_HANDLE Class) override;

  IRNode *genNewObjReturnNode(ReaderCallTargetData *CalLTargetData,
                              IRNode *ThisArg) override;

  // Helper callback used by rdrCall to emit call code.
  IRNode *genCall(ReaderCallTargetData *CallTargetInfo, bool MayThrow,
                  std::vector<IRNode *> Args, IRNode **CallNode) override;

  bool canMakeDirectCall(ReaderCallTargetData *CallTargetData) override;

  // Generate call to helper
  IRNode *callHelper(CorInfoHelpFunc HelperID, bool MayThrow, IRNode *Dst,
                     IRNode *Arg1 = nullptr, IRNode *Arg2 = nullptr,
                     IRNode *Arg3 = nullptr, IRNode *Arg4 = nullptr,
                     ReaderAlignType Alignment = Reader_AlignUnknown,
                     bool IsVolatile = false, bool NoCtor = false,
                     bool CanMoveUp = false) override;

  // Generate call to helper
  llvm::CallSite callHelperImpl(CorInfoHelpFunc HelperID, bool MayThrow,
                                llvm::Type *ReturnType, IRNode *Arg1 = nullptr,
                                IRNode *Arg2 = nullptr, IRNode *Arg3 = nullptr,
                                IRNode *Arg4 = nullptr,
                                ReaderAlignType Alignment = Reader_AlignUnknown,
                                bool IsVolatile = false, bool NoCtor = false,
                                bool CanMoveUp = false);

  /// Generate special generics helper that might need to insert flow. The
  /// helper is called if NullCheckArg is null at compile-time or if it
  /// evaluates to null at run-time.
  ///
  /// \param Helper Id of the call helper.
  /// \param Arg1 First helper argument.
  /// \param Arg2 Second helper argument.
  /// \param NullCheckArg Argument to check for null.
  /// \returns Generated call instruction if NullCheckArg is null; otherwise,
  /// PHI of NullCheckArg and the generated call instruction.
  IRNode *callRuntimeHandleHelper(CorInfoHelpFunc Helper, IRNode *Arg1,
                                  IRNode *Arg2, IRNode *NullCheckArg);

  /// Generate a helper call to enter or exit a monitor used by synchronized
  /// methods.
  ///
  /// \param IsEnter true if the monitor should be entered; false if the monitor
  /// should be exited.
  void callMonitorHelper(bool IsEnter);

  IRNode *convertToBoxHelperArgumentType(IRNode *Opr,
                                         CorInfoType CorType) override;

  IRNode *makeBoxDstOperand(CORINFO_CLASS_HANDLE Class) override;

  IRNode *genNullCheck(IRNode *Node) override;

  void
  createSym(uint32_t Num, bool IsAuto, CorInfoType CorType,
            CORINFO_CLASS_HANDLE Class, bool IsPinned,
            ReaderSpecialSymbolType SymType = Reader_NotSpecialSymbol) override;

  IRNode *derefAddress(IRNode *Address, bool DstIsGCPtr, bool IsConst,
                       bool AddressMayBeNull = true) override;

  IRNode *conditionalDerefAddress(IRNode *Address) override;

  IRNode *getHelperCallAddress(CorInfoHelpFunc HelperId) override;

  IRNode *handleToIRNode(mdToken Token, void *EmbedHandle, void *RealHandle,
                         bool IsIndirect, bool IsReadOnly, bool IsRelocatable,
                         bool IsCallTarget,
                         bool IsFrozenObject = false) override;

  // Create an operand that will be used to hold a pointer.
  IRNode *makePtrDstGCOperand(bool IsInteriorGC) override {
    return makePtrNode(IsInteriorGC ? Reader_PtrGcInterior : Reader_PtrGcBase);
  }

  IRNode *makePtrNode(ReaderPtrType PtrType = Reader_PtrNotGc) override;

  IRNode *makeStackTypeNode(IRNode *Node) override {
    throw NotYetImplementedException("makeStackTypeNode");
  };

  IRNode *makeDirectCallTargetNode(void *CodeAddr) override;

  // Called once region tree has been built.
  void setEHInfo(EHRegion *EhRegionTree, EHRegionList *EhRegionList) override;

  void setSequencePoint(uint32_t, ICorDebugInfo::SourceTypes) override {
    throw NotYetImplementedException("setSequencePoint");
  };
  bool needSequencePoints() override;

#if !defined(CC_PEVERIFY)
  //
  // Helper functions for calls
  //

  // vararg
  void canonVarargsCall(IRNode *CallNode,
                        ReaderCallTargetData *CallTargetInfo) {
    throw NotYetImplementedException("canonVarargsCall");
  };

  // stubs
  IRNode *canonStubCall(IRNode *CallNode, ReaderCallTargetData *CallTargetData);
#endif

  // Used to expand multidimensional array access intrinsics
  bool arrayGet(CORINFO_SIG_INFO *Sig, IRNode **RetVal) override {
    throw NotYetImplementedException("arrayGet");
  };
  bool arraySet(CORINFO_SIG_INFO *Sig) override {
    throw NotYetImplementedException("arraySet");
  };

#if !defined(NDEBUG)
  void dbDumpFunction(void) override {
    throw NotYetImplementedException("dbDumpFunction");
  };
  void dbPrintIRNode(IRNode *Instr) override { Instr->dump(); };
  void dbPrintFGNode(FlowGraphNode *Fg) override {
    throw NotYetImplementedException("dbPrintFGNode");
  };
  void dbPrintEHRegion(EHRegion *Eh) override {
    throw NotYetImplementedException("dbPrintEHRegion");
  };
  uint32_t dbGetFuncHash(void) override {
    throw NotYetImplementedException("dbGetFuncHash");
  };
#endif

private:
  llvm::Type *getType(CorInfoType Type, CORINFO_CLASS_HANDLE ClassHandle,
                      bool GetRefClassFields = true);

  llvm::Type *getClassType(CORINFO_CLASS_HANDLE ClassHandle, bool IsRefClass,
                           bool GetRefClassFields);

  /// \brief Construct the LLVM type of the boxed representation of the given
  ///        value type.
  ///
  /// \param Class The handle to the value type's class.
  /// \returns The LLVM type of the boxed representation of the value type.
  llvm::Type *getBoxedType(CORINFO_CLASS_HANDLE Class);

  /// Convert node to the desired type.
  /// May reinterpret, truncate, or extend as needed.
  /// \param Type Desired type
  /// \param Node Value to be converted
  /// \param IsSigned Perform sign extension if necessary, otherwise
  /// integral values are zero extended
  IRNode *convert(llvm::Type *Type, llvm::Value *Node, bool IsSigned);

  llvm::Type *binaryOpType(llvm::Type *Type1, llvm::Type *Type2);

  IRNode *genPointerAdd(IRNode *Arg1, IRNode *Arg2);
  IRNode *genPointerSub(IRNode *Arg1, IRNode *Arg2);
  IRNode *getFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                          CORINFO_FIELD_INFO *FieldInfo, IRNode *Obj,
                          bool MustNullCheck);

  IRNode *simpleFieldAddress(IRNode *BaseAddress,
                             CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             CORINFO_FIELD_INFO *FieldInfo) override;

  /// Get a node with the same value as Addr but typed as a pointer to the type
  /// corresponding to CorInfoType and ClassHandle.
  ///
  /// \param Addr Address to change the type on.
  /// \param  CorInfoType Type that Addr should point to.
  /// \param ClassHandle Class handle corresponding to CorInfoType.
  /// \param ReaderAlignment Reader alignment of the Addr access.
  /// \param Alignment [out] Converted alignment corresponding to
  /// ReaderAlignment.
  /// \returns Address pointing to a value of the specified type.
  IRNode *getTypedAddress(IRNode *Addr, CorInfoType CorInfoType,
                          CORINFO_CLASS_HANDLE ClassHandle,
                          ReaderAlignType ReaderAlignment, uint32_t *Alignment);
  /// Generate instructions for loading value of the specified type at the
  /// specified address.
  ///
  /// \param Address Address to load from.
  /// \param Ty llvm type of the value to load.
  /// \param CorType CorInfoType of the value to load.
  /// \param ResolvedToken Resolved token corresponding to the type of the value
  /// to load.
  /// \param AlignmentPrefix Alignment of the value.
  /// \param IsVolatile true iff the load is volatile.
  /// \param AddressMayBeNull true iff the address may be null.
  /// \returns Value at the specified address.
  IRNode *loadAtAddress(IRNode *Address, llvm::Type *Ty, CorInfoType CorType,
                        CORINFO_RESOLVED_TOKEN *ResolvedToken,
                        ReaderAlignType AlignmentPrefix, bool IsVolatile,
                        bool AddressMayBeNull = true);

  IRNode *loadAtAddressNonNull(IRNode *Address, llvm::Type *Ty,
                               CorInfoType CorType,
                               CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               ReaderAlignType AlignmentPrefix,
                               bool IsVolatile) {
    return loadAtAddress(Address, Ty, CorType, ResolvedToken, AlignmentPrefix,
                         IsVolatile, false);
  }

  /// Generate instructions for storing value of the specified type at the
  /// specified address.
  ///
  /// \param Address Address to store to.
  /// \param ValueToStore Value to store.
  /// \param Ty llvm type of the value to store.
  /// \param ResolvedToken Resolved token corresponding to the type of the value
  /// to store.
  /// \param AlignmentPrefix Alignment of the value.
  /// \param IsVolatile true iff the store is volatile.
  /// \param IsField true iff this is a field address.
  /// \param AddressMayBeNull true iff the address may be null.
  void storeAtAddress(IRNode *Address, IRNode *ValueToStore, llvm::Type *Ty,
                      CORINFO_RESOLVED_TOKEN *ResolvedToken,
                      ReaderAlignType AlignmentPrefix, bool IsVolatile,
                      bool IsField, bool AddressMayBeNull);

  void storeAtAddressNonNull(IRNode *Address, IRNode *ValueToStore,
                             llvm::Type *Ty,
                             CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             ReaderAlignType AlignmentPrefix, bool IsVolatile,
                             bool IsField) {
    return storeAtAddress(Address, ValueToStore, Ty, ResolvedToken,
                          AlignmentPrefix, IsVolatile, IsField, false);
  }

  void classifyCmpType(llvm::Type *Ty, uint32_t &Size, bool &IsPointer,
                       bool &IsFloat);

  /// \brief Create a basic block for use when expanding a single MSIL
  /// instruction.
  ///
  /// Use this method to create a block when expanding a single MSIL
  /// instruction into an instruction sequence with control flow. Give the
  /// block the given MSIL offset at both begin and end since it represents
  /// code at a single point in the IL stream. Mark the block as not
  /// contributing an operand stack to any subsequent join.
  ///
  /// \param PointOffset       MSIL offset the block starts and ends at.
  /// \param BlockName         Optional name for the new block.
  /// \returns                 Newly allocated block.
  llvm::BasicBlock *createPointBlock(uint32_t PointOffset,
                                     const llvm::Twine &BlockName = "");

  /// \brief Create a basic block for use when expanding a single MSIL
  /// instruction.
  ///
  /// Use this method to create a block when expanding a single MSIL
  /// instruction into an instruction sequence with control flow. Give the
  /// block the current MSIL offset at both begin and end since it represents
  /// code at a single point in the IL stream. Mark the block as not
  /// contributing an operand stack to any subsequent join.
  ///
  /// \param BlockName         Optional name for the new block.
  /// \returns                 Newly allocated block.
  llvm::BasicBlock *createPointBlock(const llvm::Twine &BlockName = "") {
    return createPointBlock(CurrInstrOffset, BlockName);
  }

  /// \brief Insert a conditional branch to a point block.
  ///
  /// Split the current block after the current instruction, creating a
  /// continuation block. Insert a conditional branch to \p PointBlock if
  /// \p Condition is true. Insert an unconditional branch into \p PointBlock
  /// to the contiuation if \p Rejoin is true. Leave the insertion point in
  /// the continuation at the same logical point it was before the split.
  ///
  /// \param Condition       Predicate condition to use in the branch.
  /// \param PointBlock      Block to branch to when \p Condition is true. If
  ///                        \p Rejoin is true, \p PointBlock must not have a
  ///                        terminator, and this method will add one. If \p
  ///                        Rejoin is false, \p PointBlock is left unmodified.
  /// \param Rejoin          If true, insert a branch into \p PointBlock back
  ///                        to the continuation.
  ///
  /// \returns               The continuation block. Insertion point is left
  ///                        within this block at the instruction after the
  ///                        original split point.
  llvm::BasicBlock *insertConditionalPointBlock(llvm::Value *Condition,
                                                llvm::BasicBlock *PointBlock,
                                                bool Rejoin);

  /// Split the block at the current insertion point.
  ///
  /// \param Goto [out] If \p Goto is non-null, \p *Goto will be set to the
  ///                   terminator connecting the old block to the new one.
  ///
  /// \returns          The newly-created successor block
  llvm::BasicBlock *splitCurrentBlock(llvm::TerminatorInst **Goto = nullptr);

  /// \brief Move point blocks preceding \p OldBlock to just before \p NewBlock
  ///
  /// Point blocks created during the first pass flow-graph construction are
  /// inserted before temp blocks created for their point.  Then they are moved
  /// to the appropriate spot when branches to temp blocks are being rewritten.
  /// This is the routine invoked during branch rewriting to move them.
  ///
  /// \param OldBlock Temporary block whose associated point blocks are to be
  ///                 fixed up
  /// \param NewBlock "Real" block that begins at the point in question
  void movePointBlocks(llvm::BasicBlock *OldBlock, llvm::BasicBlock *NewBlock);

  /// Insert one instruction in place of another.
  ///
  /// \param OldInstruction The instruction to be removed.  Must have no uses.
  /// \param NewInstruction The instruction to insert where \p OldInstruction
  ///                       was.
  void replaceInstruction(llvm::Instruction *OldInstruction,
                          llvm::Instruction *NewInstruction);

  /// \brief Insert a PHI to merge two values
  ///
  /// Create a \p PHINode in \p JoinBlock to merge \p Arg1 and \p Arg2.
  /// Insert the new PHINode after any existing PHINodes in the block.
  /// Return the new PHINode. Insertion point is unaffected.
  ///
  /// \param JoinBlock            Block to insert the PHI node.
  /// \param Arg1                 First value to join.
  /// \param Block1               Predecessor block for \p JoinBlock that
  ///                             brings in \p Arg1.
  /// \param Arg2                 Second value to join.
  /// \param Block2               Predecessor block for \p JoinBlock that
  ///                             brings in \p Arg2.
  /// \param NameStr              Optional name for the resulting PHI.
  ///
  /// \returns                    The PHINode that was inserted.
  llvm::PHINode *mergeConditionalResults(llvm::BasicBlock *JoinBlock,
                                         llvm::Value *Arg1,
                                         llvm::BasicBlock *Block1,
                                         llvm::Value *Arg2,
                                         llvm::BasicBlock *Block2,
                                         const llvm::Twine &NameStr = "");

  /// Generate a call to the helper if the condition is met.
  ///
  /// \param Condition Condition that will trigger the call.
  /// \param HelperId Id of the call helper.
  /// \param MayThrow true if the helper call may raise an exception.
  /// \param ReturnType Return type of the call helper.
  /// \param Arg1 First helper argument.
  /// \param Arg2 Second helper argument.
  /// \param CallReturns true iff the helper call returns.
  /// \param CallBlockName Name of the basic block that will contain the call.
  /// \returns Generated call/invoke instruction.
  llvm::CallSite genConditionalHelperCall(llvm::Value *Condition,
                                          CorInfoHelpFunc HelperId,
                                          bool MayThrow, llvm::Type *ReturnType,
                                          IRNode *Arg1, IRNode *Arg2,
                                          bool CallReturns,
                                          const llvm::Twine &CallBlockName);

  /// Generate a call to the throw helper if the condition is met.
  ///
  /// \param Condition Condition that will trigger the throw.
  /// \param HelperId Id of the throw-helper.
  /// \param ThrowBlockName Name of the basic block that will contain the throw.
  void genConditionalThrow(llvm::Value *Condition, CorInfoHelpFunc HelperId,
                           const llvm::Twine &ThrowBlockName);

  /// Generate array bounds check.
  ///
  /// \param Array Array to be accessed.
  /// \param Index Index to be accessed.
  /// \returns The input array.
  IRNode *genBoundsCheck(IRNode *Array, IRNode *Index);

  /// \brief Generate conditional throw for conv.ovf.
  ///
  /// As part of the overflow test sequence, this method may generate code that
  /// produces an intermediate result (specifically, the overflow tests for a
  /// conversion from floating-point to narrow int produces an intermediate
  /// wide int).  The value returned is the value to convert, and
  /// \p SourceIsSigned will be updated to reflect the new source if necessary.
  ///
  /// \param Source         Source of the conv.ovf
  /// \param TargetTy       LLVM type being converted to
  /// \param SourceIsSigned [in/out] Indicates whether an integer source should
  ///                       be considered signed; meaningless for non-integer
  ///                       sources
  /// \param DestIsSigned   Indicates whether the target type should be
  ///                       interpreted as signed
  /// \returns The value to convert to the destination.  May be the original
  ///          \p Source or may be a new intermediate value of another type.
  llvm::Value *genConvertOverflowCheck(llvm::Value *Source,
                                       llvm::IntegerType *TargetTy,
                                       bool &SourceIsSigned, bool DestIsSigned);

  uint32_t size(CorInfoType CorType);
  uint32_t stackSize(CorInfoType CorType);
  static bool isSigned(CorInfoType CorType);
  llvm::Type *getStackType(CorInfoType CorType);

  /// Convert a result to a valid stack type,
  /// generally by either reinterpretation or extension.
  /// \param Node Value to be converted
  /// \param CorType additional information needed to determine if
  /// Node's type is signed or unsigned
  IRNode *convertToStackType(IRNode *Node, CorInfoType CorType);

  /// Convert a result from a stack type to the desired type,
  /// generally by either reinterpretation or truncation.
  /// \param Node Value to be converted
  /// \param CorType additional information needed to determine if
  /// ResultTy is signed or unsigned
  /// \param ResultTy - Desired type
  IRNode *convertFromStackType(IRNode *Node, CorInfoType CorType,
                               llvm::Type *ResultTy);

  /// Get the type of the result of the merge of two values from operand stacks
  /// of a block's predecessors. The allowed combinations are nativeint and
  // int32 (resulting in nativeint), float and double (resulting in double),
  /// and GC pointers (resulting in the closest common supertype).
  ///
  /// \param Ty1 Type of the first value.
  /// \param Ty1 Type of the second value.
  /// \returns The result type.
  llvm::Type *getStackMergeType(llvm::Type *Ty1, llvm::Type *Ty2);

  bool objIsThis(IRNode *Obj);

  /// Create a new temporary variable that can be
  /// used anywhere within the method.
  ///
  /// \param Ty Type for the new variable.
  /// \param Name Optional name for the new variable.
  /// \returns Instruction establishing the variable's location.
  llvm::Instruction *createTemporary(llvm::Type *Ty,
                                     const llvm::Twine &Name = "");

  IRNode *loadManagedAddress(llvm::Value *UnmanagedAddresses);

  llvm::PointerType *getManagedPointerType(llvm::Type *ElementType);

  llvm::PointerType *getUnmanagedPointerType(llvm::Type *ElementType);

  bool isManagedType(llvm::Type *Type);
  bool isManagedPointerType(llvm::Type *Type);
  bool isManagedAggregateType(llvm::Type *Type);

  /// \brief Check whether Type is an unmanaged pointer type.
  ///
  /// \param Type Type to check.
  /// \returns true iff \p Type is an unmanaged pointer type.
  bool isUnmanagedPointerType(llvm::Type *Type);

  llvm::StoreInst *makeStore(llvm::Value *ValueToStore, llvm::Value *Address,
                             bool IsVolatile, bool AddressMayBeNull = true);
  llvm::StoreInst *makeStoreNonNull(llvm::Value *ValueToStore,
                                    llvm::Value *Address, bool IsVolatile) {
    return makeStore(ValueToStore, Address, IsVolatile, false);
  }

  llvm::LoadInst *makeLoad(llvm::Value *Address, bool IsVolatile,
                           bool AddressMayBeNull = true);
  llvm::LoadInst *makeLoadNonNull(llvm::Value *Address, bool IsVolatile) {
    return makeLoad(Address, IsVolatile, false);
  }

  /// \brief Create a call or invoke instruction
  ///
  /// The call is inserted at the LLVMBuilder's current insertion point.
  ///
  /// \param Callee    Target of the call
  /// \param MayThrow  True if the callee may raise an exception
  /// \param Args      Arguments to pass to the callee
  /// \returns         A \p CallSite wrapping the CallInst or InvokeInst
  llvm::CallSite makeCall(llvm::Value *Callee, bool MayThrow,
                          llvm::ArrayRef<llvm::Value *> Args);

  /// Store a value to an argument passed indirectly.
  ///
  /// The storage backing such arguments may be located on the heap; any stores
  /// to these locations may need write barriers.
  ///
  /// \param ValueArgType  EE type info for the value to store.
  /// \param ValueToStore  The value to store.
  /// \param Address       The address to store to (i.e. the indirect argument).
  /// \param IsVolatile    Indicates whether or not the store is volatile.
  void storeIndirectArg(const CallArgType &ValueArgType,
                        llvm::Value *ValueToStore, llvm::Value *Address,
                        bool IsVolatile);

  /// Get address of the array element.
  ///
  /// \param Array Array that the element belongs to.
  /// \param Index Index of the element.
  /// \param ElementTy Type of the element.
  /// \returns Node representing the address of the element.
  IRNode *genArrayElemAddress(IRNode *Array, IRNode *Index,
                              llvm::Type *ElementTy);

  /// Convert ReaderAlignType to byte alighnment to byte alignment.
  ///
  /// \param ReaderAlignment Reader alignment.
  /// \returns Alignment in bytes.
  uint32_t convertReaderAlignment(ReaderAlignType ReaderAlignment);

  /// Get array element type.
  ///
  /// \param Array Array node.
  /// \param ResolvedToken Resolved token from ldelem or stelem instruction.
  /// \param CorInfoType [IN/OUT] Type of the element (will be resolved for
  /// CORINFO_TYPE_UNDEF).
  /// \param Alignment - [IN/OUT] Alignment that will be updated for value
  /// classes.
  /// \returns Array element type.
  llvm::Type *getArrayElementType(IRNode *Array,
                                  CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                  CorInfoType *CorType,
                                  ReaderAlignType *Alignment);

  /// Create a PHI node in a block that may or may not have a terminator.
  ///
  /// \param Block Block to create the PHI node in.
  /// \param Ty Type of the PHI values.
  /// \param NumReservedValues Hint for the number of incoming edges that this
  //  PHI node will have.
  /// \param NameStr Name of the PHI node.
  /// \returns Created PHI node.
  llvm::PHINode *createPHINode(llvm::BasicBlock *Block, llvm::Type *Ty,
                               unsigned int NumReservedValues,
                               const llvm::Twine &NameStr);

  /// Add a new operand to the PHI instruction. The type of the new operand may
  /// or may not equal to the type of the PHI instruction. Adjust the types as
  /// necessary.
  ///
  /// \param PHI PHI instruction.
  /// \param NewOperand Operand to add to the PHI instruction.
  void AddPHIOperand(llvm::PHINode *PHI, llvm::Value *NewOperand,
                     llvm::BasicBlock *NewBlock);

  /// Change the type of a PHI instruction operand as a result of a stack merge.
  ///
  /// \param Operand Operand to change the type on.
  /// \param OperandBlock Basic block corresponding to the operand.
  /// \param NewTy New type of the operand.
  /// \returns Operand with the changed type.
  llvm::Value *ChangePHIOperandType(llvm::Value *Operand,
                                    llvm::BasicBlock *OperandBlock,
                                    llvm::Type *NewTy);

  bool fgNodePropagatesOperandStack(FlowGraphNode *Fg) override;

  /// Set whether this node propagates operand stack.
  ///
  /// \param Fg Flow graph node.
  /// \param PropagatesOperandStack true iff this flow graph node propagates
  /// operand stack.
  void fgNodeSetPropagatesOperandStack(FlowGraphNode *Fg,
                                       bool PropagatesOperandStack);

  /// Check whether the node is constant null.
  ///
  /// \param Node Node to check.
  /// \returns true iff this node is constant null.
  bool isConstantNull(IRNode *Node);

  /// \brief Insert IR to keep the generic context alive
  void insertIRToKeepGenericContextAlive();

  /// \brief Insert IR to setup the security object
  void insertIRForSecurityObject();

  /// \brief Insert IR to setup the unmanaged call frame (PInvoke frame) and
  ///        the thread pointer.
  void insertIRForUnmanagedCallFrame();

  /// \brief Create the @gc.safepoint_poll() method
  /// Creates the @gc.safepoint_poll() method and insertes it into the
  /// current module. This helper is required by the LLVM GC-Statepoint
  /// insertion phase.
  void createSafepointPoll();

  /// \brief Override of doTailCallOpt method
  /// Provides client specific Options look up.
  bool doTailCallOpt() override;

  /// If isZeroInitLocals() returns true, zero intitialize all locals;
  /// otherwise, zero initialize all gc pointers and structs with gc pointers.
  void zeroInitLocals();

  /// Zero initialize the block.
  ///
  /// \param Address Address of the block.
  /// \param Size Size of the block.
  void zeroInitBlock(llvm::Value *Address, uint64_t Size);

  /// Zero initialize the block.
  ///
  /// \param Address Address of the block.
  /// \param Size Size of the block.
  void zeroInitBlock(llvm::Value *Address, llvm::Value *Size);

private:
  LLILCJitContext *JitContext;
  ABIInfo *TheABIInfo;
  ReaderMethodSignature MethodSignature;
  ABIMethodSignature ABIMethodSig;
  llvm::Function *Function;
  // The LLVMBuilder has a notion of a current insertion point.  During the
  // first-pass flow-graph construction, each method sets the insertion point
  // explicitly before inserting IR (the fg- methods typically take an
  // InsertNode parameter indicating where to set it).  During the second pass
  // translation of the non-flow instructions, the insertion point is
  // explicitly set at the start of each block (in beginFlowGraphNode), and
  // translation methods assume that the builder's current insertion point is
  // where they should be inserted (the gen- methods do not take explicit
  // insertion point parameters).
  llvm::IRBuilder<> *LLVMBuilder;
  std::map<CORINFO_CLASS_HANDLE, llvm::Type *> *ClassTypeMap;
  std::map<llvm::Type *, CORINFO_CLASS_HANDLE> *ReverseClassTypeMap;
  std::map<CORINFO_CLASS_HANDLE, llvm::Type *> *BoxedTypeMap;
  std::map<std::tuple<CorInfoType, CORINFO_CLASS_HANDLE, uint32_t>,
           llvm::Type *> *ArrayTypeMap;
  std::map<CORINFO_FIELD_HANDLE, uint32_t> *FieldIndexMap;
  std::map<llvm::BasicBlock *, FlowGraphNodeInfo> FlowGraphInfoMap;
  std::vector<llvm::Value *> LocalVars;
  llvm::Value *UnmanagedCallFrame; ///< If the method contains unmanaged calls,
                                   ///< this is the address of the unmanaged
                                   ///< call frame.
  llvm::Value *ThreadPointer;      ///< If the method contains unmanaged calls,
                                   ///< this is the address of the pointer to
                                   ///< the runtime thread.
  std::vector<CorInfoType> LocalVarCorTypes;
  std::vector<llvm::Value *> Arguments;
  llvm::Value *IndirectResult;
  llvm::DenseMap<uint32_t, llvm::StoreInst *> ContinuationStoreMap;
  FlowGraphNode *FirstMSILBlock;
  llvm::BasicBlock *UnreachableContinuationBlock;
  bool KeepGenericContextAlive;
  bool NeedsSecurityObject;
  bool DoneBuildingFlowGraph;
  llvm::BasicBlock *EntryBlock;
  llvm::Instruction *TempInsertionPoint;
  IRNode *MethodSyncHandle; ///< If the method is synchronized, this is
                            ///< the handle used for entering and exiting
                            ///< the monitor.
  llvm::Value *SyncFlag;    ///< For synchronized methods this flag
                            ///< indicates whether the monitor has been
                            ///< entered. It is set and checked by monitor
                            ///< helpers.
  uint32_t TargetPointerSizeInBits;
  const uint32_t UnmanagedAddressSpace = 0;
  const uint32_t ManagedAddressSpace = 1;
};

#endif // MSIL_READER_IR_H
