//===---- lib/MSILReader/GenIRStubs.cpp -------------------------*- C++ -*-===//
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
/// \brief Common reader functionality that is either not yet implemented
/// or stubbed out.
///
//===----------------------------------------------------------------------===//

#include "reader.h"
#include <cstdarg>

// Get the special block-start placekeeping node
IRNode *fgNodeGetStartIRNode(FlowGraphNode *FgNode) { return (IRNode *)FgNode; }

// Get the first non-placekeeping node in block
IRNode *fgNodeGetStartInsertIRNode(FlowGraphNode *FgNode) {
  // Not using placekeepers yet
  return fgNodeGetStartIRNode(FgNode);
}

GlobalVerifyData *fgNodeGetGlobalVerifyData(FlowGraphNode *Fg) {
  throw NotYetImplementedException("fgNodeGetGlobalVerifyData");
}

void fgNodeSetGlobalVerifyData(FlowGraphNode *Fg, GlobalVerifyData *GvData) {
  throw NotYetImplementedException("fgNodeSetGlobalVerifyData");
}

uint32_t fgNodeGetBlockNum(FlowGraphNode *Fg) {
  throw NotYetImplementedException("fgNodeGetBlockNum");
}

#ifdef CC_PEVERIFY
void fgEdgeListMakeFake(FlowGraphEdgeList *FgEdge) {
  throw NotYetImplementedException("fgEdgeListMakeFake");
}
#endif

IRNode *irNodeGetNext(IRNode *Node) {
  throw NotYetImplementedException("irNodeGetNext");
}

bool irNodeIsBranch(IRNode *Node) {
  throw NotYetImplementedException("irNodeIsBranch");
}

IRNode *irNodeGetInsertPointAfterMSILOffset(IRNode *Node, uint32_t Offset) {
  throw NotYetImplementedException("irNodeGetInsertPointAfterMSILOffset");
}

IRNode *irNodeGetInsertPointBeforeMSILOffset(IRNode *Node, uint32_t Offset) {
  throw NotYetImplementedException("irNodeGetInsertPointBeforeMSILOffset");
}

IRNode *
irNodeGetFirstLabelOrInstrNodeInEnclosingBlock(IRNode *HandlerStartNode) {
  throw NotYetImplementedException(
      "irNodeGetFirstLabelOrInstrNodeInEnclosingBlock");
}

uint32_t irNodeGetMSILOffset(IRNode *Node) {
  throw NotYetImplementedException("irNodeGetMSILOffset");
}

void irNodeLabelSetMSILOffset(IRNode *Node, uint32_t LabelMSILOffset) {
  throw NotYetImplementedException("irNodeLabelSetMSILOffset");
}

// TODO: figure out how we're going to communicate this information.
void irNodeBranchSetMSILOffset(IRNode *BranchNode, uint32_t Offset) { return; }

void irNodeExceptSetMSILOffset(IRNode *BranchNode, uint32_t Offset) {
  throw NotYetImplementedException("irNodeExceptSetMSILOffset");
}

void irNodeInsertBefore(IRNode *InsertionPointTuple, IRNode *NewNode) {
  throw NotYetImplementedException("irNodeInsertBefore");
}

void irNodeInsertAfter(IRNode *InsertionPointTuple, IRNode *NewNode) {
  throw NotYetImplementedException("irNodeInsertAfter");
}

// TODO: figure out how we're going to communicate this information.
void irNodeSetRegion(IRNode *Node, EHRegion *Region) { return; }

EHRegion *irNodeGetRegion(IRNode *Node) {
  throw NotYetImplementedException("irNodeGetRegion");
}

FlowGraphNode *irNodeGetEnclosingBlock(IRNode *Node) {
  throw NotYetImplementedException("irNodeGetEnclosingBlock");
}

bool irNodeIsEHFlowAnnotation(IRNode *Node) {
  throw NotYetImplementedException("irNodeIsEHFlowAnnotation");
}

bool irNodeIsHandlerFlowAnnotation(IRNode *Node) {
  throw NotYetImplementedException("irNodeIsHandlerFlowAnnotation");
}

BranchList *branchListGetNext(BranchList *BranchList) {
  throw NotYetImplementedException("branchListGetNext");
}

IRNode *branchListGetIRNode(BranchList *BranchList) {
  throw NotYetImplementedException("branchListGetIRNode");
}

void ReaderBase::verifyNeedsVerification() { return; }

VerificationState *ReaderBase::verifyInitializeBlock(FlowGraphNode *,
                                                     uint32_t IlOffset) {
  return nullptr;
}

void ReaderBase::verPropEHInitFlow(FlowGraphNode *Block) { return; }

void ReaderBase::verPropHandlerInitFlow(FlowGraphNode *Block) { return; }

VerificationState *ReaderBase::verCreateNewVState(uint32_t MaxStack,
                                                  uint32_t NumLocals,
                                                  bool InitLocals,
                                                  InitState InitState) {
  return nullptr;
}

void ReaderBase::verifyFinishBlock(VerificationState *Vstate,
                                   FlowGraphNode *Block) {
  return;
}

void ReaderBase::verifyPropCtorInitToSucc(InitState CurrState,
                                          FlowGraphNode *Succ, char *Reason) {
  return;
}

void ReaderBase::verifyPropCtorInitThroughBadBlock(FlowGraphNode *Block) {
  return;
}

FlowGraphNode *ReaderBase::verifyGetRegionBlock(EHRegion *Region) {
  return nullptr;
}

void ReaderBase::verifyEnqueueBlock(GlobalVerifyData *GvSucc) { return; }

FlowGraphNode *
ReaderBase::verifyFindFaultHandlerBlock(VerificationState *Vstate,
                                        EHRegion *TryRegion) {
  return nullptr;
}

void ReaderBase::verifyRecordLocalType(uint32_t Num, CorInfoType Type,
                                       CORINFO_CLASS_HANDLE ClassHandle) {
  return;
}

void ReaderBase::verifyRecordParamType(uint32_t Num, CorInfoType Type,
                                       CORINFO_CLASS_HANDLE ClassHandle,
                                       bool MakeByRef, bool IsThis) {
  return;
}

void ReaderBase::verifyRecordParamType(uint32_t Num, CORINFO_SIG_INFO *Sig,
                                       CORINFO_ARG_LIST_HANDLE Args) {
  return;
}

void ReaderBase::verifyRecordLocalType(uint32_t Num, CORINFO_SIG_INFO *Sig,
                                       CORINFO_ARG_LIST_HANDLE Args) {
  return;
}

void ReaderBase::verifyPushExceptionObject(VerificationState *Vstate,
                                           mdToken Token) {
  return;
}

void ReaderBase::verifyFieldAccess(VerificationState *Vstate,
                                   ReaderBaseNS::OPCODE Opcode,
                                   CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

bool verIsCallToInitThisPtr(CORINFO_CLASS_HANDLE Context,
                            CORINFO_CLASS_HANDLE Ttarget) {
  return FALSE;
}

void ReaderBase::verifyLoadElemA(VerificationState *Vstate, bool ReadOnlyPrefix,
                                 CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyLoadElem(VerificationState *Vstate,
                                ReaderBaseNS::OPCODE Opcode,
                                CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyLoadConstant(VerificationState *Vstate,
                                    ReaderBaseNS::OPCODE Opcode) {
  return;
}

void ReaderBase::verifyStoreObj(VerificationState *Vstate,
                                CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyLoadObj(VerificationState *Vstate,
                               CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyStloc(VerificationState *Vstate, uint32_t Locnum) {
  return;
}

void ReaderBase::verifyIsInst(VerificationState *Vstate,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyCastClass(VerificationState *Vstate,
                                 CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyBox(VerificationState *Vstate,
                           CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyLoadAddr(VerificationState *Vstate) { return; }

void ReaderBase::verifyLoadToken(VerificationState *Vstate,
                                 CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyUnbox(VerificationState *Vstate,
                             CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyStoreElemRef(VerificationState *Vstate) { return; }

void ReaderBase::verifyLdarg(VerificationState *Vstate, uint32_t Locnum,
                             ReaderBaseNS::OPCODE Opcode) {
  return;
}

void ReaderBase::verifyStarg(VerificationState *Vstate, uint32_t Locnum) {
  return;
}

void ReaderBase::verifyLdloc(VerificationState *Vstate, uint32_t Locnum,
                             ReaderBaseNS::OPCODE Opcode) {
  return;
}

void ReaderBase::verifyStoreElem(VerificationState *Vstate,
                                 ReaderBaseNS::StElemOpcode Opcode,
                                 CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyLoadLen(VerificationState *Vstate) { return; }

void ReaderBase::verifyDup(VerificationState *Vstate, const uint8_t *CodeAddr) {
  return;
}

void ReaderBase::verifyEndFilter(VerificationState *Vstate,
                                 uint32_t MsilOffset) {
  return;
}

void ReaderBase::verifyInitObj(VerificationState *Vstate,
                               CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyCall(VerificationState *Vstate,
                            ReaderBaseNS::OPCODE Opcode, bool TailCall,
                            bool ReadOnlyCall, bool ConstraintCall,
                            bool ThisPossiblyModified,
                            mdToken ConstraintTypeRef, mdToken Token) {
  return;
}

void ReaderBase::verifyCpObj(VerificationState *Vstate,
                             CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyNewObj(VerificationState *Vstate,
                              ReaderBaseNS::OPCODE Opcode, bool IsTail,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken,
                              const uint8_t *CodeAddr) {
  return;
}

void ReaderBase::verifyBoolBranch(VerificationState *Vstate,
                                  uint32_t NextOffset, uint32_t TargetOffset) {
  return;
}

void ReaderBase::verifyLoadNull(VerificationState *Vstate) { return; }

void ReaderBase::verifyLoadStr(VerificationState *Vstate, mdToken Token) {
  return;
}

void ReaderBase::verifyIntegerBinary(VerificationState *Vstate) { return; }

void ReaderBase::verifyBinary(VerificationState *Vstate,
                              ReaderBaseNS::OPCODE Opcode) {
  return;
}

void ReaderBase::verifyShift(VerificationState *Vstate) { return; }

void ReaderBase::verifyReturn(VerificationState *Vstate, EHRegion *Region) {
  return;
}

void ReaderBase::verifyEndFinally(VerificationState *Vstate) { return; }

void ReaderBase::verifyThrow(VerificationState *Vstate) { return; }

void ReaderBase::verifyLoadFtn(VerificationState *Vstate,
                               ReaderBaseNS::OPCODE Opcode,
                               CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               const uint8_t *CodeAddr,
                               CORINFO_CALL_INFO *CallInfo) {
  return;
}

void ReaderBase::verifyNewArr(VerificationState *Vstate,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyLoadIndirect(VerificationState *Vstate,
                                    ReaderBaseNS::LdIndirOpcode Opcode) {
  return;
}

void ReaderBase::verifyStoreIndir(VerificationState *Vstate,
                                  ReaderBaseNS::StIndirOpcode Opcode) {
  return;
}

void ReaderBase::verifyConvert(VerificationState *Vstate,
                               ReaderBaseNS::ConvOpcode Opcode) {
  return;
}

void ReaderBase::verifyCompare(VerificationState *Vstate,
                               ReaderBaseNS::OPCODE Opcode) {
  return;
}

void ReaderBase::verifyUnary(VerificationState *Vstate,
                             ReaderBaseNS::UnaryOpcode Opcode) {
  return;
}

void ReaderBase::verifyPop(VerificationState *Vstate) { return; }

void ReaderBase::verifyArgList(VerificationState *Vstate) { return; }

void ReaderBase::verifyCkFinite(VerificationState *Vstate) { return; }

void ReaderBase::verifyFailure(VerificationState *Vstate) { return; }

void ReaderBase::verifyToken(mdToken Token) { return; }

void ReaderBase::verifyRefAnyVal(VerificationState *Vstate,
                                 CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyRefAnyType(VerificationState *Vstate) { return; }

void ReaderBase::verifyUnboxAny(VerificationState *Vstate,
                                CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifySwitch(VerificationState *Vstate) { return; }

void ReaderBase::verifyMkRefAny(VerificationState *Vstate,
                                CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifySizeOf(VerificationState *Vstate,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return;
}

void ReaderBase::verifyRethrow(VerificationState *Vstate, EHRegion *Region) {
  return;
}

void ReaderBase::verifyTail(VerificationState *Vstate, EHRegion *Region) {
  return;
}

void ReaderBase::verifyConstrained(VerificationState *Vstate,
                                   mdToken TypeDefOrRefOrSpec) {
  return;
}

void ReaderBase::verifyReadOnly(VerificationState *Vstate) { return; }

void ReaderBase::verifyVolatile(VerificationState *Vstate) { return; }

void ReaderBase::verifyUnaligned(VerificationState *Vstate,
                                 ReaderAlignType Alignment) {
  return;
}

void ReaderBase::verifyPrefixConsumed(VerificationState *Vstate,
                                      ReaderBaseNS::OPCODE Opcode) {
  return;
}

void ReaderBase::verifyLeave(VerificationState *Vstate) { return; }

void ReaderBase::verifyBranchTarget(VerificationState *Vstate,
                                    FlowGraphNode *CurrentFGNode,
                                    EHRegion *SrcRegion, uint32_t TargetOffset,
                                    bool IsLeave) {
  return;
}

void ReaderBase::verifyReturnFlow(uint32_t SrcOffset) { return; }

void ReaderBase::verifyFallThrough(VerificationState *Vstate,
                                   FlowGraphNode *Fg) {
  return;
}

bool verCheckDelegateCreation(ReaderBaseNS::OPCODE Opcode,
                              VerificationState *Vstate,
                              const uint8_t *CodeAddr,
                              mdMemberRef &TargetMemberRef, VerType FtnType,
                              VerType ObjType) {
  return false;
}

void ReaderBase::verVerifyCall(ReaderBaseNS::OPCODE Opcode,
                               const CORINFO_RESOLVED_TOKEN *ResolvedToken,
                               const CORINFO_CALL_INFO *CallInfo, bool TailCall,
                               const uint8_t *CodeAddr,
                               VerificationState *Vstate) {
  return;
}

void ReaderBase::verifyIsMethodToken(mdToken Token) { return; }

void ReaderBase::verifyIsCallToken(mdToken Token) { return; }

void ReaderBase::verVerifyField(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                const CORINFO_FIELD_INFO &FieldInfo,
                                const VerType *TiThis, bool Mutator) {
  return;
}

bool ReaderBase::verIsValueClass(CORINFO_CLASS_HANDLE ClassHandle) {
  return false;
}

bool ReaderBase::verIsBoxedValueType(const VerType &Ti) { return false; }

bool ReaderBase::verIsCallToken(mdToken Token) { return false; }

bool ReaderBase::verIsValClassWithStackPtr(CORINFO_CLASS_HANDLE ClassHandle) {
  return false;
}

bool ReaderBase::verIsGenericTypeVar(CORINFO_CLASS_HANDLE ClassHandle) {
  return false;
}

void ReaderBase::verDumpType(const VerType &Ti) { return; }

bool ReaderBase::verNeedsCtorTrack() { return false; }

void ReaderBase::verifyIsClassToken(mdToken Token) { return; }

void ReaderBase::verifyIsFieldToken(mdToken Token) { return; }

void ReaderBase::verifyEIT() { return; }

void ReaderBase::verGlobalError(const char *Message) { return; }

int _cdecl dbPrint(const char *Format, ...) {
  va_list Args;
  va_start(Args, Format);
  int NumChars = vfprintf(stderr, Format, Args);
  va_end(Args);
  return NumChars;
}

bool HaveEnvConfigTailCallOpt = false;
uint32_t EnvConfigTailCallOpt = 0;
bool HaveEnvConfigTailCallMax = false;
uint32_t EnvConfigTailCallMax = 0;
