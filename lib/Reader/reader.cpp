//===---- lib/Reader/reader.cpp ---------------------------------*- C++ -*-===//
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
/// \brief Common code for converting MSIL bytecode into some other
/// representation.
///
/// The common reader's operation revolves around two central classes.
/// ReaderBase:: the common reader class
/// GenIR::      an opaque vessel for holding the client's state
///
/// The GenIR class is opaque to the common reader class, all manipulations of
/// GenIR are performed by client implemented code.
///
/// The common reader generates code through the methods that are implemented in
/// this file, and static member functions that are implemented by the client.
///
//===----------------------------------------------------------------------===//

#include "earlyincludes.h"
#include "reader.h"
#include "newvstate.h"
#include "imeta.h"
#include <climits>
#include <algorithm>
#include <list>

extern int _cdecl dbPrint(const char *Form, ...);

// --------------------------------------------------------------
//   private functions and data used by common reader.
// --------------------------------------------------------------

#define BADCODE(Message) (ReaderBase::verGlobalError(Message))

// Max elements per entry in FlowGraphNodeListArray.
#define FLOW_GRAPH_NODE_LIST_ARRAY_STRIDE 32

// OPCODE REMAP
ReaderBaseNS::CallOpcode remapCallOpcode(ReaderBaseNS::OPCODE Op) {
  ReaderBaseNS::CallOpcode CallOp = (ReaderBaseNS::CallOpcode)OpcodeRemap[Op];
  ASSERTNR(CallOp >= 0 && CallOp < ReaderBaseNS::LastCallOpcode);
  return CallOp;
}

/// \brief Reads a value of type #Type from the given buffer.
///
/// \param[in]  ILCursor The buffer to read from.
/// \param[out] Value    When this function returns, the decoded value.
///
/// \returns The number of bytes read from the buffer.
template <typename Type>
size_t readValue(const uint8_t *ILCursor, Type *Value) {
// MSIL contains little-endian values; swap the bytes around when compiled
// for big-endian platforms.
#if defined(BIGENDIAN)
  for (uint32_t I = 0; I < sizeof(Type); ++I)
    ((uint8_t *)Value)[I] = ILCursor[sizeof(Type) - I - 1];
#else
  *Value = *(UNALIGNED const Type *)ILCursor;
#endif
  return sizeof(Type);
}

/// \brief Reads a value of type #Type from the given buffer.
///
/// \param[in]  ILCursor The buffer to read from.
///
/// \returns The decoded value.
template <typename Type> Type readValue(const uint8_t *ILCursor) {
  Type Value;
  readValue(ILCursor, &Value);
  return Value;
}

/// This higher level read method will read the number of switch cases from an
/// operand and then increment the buffer to the first case.
///
/// \param[in, out] The buffer to read from. After this function returns,
///                 holds the address of the first switch case.
///
/// \returns The decoded number of switch cases.
static inline uint32_t readNumberOfSwitchCases(uint8_t **ILCursor) {
  uint32_t Val;
  *ILCursor += readValue(*ILCursor, &Val);
  return Val;
}

/// This higher level read method will read a switch case from an operand
/// and then increment the buffer to the next case.
///
/// \param[in, out] The buffer to read from. After this function returns,
///                 holds the address of the next switch case.
///
/// \returns The decoded switch case.
static inline int readSwitchCase(uint8_t **ILCursor) {
  int32_t Val;
  *ILCursor += readValue(*ILCursor, &Val);
  return Val;
}

// FlowGraphNodeOffsetList - maintains a list of byte offsets at which branches
// need to be inserted along with the associated labels. Used for building
// flow graph.
class FlowGraphNodeOffsetList {
  FlowGraphNodeOffsetList *Next;
  FlowGraphNode *Node;
  uint32_t Offset;
  IRNode *ReaderStack;

public:
  FlowGraphNodeOffsetList *getNext(void) const { return Next; }
  void setNext(FlowGraphNodeOffsetList *N) { Next = N; }
  FlowGraphNode *getNode(void) const { return Node; }
  void setNode(FlowGraphNode *N) { Node = N; }
  uint32_t getOffset(void) const { return Offset; }
  void setOffset(uint32_t O) { Offset = O; }

  IRNode *getStack(void) const { return ReaderStack; }
  void setStack(IRNode *RS) { ReaderStack = RS; }
};

class ReaderBitVector {
private:
  // Some class constants
  typedef uint8_t BitVectorElement;
  static const int ElementSize = 8 * sizeof(BitVectorElement);

  BitVectorElement *BitVector;
  ReaderBitVector(){};

#if !defined(NODBUG)
  uint32_t BitVectorLength;
  uint32_t NumBits;
#endif

public:
  void allocateBitVector(uint32_t Size, ReaderBase *Reader) {
    // Allocate and zero the backing storage.
    uint32_t Length = (Size + ElementSize - 1) / ElementSize;
    BitVector = (BitVectorElement *)Reader->getTempMemory(
        sizeof(BitVectorElement) * Length);
    memset(BitVector, 0, (sizeof(BitVectorElement) * Length));

#if !defined(NODBUG)
    BitVectorLength = Length;
    NumBits = Size;
#endif
  }

  void setBit(uint32_t BitNum) {
    ASSERTDBG(BitNum < NumBits);
    uint32_t Elem = BitNum / ElementSize;
    BitVectorElement Mask = 1 << (BitNum % ElementSize);

    ASSERTDBG(Elem < BitVectorLength);
    BitVector[Elem] |= Mask;
  }

  bool getBit(uint32_t BitNum) {
    ASSERTDBG(BitNum < NumBits);
    uint32_t Elem = BitNum / ElementSize;
    BitVectorElement Mask = 1 << (BitNum % ElementSize);

    ASSERTDBG(Elem < BitVectorLength);
    return ((BitVector[Elem] & Mask) != 0);
  }

  void clrBit(uint32_t BitNum) {
    ASSERTDBG(BitNum < NumBits);
    uint32_t Elem = BitNum / ElementSize;
    BitVectorElement Mask = 1 << (BitNum % ElementSize);

    ASSERTDBG(Elem < BitVectorLength);
    BitVector[Elem] &= ~Mask;
  }
};

ReaderBase::ReaderBase(ICorJitInfo *JitInfo, CORINFO_METHOD_INFO *MethodInfo,
                       uint32_t Flags) {
  // Zero-Initialize all class data.
  memset(&this->MethodInfo, 0,
         ((char *)&DummyLastBaseField - (char *)&this->MethodInfo));

  this->JitInfo = JitInfo;
  this->MethodInfo = MethodInfo;
  this->Flags = Flags;
  MethodBeingCompiled = this->MethodInfo->ftn;
  ExactContext = MAKE_METHODCONTEXT(MethodBeingCompiled);
  IsVerifiableCode = true;
}

bool fgEdgeIteratorMoveNextSuccessorActual(FlowGraphEdgeIterator &Iterator) {
  bool HasEdge = fgEdgeIteratorMoveNextSuccessor(Iterator);
  while (HasEdge && fgEdgeIsNominal(Iterator)) {
    HasEdge = fgEdgeIteratorMoveNextSuccessor(Iterator);
  }
  return HasEdge;
}

bool fgEdgeIteratorMoveNextPredecessorActual(FlowGraphEdgeIterator &Iterator) {
  bool HasEdge = fgEdgeIteratorMoveNextPredecessor(Iterator);
  while (HasEdge && fgEdgeIsNominal(Iterator)) {
    HasEdge = fgEdgeIteratorMoveNextPredecessor(Iterator);
  }
  return HasEdge;
}

FlowGraphEdgeIterator ReaderBase::fgNodeGetSuccessorsActual(FlowGraphNode *Fg) {
  FlowGraphEdgeIterator Iterator = fgNodeGetSuccessors(Fg);
  bool HasEdge = !fgEdgeIteratorIsEnd(Iterator);
  while (HasEdge && fgEdgeIsNominal(Iterator)) {
    HasEdge = fgEdgeIteratorMoveNextSuccessor(Iterator);
  }
  return Iterator;
}

FlowGraphEdgeIterator
ReaderBase::fgNodeGetPredecessorsActual(FlowGraphNode *Fg) {
  FlowGraphEdgeIterator Iterator = fgNodeGetPredecessors(Fg);
  bool HasEdge = !fgEdgeIteratorIsEnd(Iterator);
  while (HasEdge && fgEdgeIsNominal(Iterator)) {
    HasEdge = fgEdgeIteratorMoveNextPredecessor(Iterator);
  }
  return Iterator;
}

// getMSILInstrLength
//
// Returns the length of an instruction given an op code and a pointer
// to the operand. It assumes the only variable length opcode is CEE_SWITCH.
uint32_t getMSILInstrLength(ReaderBaseNS::OPCODE Opcode, uint8_t *Operand) {

  // Table that maps opcode enum to operand size in bytes.
  // -1 indicates either an undefined opcode, or an operand
  // with variable length, in both cases the table should
  // not be used.
  static const int8_t OperandSizeMap[] = {
#define OPDEF_HELPER OPDEF_OPERANDSIZE
#include "ophelper.def"
#undef OPDEF_HELPER
  };

  uint32_t Length;

  if (Opcode == ReaderBaseNS::CEE_SWITCH) {
    ASSERTNR(nullptr != Operand);
    // Length of a switch is the 4 bytes + 4 bytes * the value of the first 4
    // bytes
    uint32_t NumCases = readNumberOfSwitchCases(&Operand);
    Length = sizeof(uint32_t) + (NumCases * sizeof(uint32_t));
  } else {
    Length = (uint32_t)OperandSizeMap[Opcode - ReaderBaseNS::CEE_NOP];
  }
  return Length;
}

/// \brief Parses a single MSIL opcode from the given buffer, reading at most
///        (ILInputSize - CurrentOffset) bytes from the input.
///
/// \param[in]      ILInput     The buffer from which to parse an opcode.
/// \param          ILOffset    The current offset into the input buffer.
/// \param          ILSize      The total size of the input buffer in bytes.
/// \param[in]      Reader      The #ReaderBase instance responsible for
///                             handling parse errors, if any occur.
/// \param[out]     Opcode      When this method returns, the parsed opcode.
/// \param[out]     Operand     When this method returns, the address of the
///                             returned opcode's operand.
/// \param          ReportError Indicates whether or not to report parse errors.
///
/// \returns The offset into the input buffer of the next MSIL opcode.
uint32_t parseILOpcode(uint8_t *ILInput, uint32_t ILOffset, uint32_t ILSize,
                       ReaderBase *Reader, ReaderBaseNS::OPCODE *Opcode,
                       uint8_t **Operand, bool ReportErrors = true) {
// Illegal opcodes are currently marked as CEE_ILLEGAL. These should
// cause verification errors.

#include "bytecodetowvmcode.def"

  uint32_t ILCursor = ILOffset;
  uint32_t OperandOffset = 0;
  uint8_t ByteCode = 0;
  ReaderBaseNS::OPCODE TheOpcode = ReaderBaseNS::CEE_ILLEGAL;

  *Opcode = ReportErrors ? ReaderBaseNS::CEE_ILLEGAL : ReaderBaseNS::CEE_NOP;
  *Operand = ReportErrors ? nullptr : &ILInput[ILCursor];

  // We need to make sure that we're not going to parse outside the buffer.
  // Note that only the opcode itself is parsed, so we can check whether
  // any operands would exceed the buffer by checking BytesRemaining after
  // the opcode has been parsed.
  //
  // This leaves two cases:
  // 1) 2-byte opcode = 0xFE then actual op (no symbolic name?)
  // 2) switch opcode = CEE_SWITCH then 4-byte length field

  // We must have at least one byte.
  if (ILCursor >= ILSize) {
    goto underflow;
  }

  ByteCode = ILInput[ILCursor++];
  if (ByteCode == 0xFE) {
    // This is case (1): a 2-byte opcode. Make sure we have at least one byte
    // remaining.
    if (ILCursor == ILSize) {
      goto underflow;
    }

    ByteCode = ILInput[ILCursor++];
    if (ByteCode <= sizeof(PrefixedByteCodes) / sizeof(PrefixedByteCodes[0])) {
      TheOpcode = PrefixedByteCodes[ByteCode];
    } else {
      TheOpcode = ReaderBaseNS::CEE_ILLEGAL;
    }
  } else {
    TheOpcode = ByteCodes[ByteCode];
  }

  // Ensure that the opcode isn't CEE_ILLEGAL.
  if (TheOpcode == ReaderBaseNS::CEE_ILLEGAL) {
    if (ReportErrors) {
      if (Reader == nullptr) {
        dbPrint("parseILOpcode: Bad Opcode\n");
      } else {
        Reader->verGlobalError(MVER_E_UNKNOWN_OPCODE);
      }
    }
    return ILSize;
  }

  // This is case (2): a switch opcode. Make sure we have at least four bytes
  // remaining s.t. getMSILInstrLength can read the number of switch cases.
  if (TheOpcode == ReaderBaseNS::CEE_SWITCH && (ILSize - ILCursor) < 4) {
    goto underflow;
  }

  OperandOffset = ILCursor;
  ILCursor += getMSILInstrLength(TheOpcode, &ILInput[OperandOffset]);
  if (ILCursor > ILSize) {
    goto underflow;
  }

  *Opcode = TheOpcode;
  *Operand = &ILInput[OperandOffset];
  return ILCursor;

underflow:
  if (ReportErrors) {
    if (Reader == nullptr) {
      dbPrint("parseILOpcode: Underflow\n");
    } else {
      Reader->verGlobalError(MVER_E_METHOD_END);
    }
  }
  return ILSize;
}

const char *OpcodeName[] = {
#define OPDEF_HELPER OPDEF_OPCODENAME
#include "ophelper.def"
#undef OPDEF_HELPER
};

void ReaderBase::printMSIL() {
  printMSIL(MethodInfo->ILCode, 0, MethodInfo->ILCodeSize);
}

void ReaderBase::printMSIL(uint8_t *Buf, uint32_t StartOffset,
                           uint32_t EndOffset) {
  uint8_t *Operand;
  ReaderBaseNS::OPCODE Opcode;
  uint32_t Offset = StartOffset;
  uint64_t OperandSize;
  uint32_t NumBytes;

  if (StartOffset >= EndOffset)
    return;

  NumBytes = EndOffset - StartOffset;

  while (Offset < NumBytes) {
    dbPrint("0x%-4x: ", StartOffset + Offset);
    Offset = parseILOpcode(Buf, Offset, NumBytes, nullptr, &Opcode, &Operand);
    dbPrint("%d: %-10s ", Offset, OpcodeName[Opcode]);

    switch (Opcode) {
    default:
      OperandSize = (Buf + Offset) - Operand;
      switch (OperandSize) {
      case 0:
        break;
      case 1:
        dbPrint("0x%x", readValue<int8_t>(Operand));
        break;
      case 2:
        dbPrint("0x%x", readValue<int16_t>(Operand));
        break;
      case 4:
        if (Opcode == ReaderBaseNS::CEE_LDC_R4) {
          dbPrint("%f", readValue<float>(Operand));
        } else {
          dbPrint("0x%x", readValue<int32_t>(Operand));
        }
        break;
      case 8:
        if (Opcode == ReaderBaseNS::CEE_LDC_R8) {
          dbPrint("%f", readValue<double>(Operand));
        } else {
          dbPrint("0x%I64x", readValue<int64_t>(Operand));
        }
        break;
      }
      break;

    case ReaderBaseNS::CEE_SWITCH: {
      uint32_t NumCases = readNumberOfSwitchCases(&Operand);
      dbPrint("%-4d cases\n", NumCases);
      for (uint32_t I = 0; I < NumCases; I++) {
        dbPrint("        case %d: 0x%x\n", I, readSwitchCase(&Operand));
      }
    } break;
    }
    dbPrint("\n");
  }
}

//////////////////////////////////////////////////////////////////////////
//
// EE Data Accessor Methods.
//
// GenIR does not have access to JitInfo or MethodInfo. It must
// ask the reader to fetch metadata.
//
//////////////////////////////////////////////////////////////////////////

bool ReaderBase::isPrimitiveType(CORINFO_CLASS_HANDLE Handle) {
  return isPrimitiveType(JitInfo->asCorInfoType((CORINFO_CLASS_HANDLE)Handle));
}

bool ReaderBase::isPrimitiveType(CorInfoType CorInfoType) {
  return (CORINFO_TYPE_BOOL <= CorInfoType &&
          CorInfoType <= CORINFO_TYPE_DOUBLE);
}

void *ReaderBase::getHelperDescr(CorInfoHelpFunc HelpFuncId, bool *IsIndirect) {
  void *HelperHandle, *IndirectHelperHandle;

  ASSERTNR(IsIndirect != nullptr);
  HelperHandle = JitInfo->getHelperFtn(HelpFuncId, &IndirectHelperHandle);
  if (HelperHandle != nullptr) {
    *IsIndirect = false;
    return HelperHandle;
  }

  ASSERTNR(IndirectHelperHandle != nullptr);
  *IsIndirect = true;
  return IndirectHelperHandle;
}

CorInfoHelpFunc
ReaderBase::getNewHelper(CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return JitInfo->getNewHelper(ResolvedToken, getCurrentMethodHandle());
}

void *ReaderBase::getVarArgsHandle(CORINFO_SIG_INFO *Sig, bool *IsIndirect) {
  CORINFO_VARARGS_HANDLE *IndirectVarCookie;
  CORINFO_VARARGS_HANDLE VarCookie;

  VarCookie = JitInfo->getVarArgsHandle(Sig, (void **)&IndirectVarCookie);
  ASSERTNR((!VarCookie) != (!IndirectVarCookie));

  if (VarCookie != nullptr) {
    *IsIndirect = false;
    return VarCookie;
  } else {
    *IsIndirect = true;
    return IndirectVarCookie;
  }
}

bool ReaderBase::canGetVarArgsHandle(CORINFO_SIG_INFO *Sig) {
  return JitInfo->canGetVarArgsHandle(Sig);
}

//////////////////////////////////////////////////////////////////////////
//
// Properties of current method.
//
//////////////////////////////////////////////////////////////////////////

bool ReaderBase::isZeroInitLocals(void) {
  return ((MethodInfo->options & CORINFO_OPT_INIT_LOCALS) != 0);
}

uint32_t ReaderBase::getCurrentMethodNumAutos(void) {
  return MethodInfo->locals.numArgs;
}

CORINFO_CLASS_HANDLE
ReaderBase::getCurrentMethodClass(void) {
  return JitInfo->getMethodClass(getCurrentMethodHandle());
}

CORINFO_METHOD_HANDLE
ReaderBase::getCurrentMethodHandle(void) { return MethodInfo->ftn; }

CORINFO_METHOD_HANDLE
ReaderBase::getCurrentContext(void) { return ExactContext; }

// Returns the EE's hash code for the method being compiled.
uint32_t ReaderBase::getCurrentMethodHash(void) {
  return JitInfo->getMethodHash(getCurrentMethodHandle());
}

uint32_t ReaderBase::getCurrentMethodAttribs(void) {
  return JitInfo->getMethodAttribs(getCurrentMethodHandle());
}

const char *ReaderBase::getCurrentMethodName(const char **ModuleName) {
  return JitInfo->getMethodName(getCurrentMethodHandle(), ModuleName);
}

mdToken ReaderBase::getMethodDefFromMethod(CORINFO_METHOD_HANDLE Handle) {
  return JitInfo->getMethodDefFromMethod(Handle);
}

void ReaderBase::getFunctionEntryPoint(CORINFO_METHOD_HANDLE Handle,
                                       CORINFO_CONST_LOOKUP *Result,
                                       CORINFO_ACCESS_FLAGS AccessFlags) {
  JitInfo->getFunctionEntryPoint(Handle, Result, AccessFlags);
}

void ReaderBase::getFunctionFixedEntryPoint(CORINFO_METHOD_HANDLE Handle,
                                            CORINFO_CONST_LOOKUP *Result) {
  JitInfo->getFunctionFixedEntryPoint(Handle, Result);
}

CORINFO_MODULE_HANDLE
ReaderBase::getCurrentModuleHandle(void) { return MethodInfo->scope; }

void ReaderBase::embedGenericHandle(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                    bool ShouldEmbedParent,
                                    CORINFO_GENERICHANDLE_RESULT *Result) {
  JitInfo->embedGenericHandle(ResolvedToken, ShouldEmbedParent, Result);
}

//////////////////////////////////////////////////////////////////////////
//
// Properties of current jitinfo.
// These functions assume the context of the current module and method info.
//
//////////////////////////////////////////////////////////////////////////

//
// class
//

CORINFO_CLASS_HANDLE
ReaderBase::getMethodClass(CORINFO_METHOD_HANDLE Handle) {
  return JitInfo->getMethodClass(Handle);
}

void ReaderBase::getMethodVTableOffset(CORINFO_METHOD_HANDLE Handle,
                                       uint32_t *OffsetOfIndirection,
                                       uint32_t *OffsetAfterIndirection) {
  JitInfo->getMethodVTableOffset(Handle, OffsetOfIndirection,
                                 OffsetAfterIndirection);
}

bool ReaderBase::checkMethodModifier(
    CORINFO_METHOD_HANDLE Method,
    LPCSTR Modifier, // name of the modifier to check for
    bool IsOptional  // true for modopt, false for modreqd
    ) {
  return JitInfo->checkMethodModifier(Method, Modifier, IsOptional);
}

const char *ReaderBase::getClassName(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getClassName(Class);
}

int ReaderBase::appendClassName(char16_t **Buffer, int32_t *BufferLen,
                                CORINFO_CLASS_HANDLE Class,
                                bool IncludeNamespace, bool FullInst,
                                bool IncludeAssembly) {
  int IntBufferLen = *BufferLen, Return;
  Return = JitInfo->appendClassName(
      (WCHAR **)Buffer, &IntBufferLen, Class, IncludeNamespace ? TRUE : FALSE,
      FullInst ? TRUE : FALSE, IncludeAssembly ? TRUE : FALSE);
  *BufferLen = IntBufferLen;
  return Return;
}

// Construct The GC Layout from CoreCLR Type
GCLayout *ReaderBase::getClassGCLayout(CORINFO_CLASS_HANDLE Class) {
  // The actual size of the byte array the runtime is expecting (gcLayoutSize)
  // is one byte for every sizeof(void*) slot in the valueclass.
  // Note that we round this computation up.
  const uint32_t PointerSize = getPointerByteSize();
  const uint32_t ClassSize = JitInfo->getClassSize(Class);
  const uint32_t GcLayoutSize = ((ClassSize + PointerSize - 1) / PointerSize);

  // Our internal data structures prepend the number of GC pointers
  // before the struct.  Therefore we add the size of the
  // GCLAYOUT_STRUCT to our computed size above.
  GCLayout *GCLayoutInfo =
      (GCLayout *)calloc(GcLayoutSize + sizeof(GCLayout), sizeof(uint8_t));
  uint32_t NumGCVars =
      JitInfo->getClassGClayout(Class, GCLayoutInfo->GCPointers);

  if (NumGCVars > 0) {
    // We cache away the number of GC vars.
    GCLayoutInfo->NumGCPointers = NumGCVars;
  } else {
    // If we had no GC pointers, then we won't bother returning the
    // GCLayout.  It is our convention that if you have a GCLayout,
    // then you have pointers.  This allows us to do only one check
    // when we want to know if a MB has GC pointers.
    free(GCLayoutInfo);
    GCLayoutInfo = nullptr;
  }

  return GCLayoutInfo;
}

bool ReaderBase::classHasGCPointers(CORINFO_CLASS_HANDLE Class) {
  GCLayout *Layout = getClassGCLayout(Class);
  free(Layout);
  return Layout != nullptr;
}

uint32_t ReaderBase::getClassAttribs(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getClassAttribs(Class);
}

uint32_t ReaderBase::getClassSize(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getClassSize(Class);
}

uint32_t ReaderBase::getClassAlignmentRequirement(CORINFO_CLASS_HANDLE Class) {
  // Class must be value (a non-primitive value class, a multibyte)
  ASSERTNR(Class && (getClassAttribs(Class) & CORINFO_FLG_VALUECLASS));

#if !defined(NODEBUG)
  // Make sure that classes which contain GC refs also have sufficient
  // alignment requirements.
  if (classHasGCPointers(Class)) {
    const uint32_t PointerSize = getPointerByteSize();
    ASSERTNR(JitInfo->getClassAlignmentRequirement(Class) >= PointerSize);
  }
#endif // !NODEBUG

  return JitInfo->getClassAlignmentRequirement(Class);
}

ReaderAlignType
ReaderBase::getMinimumClassAlignment(CORINFO_CLASS_HANDLE Class,
                                     ReaderAlignType Alignment) {
  ReaderAlignType AlignRequired;

  AlignRequired = (ReaderAlignType)getClassAlignmentRequirement(Class);
  if (AlignRequired != 0 &&
      (Alignment == Reader_AlignNatural || Alignment > AlignRequired)) {
    Alignment = AlignRequired;
  }

  // Unaligned GC pointers are not supported by the CLR.
  // Simply ignore users that specify otherwise
  if (classHasGCPointers(Class)) {
    Alignment = Reader_AlignNatural;
  }

  return Alignment;
}

CorInfoType ReaderBase::getClassType(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->asCorInfoType(Class);
}

// Size and pRetSig are 0/nullptr if type is non-value or primitive.
void ReaderBase::getClassType(CORINFO_CLASS_HANDLE Class, uint32_t ClassAttribs,
                              CorInfoType *CorInfoType, uint32_t *Size) {

  if ((ClassAttribs & CORINFO_FLG_VALUECLASS) == 0) {
    // If non-value class then create pointer temp
    *CorInfoType = CORINFO_TYPE_PTR;
    *Size = 0;
  } else if (isPrimitiveType(Class)) {
    // If primitive value type then create temp of that type
    *CorInfoType = JitInfo->asCorInfoType(Class);
    *Size = 0;
  } else {
    // else class is non-primitive value class, a multibyte
    *CorInfoType = CORINFO_TYPE_VALUECLASS;
    *Size = getClassSize(Class);
  }
}

bool ReaderBase::canInlineTypeCheckWithObjectVTable(
    CORINFO_CLASS_HANDLE Class) {
  return JitInfo->canInlineTypeCheckWithObjectVTable(Class);
}

bool ReaderBase::accessStaticFieldRequiresClassConstructor(
    CORINFO_FIELD_HANDLE FieldHandle) {
  return (initClass(FieldHandle, getCurrentMethodHandle(),
                    getCurrentContext()) &
          CORINFO_INITCLASS_USE_HELPER) != 0;
}

void ReaderBase::classMustBeLoadedBeforeCodeIsRun(CORINFO_CLASS_HANDLE Handle) {
  JitInfo->classMustBeLoadedBeforeCodeIsRun(Handle);
}

CorInfoInitClassResult ReaderBase::initClass(CORINFO_FIELD_HANDLE Field,
                                             CORINFO_METHOD_HANDLE Method,
                                             CORINFO_CONTEXT_HANDLE Context,
                                             bool Speculative) {
  return JitInfo->initClass(Field, Method, Context, Speculative);
}

//
// field
//

const char *ReaderBase::getFieldName(CORINFO_FIELD_HANDLE Field,
                                     const char **ModuleName) {
  return JitInfo->getFieldName(Field, ModuleName);
}

// Returns a handle to a field that can be embedded in the JITed code
CORINFO_FIELD_HANDLE
ReaderBase::embedFieldHandle(CORINFO_FIELD_HANDLE Field, bool *IsIndirect) {
  CORINFO_FIELD_HANDLE DirectFieldHandle, IndirectFieldHandle;
  DirectFieldHandle =
      JitInfo->embedFieldHandle(Field, (void **)&IndirectFieldHandle);

  if (DirectFieldHandle != nullptr) {
    ASSERTNR(IndirectFieldHandle == nullptr);
    *IsIndirect = false;
    return DirectFieldHandle;
  } else {
    ASSERTNR(IndirectFieldHandle != nullptr);
    *IsIndirect = true;
    return IndirectFieldHandle;
  }
}

CORINFO_CLASS_HANDLE
ReaderBase::getFieldClass(CORINFO_FIELD_HANDLE Field) {
  return JitInfo->getFieldClass(Field);
}

CorInfoType ReaderBase::getFieldType(
    CORINFO_FIELD_HANDLE Field, CORINFO_CLASS_HANDLE *Class,
    CORINFO_CLASS_HANDLE Owner /* optional: for verification */
    ) {
  return JitInfo->getFieldType(Field, Class, Owner);
}

uint32_t ReaderBase::getClassNumInstanceFields(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getClassNumInstanceFields(Class);
}

CORINFO_FIELD_HANDLE
ReaderBase::getFieldInClass(CORINFO_CLASS_HANDLE Class, uint32_t Ordinal) {
  return JitInfo->getFieldInClass(Class, Ordinal);
}

void ReaderBase::getFieldInfo(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                              CORINFO_ACCESS_FLAGS AccessFlags,
                              CORINFO_FIELD_INFO *FieldInfo) {
  if (Flags & CORJIT_FLG_READYTORUN) {
    // CORINFO_ACCESS_ATYPICAL_CALLSITE means that we can't guarantee
    // that we'll be able to generate call [rel32] form of the helper call so
    // crossgen shouldn't try to disassemble the call instruction.
    AccessFlags =
        (CORINFO_ACCESS_FLAGS)(AccessFlags | CORINFO_ACCESS_ATYPICAL_CALLSITE);
  }

  JitInfo->getFieldInfo(ResolvedToken, getCurrentMethodHandle(), AccessFlags,
                        FieldInfo);
}
CorInfoType ReaderBase::getFieldInfo(CORINFO_CLASS_HANDLE Class,
                                     uint32_t Ordinal, uint32_t *FieldOffset,
                                     CORINFO_CLASS_HANDLE *FieldClass) {
  CORINFO_FIELD_HANDLE Field;

  Field = JitInfo->getFieldInClass(Class, Ordinal);
  if (FieldOffset) {
    *FieldOffset = JitInfo->getFieldOffset(Field);
  }
  return JitInfo->getFieldType(Field, FieldClass);
}

CorInfoIsAccessAllowedResult
ReaderBase::canAccessClass(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                           CORINFO_METHOD_HANDLE Caller,
                           CORINFO_HELPER_DESC *ThrowHelper) {
  return JitInfo->canAccessClass(ResolvedToken, Caller, ThrowHelper);
}

uint32_t ReaderBase::getFieldOffset(CORINFO_FIELD_HANDLE Field) {
  return JitInfo->getFieldOffset(Field);
}

void *ReaderBase::getStaticFieldAddress(CORINFO_FIELD_HANDLE Field,
                                        bool *IsIndirect) {
  *IsIndirect = false;

  void *IndirectFieldAddress;
  void *FieldAddress =
      (void *)JitInfo->getFieldAddress(Field, &IndirectFieldAddress);

  ASSERTNR(((FieldAddress != nullptr) && (IndirectFieldAddress == nullptr)) ||
           ((FieldAddress == nullptr) && (IndirectFieldAddress != nullptr)));

  if (FieldAddress == nullptr) {
    *IsIndirect = true;
    return IndirectFieldAddress;
  }

  return FieldAddress;
}

void *ReaderBase::getJustMyCodeHandle(CORINFO_METHOD_HANDLE Handle,
                                      bool *IsIndirect) {
  CORINFO_JUST_MY_CODE_HANDLE DebugHandle, *IndirectDebugHandle = nullptr;
  DebugHandle = JitInfo->getJustMyCodeHandle(Handle, &IndirectDebugHandle);
  ASSERTNR(!(DebugHandle && IndirectDebugHandle)); // both can't be non-null

  if (DebugHandle) {
    *IsIndirect = false;
    return DebugHandle;
  } else {
    *IsIndirect = true;
    return IndirectDebugHandle;
  }
}

void *ReaderBase::getMethodSync(bool *IsIndirect) {
  void *CriticalSection = 0, *IndirectCriticalSection = 0;
  CriticalSection = JitInfo->getMethodSync(getCurrentMethodHandle(),
                                           &IndirectCriticalSection);
  ASSERT((!CriticalSection) != (!IndirectCriticalSection));

  if (CriticalSection) {
    *IsIndirect = false;
    return CriticalSection;
  } else {
    *IsIndirect = true;
    return IndirectCriticalSection;
  }
}

void *ReaderBase::getCookieForPInvokeCalliSig(CORINFO_SIG_INFO *SigTarget,
                                              bool *IsIndirect) {
  void *CalliCookie, *IndirectCalliCookie = nullptr;
  CalliCookie =
      JitInfo->GetCookieForPInvokeCalliSig(SigTarget, &IndirectCalliCookie);
  ASSERTNR((!CalliCookie) != (!IndirectCalliCookie)); // both can't be non-null

  if (CalliCookie) {
    *IsIndirect = false;
    return CalliCookie;
  } else {
    *IsIndirect = true;
    return IndirectCalliCookie;
  }
}

bool ReaderBase::canGetCookieForPInvokeCalliSig(CORINFO_SIG_INFO *SigTarget) {
  return JitInfo->canGetCookieForPInvokeCalliSig(SigTarget);
}

void *ReaderBase::getAddressOfPInvokeFixup(CORINFO_METHOD_HANDLE Method,
                                           InfoAccessType *AccessType) {
  ASSERTNR(AccessType);
  void *IndirectAddress;
  void *Address = JitInfo->getAddressOfPInvokeFixup(Method, &IndirectAddress);

  if (Address) {
    *AccessType = IAT_VALUE;
    return Address;
  } else {
    ASSERTNR(IndirectAddress);
    *AccessType = IAT_PVALUE;
    return IndirectAddress;
  }
}

void *ReaderBase::getPInvokeUnmanagedTarget(CORINFO_METHOD_HANDLE Method) {
  void *Unused = nullptr;
  // Always retuns the entry point of the call or null.
  return JitInfo->getPInvokeUnmanagedTarget(Method, &Unused);
}

bool ReaderBase::pInvokeMarshalingRequired(CORINFO_METHOD_HANDLE Method,
                                           CORINFO_SIG_INFO *Sig) {
  return JitInfo->pInvokeMarshalingRequired(Method, Sig) ? true : false;
}

//
// method
//

const char *ReaderBase::getMethodName(CORINFO_METHOD_HANDLE Method,
                                      const char **ModuleName) {
  return JitInfo->getMethodName(Method, ModuleName);
}

// Find the attribs of the method handle
uint32_t ReaderBase::getMethodAttribs(CORINFO_METHOD_HANDLE Method) {
  return JitInfo->getMethodAttribs(Method);
}

void ReaderBase::setMethodAttribs(CORINFO_METHOD_HANDLE Method,
                                  CorInfoMethodRuntimeFlags Flags) {
  return JitInfo->setMethodAttribs(Method, Flags);
}

void ReaderBase::getMethodSig(CORINFO_METHOD_HANDLE Method,
                              CORINFO_SIG_INFO *Sig) {
  JitInfo->getMethodSig(Method, Sig);
}

const char *ReaderBase::getMethodRefInfo(CORINFO_METHOD_HANDLE Method,
                                         CorInfoCallConv *CallingConvention,
                                         CorInfoType *CorType,
                                         CORINFO_CLASS_HANDLE *RetTypeClass,
                                         const char **ModuleName) {
  CORINFO_SIG_INFO Sig;

  // Fetch Signature.
  JitInfo->getMethodSig(Method, &Sig);

  // Get the calling convention
  *CallingConvention = Sig.getCallConv();

  // Get the return type
  *CorType = Sig.retType;
  *RetTypeClass = Sig.retTypeClass;

  // Get method and module name.
  return JitInfo->getMethodName(Method, ModuleName);
}

void ReaderBase::getMethodSigData(CorInfoCallConv *CallingConvention,
                                  CorInfoType *ReturnType,
                                  CORINFO_CLASS_HANDLE *ReturnClass,
                                  uint32_t *TotalILArgs, bool *IsVarArg,
                                  bool *HasThis, uint8_t *RetSig) {
  CORINFO_SIG_INFO Sig;

  JitInfo->getMethodSig(getCurrentMethodHandle(), &Sig);
  *CallingConvention = Sig.getCallConv();
  *ReturnType = Sig.retType;
  *ReturnClass = Sig.retTypeClass;
  *TotalILArgs = (uint32_t)Sig.totalILArgs();
  *IsVarArg = Sig.isVarArg();
  *HasThis = Sig.hasThis();
  *RetSig = 0;
}

void ReaderBase::getMethodInfo(CORINFO_METHOD_HANDLE Method,
                               CORINFO_METHOD_INFO *Info) {
  JitInfo->getMethodInfo(Method, Info);
}

void ReaderBase::methodMustBeLoadedBeforeCodeIsRun(
    CORINFO_METHOD_HANDLE Method) {
  JitInfo->methodMustBeLoadedBeforeCodeIsRun(Method);
}

LONG ReaderBase::eeJITFilter(PEXCEPTION_POINTERS ExceptionPointersPtr,
                             void *Param) {
  JITFilterParam *TheJITFilterParam = (JITFilterParam *)Param;
  ICorJitInfo *JitInfo = TheJITFilterParam->JitInfo;
  TheJITFilterParam->ExceptionPointers = *ExceptionPointersPtr;
  int Answer = JitInfo->FilterException(ExceptionPointersPtr);

#ifdef CC_PEVERIFY
  verLastError = JitInfo->GetErrorHRESULT(ExceptionPointersPtr);
#endif
  return Answer;
}

// Finds name of MemberRef or MethodDef token
void ReaderBase::findNameOfToken(mdToken Token, char *Buffer,
                                 size_t BufferSize) {
  findNameOfToken(getCurrentModuleHandle(), Token, Buffer, BufferSize);
}

void ReaderBase::findNameOfToken(CORINFO_MODULE_HANDLE Scope, mdToken Token,
                                 char *Buffer, size_t BufferSize) {
  JitInfo->findNameOfToken(Scope, Token, Buffer, BufferSize);
}

// In general one should use embedGenericHandle via GenericTokenToNode
//   instead of embedMethodHandle. This also holds true for embedClassHandle as
//   well. This is due to the fact that special handling has to be done in the
//   case of generics.
//
// Currently embedMethodHandle is called from four places.
//   1 - GenericTokenToNode
//   2 - checkCallAuthorization
//   3 - callPinvokeInlineHelper
//   4 - InsertClassConstructor
CORINFO_METHOD_HANDLE
ReaderBase::embedMethodHandle(CORINFO_METHOD_HANDLE Method, bool *IsIndirect) {
  void *MethodHandle, *IndirectMethodHandle;

  MethodHandle = JitInfo->embedMethodHandle(Method, &IndirectMethodHandle);
  if (MethodHandle) {
    ASSERTNR(!IndirectMethodHandle);
    *IsIndirect = false;
    return (CORINFO_METHOD_HANDLE)MethodHandle;
  } else {
    ASSERTNR(IndirectMethodHandle);
    *IsIndirect = true;
    return (CORINFO_METHOD_HANDLE)IndirectMethodHandle;
  }
}

// In general one should use embedGenericHandle via GenericTokenToNode
//   instead of embedClassHandle. This also holds true for embedMethodHandle as
//   well. This is due to the fact that special handling has to be done in the
//   case of generics.
//
// Currently embedMethodHandle is called from two places.
//   1 - InsertClassConstructor
//   2 - rdrCallWriteBarrierHelper (okayed by DSyme)
//
CORINFO_CLASS_HANDLE
ReaderBase::embedClassHandle(CORINFO_CLASS_HANDLE Class, bool *IsIndirect) {
  void *ClassHandle, *IndirectClassHandle;

  ClassHandle = JitInfo->embedClassHandle(Class, &IndirectClassHandle);
  if (ClassHandle) {
    ASSERTNR(!IndirectClassHandle);
    *IsIndirect = false;
    return (CORINFO_CLASS_HANDLE)ClassHandle;
  } else {
    ASSERTNR(IndirectClassHandle);
    *IsIndirect = true;
    return (CORINFO_CLASS_HANDLE)IndirectClassHandle;
  }
}

CorInfoHelpFunc ReaderBase::getSharedCCtorHelper(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getSharedCCtorHelper(Class);
}

CORINFO_CLASS_HANDLE ReaderBase::getTypeForBox(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getTypeForBox(Class);
}

CorInfoHelpFunc ReaderBase::getBoxHelper(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getBoxHelper(Class);
}

CorInfoHelpFunc ReaderBase::getUnBoxHelper(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getUnBoxHelper(Class);
}

void *ReaderBase::getAddrOfCaptureThreadGlobal(bool *IsIndirect) {
  void *Address, *IndirectAddress;

  Address = JitInfo->getAddrOfCaptureThreadGlobal(&IndirectAddress);

  if (Address) {
    ASSERTNR(!IndirectAddress);
    *IsIndirect = false;
    return Address;
  } else {
    ASSERTNR(IndirectAddress);
    *IsIndirect = true;
    return IndirectAddress;
  }
}

void ReaderBase::getCallInfo(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             CORINFO_RESOLVED_TOKEN *ConstrainedResolvedToken,
                             CORINFO_CALLINFO_FLAGS Flags,
                             CORINFO_CALL_INFO *Result,
                             CORINFO_METHOD_HANDLE Caller) {
  // Always do the security checks
  Flags = (CORINFO_CALLINFO_FLAGS)(Flags | CORINFO_CALLINFO_SECURITYCHECKS);
  if (VerificationNeeded)
    Flags = (CORINFO_CALLINFO_FLAGS)(Flags | CORINFO_CALLINFO_VERIFICATION);

  if (this->Flags & CORJIT_FLG_READYTORUN) {
    // CORINFO_ACCESS_ATYPICAL_CALLSITE means that we can't guarantee
    // that we'll be able to generate call [rel32] form so crossgen shouldn't
    // try to disassemble the call instruction.
    Flags =
        (CORINFO_CALLINFO_FLAGS)(Flags | CORINFO_CALLINFO_ATYPICAL_CALLSITE);
  }

  JitInfo->getCallInfo(ResolvedToken, ConstrainedResolvedToken, Caller, Flags,
                       Result);
}

void ReaderBase::getCallInfo(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                             CORINFO_RESOLVED_TOKEN *ConstrainedResolvedToken,
                             CORINFO_CALLINFO_FLAGS Flags,
                             CORINFO_CALL_INFO *Result) {
  getCallInfo(ResolvedToken, ConstrainedResolvedToken, Flags, Result,
              getCurrentMethodHandle());
}

void *ReaderBase::getEmbedModuleDomainIDForStatics(CORINFO_CLASS_HANDLE Class,
                                                   bool *IsIndirect) {
  size_t ModuleDomainID;
  void *IndirectModuleDomainID;

  ModuleDomainID = JitInfo->getClassModuleIdForStatics(Class, nullptr,
                                                       &IndirectModuleDomainID);

  if (IndirectModuleDomainID == nullptr) {
    *IsIndirect = false;
    /* Note this is an unsigned integer not a relocatable pointer */
    return (void *)ModuleDomainID;
  } else {
    *IsIndirect = true;
    return IndirectModuleDomainID;
  }
}

void *ReaderBase::getEmbedClassDomainID(CORINFO_CLASS_HANDLE Class,
                                        bool *IsIndirect) {
  uint32_t ClassDomainID;
  void *IndirectClassDomainID;

  ClassDomainID = JitInfo->getClassDomainID(Class, &IndirectClassDomainID);

  if (IndirectClassDomainID == nullptr) {
    *IsIndirect = false;
    /* Note this is an unsigned integer not a relocatable pointer */
    return (void *)(size_t)ClassDomainID;
  } else {
    *IsIndirect = true;
    return IndirectClassDomainID;
  }
}

InfoAccessType ReaderBase::constructStringLiteral(mdToken Token, void **Info) {
  return JitInfo->constructStringLiteral(getCurrentModuleHandle(), Token, Info);
}

void ReaderBase::handleMemberAccess(CorInfoIsAccessAllowedResult AccessAllowed,
                                    const CORINFO_HELPER_DESC &AccessHelper) {
  if (AccessAllowed == CORINFO_ACCESS_ALLOWED)
    return;
  handleMemberAccessWorker(AccessAllowed, AccessHelper);
}

void ReaderBase::handleMemberAccessWorker(
    CorInfoIsAccessAllowedResult AccessAllowed,
    const CORINFO_HELPER_DESC &AccessHelper) {
  switch (AccessAllowed) {
  case CORINFO_ACCESS_ALLOWED:
    ASSERTNR(!"don't call this for allowed");
  case CORINFO_ACCESS_ILLEGAL:
  case CORINFO_ACCESS_RUNTIME_CHECK:
    insertHelperCall(AccessHelper);
  }
}

void ReaderBase::handleMemberAccessForVerification(
    CorInfoIsAccessAllowedResult AccessAllowed,
    const CORINFO_HELPER_DESC &AccessHelper,
#ifdef CC_PEVERIFY
    HRESULT HResult
#else
    const char *HResult
#endif // CC_PEVERIFY
    ) {
  switch (AccessAllowed) {
  case CORINFO_ACCESS_ILLEGAL:
#ifdef CC_PEVERIFY
    // In the PE Verify case, treat it as the correct verification error type.
    ReaderBase::verifyOrReturn(0, HResult);
#else
    // Otherwise we only throw if we're in verify only mode.  Otherwise we'd
    // like the option to insert a
    // throw helper at the call site.
    if (Flags & CORJIT_FLG_IMPORT_ONLY) {
      JitInfo->ThrowExceptionForHelper(&AccessHelper);
    }
#endif
    break;
  case CORINFO_ACCESS_ALLOWED:
  case CORINFO_ACCESS_RUNTIME_CHECK:
    // In the verifier, do nothing.
    break;
  }
}

void ReaderBase::handleClassAccess(CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  CorInfoIsAccessAllowedResult Auth;
  CORINFO_HELPER_DESC AccessAllowedInfo;

  Auth = JitInfo->canAccessClass(ResolvedToken, getCurrentMethodHandle(),
                                 &AccessAllowedInfo);
  handleMemberAccess(Auth, AccessAllowedInfo);
}

void ReaderBase::insertHelperCall(
    const CORINFO_HELPER_DESC &AccessAllowedInfo) {
  // Decision deferred

  removeStackInterference();

  bool IsIndirect;

  void *RealHandle = nullptr;
  void *EmbeddedHandle = nullptr;

  IRNode *HelperArgNodes[CORINFO_ACCESS_ALLOWED_MAX_ARGS] = {0};
  ASSERTNR(AccessAllowedInfo.numArgs <=
           (sizeof(HelperArgNodes) / sizeof(HelperArgNodes[0])));
  for (uint32_t Index = 0; Index < AccessAllowedInfo.numArgs; ++Index) {
    IRNode *CurrentArg = nullptr;
    const CORINFO_HELPER_ARG &HelperArg = AccessAllowedInfo.args[Index];
    switch (HelperArg.argType) {
    case CORINFO_HELPER_ARG_TYPE_Field:
      RealHandle = HelperArg.fieldHandle;
      JitInfo->classMustBeLoadedBeforeCodeIsRun(
          JitInfo->getFieldClass(HelperArg.fieldHandle));
      EmbeddedHandle = embedFieldHandle(HelperArg.fieldHandle, &IsIndirect);
      goto HANDLE_COMMON;
    case CORINFO_HELPER_ARG_TYPE_Method:
      RealHandle = HelperArg.methodHandle;
      JitInfo->methodMustBeLoadedBeforeCodeIsRun(HelperArg.methodHandle);
      EmbeddedHandle = embedMethodHandle(HelperArg.methodHandle, &IsIndirect);
      goto HANDLE_COMMON;
    case CORINFO_HELPER_ARG_TYPE_Class:
      RealHandle = HelperArg.classHandle;
      JitInfo->classMustBeLoadedBeforeCodeIsRun(HelperArg.classHandle);
      EmbeddedHandle = embedClassHandle(HelperArg.classHandle, &IsIndirect);
      goto HANDLE_COMMON;
    case CORINFO_HELPER_ARG_TYPE_Module: {
      void *IndirectModuleHandle;
      RealHandle = HelperArg.moduleHandle;
      EmbeddedHandle = JitInfo->embedModuleHandle(HelperArg.moduleHandle,
                                                  &IndirectModuleHandle);
      if (EmbeddedHandle == nullptr) {
        IsIndirect = true;
        EmbeddedHandle = IndirectModuleHandle;
      } else {
        IsIndirect = false;
      }
      goto HANDLE_COMMON;
    }
    HANDLE_COMMON:
      CurrentArg = handleToIRNode(mdTokenNil, /*Never used*/
                                  EmbeddedHandle, RealHandle, IsIndirect,
                                  IsIndirect,   /* read only */
                                  true, false); /* call target */
      break;

    case CORINFO_HELPER_ARG_TYPE_Const:
      CurrentArg = loadConstantI8((__int32)HelperArg.constant);
      break;
    default:
      ASSERTMNR(UNREACHED, "Unexpected constant kind.");
      break;
    }
    HelperArgNodes[Index] = CurrentArg;
  }

  const bool MayThrow = true;
  callHelper(AccessAllowedInfo.helperNum, MayThrow, nullptr, HelperArgNodes[0],
             HelperArgNodes[1], HelperArgNodes[2], HelperArgNodes[3]);
}

bool ReaderBase::canTailCall(CORINFO_METHOD_HANDLE DeclaredTarget,
                             CORINFO_METHOD_HANDLE ExactTarget,
                             bool IsTailPrefix) {
  return JitInfo->canTailCall(getCurrentMethodHandle(), DeclaredTarget,
                              ExactTarget, IsTailPrefix);
}

CorInfoInline ReaderBase::canInline(CORINFO_METHOD_HANDLE Caller,
                                    CORINFO_METHOD_HANDLE Target,
                                    uint32_t *Restrictions) {
  return JitInfo->canInline(Caller, Target, (DWORD *)Restrictions);
}

CORINFO_ARG_LIST_HANDLE
ReaderBase::getArgNext(CORINFO_ARG_LIST_HANDLE Args) {
  return JitInfo->getArgNext(Args);
}

CorInfoTypeWithMod ReaderBase::getArgType(CORINFO_SIG_INFO *Sig,
                                          CORINFO_ARG_LIST_HANDLE Args,
                                          CORINFO_CLASS_HANDLE *TypeRet) {
  return JitInfo->getArgType(Sig, Args, TypeRet);
}

CORINFO_CLASS_HANDLE
ReaderBase::getArgClass(CORINFO_SIG_INFO *Sig, CORINFO_ARG_LIST_HANDLE Args) {
  return JitInfo->getArgClass(Sig, Args);
}

CORINFO_CLASS_HANDLE
ReaderBase::getBuiltinClass(CorInfoClassId ClassId) {
  return JitInfo->getBuiltinClass(ClassId);
}

CorInfoType ReaderBase::getChildType(CORINFO_CLASS_HANDLE Class,
                                     CORINFO_CLASS_HANDLE *ClassRet) {
  return JitInfo->getChildType(Class, ClassRet);
}

bool ReaderBase::isSDArray(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->isSDArray(Class);
}

uint32_t ReaderBase::getArrayRank(CORINFO_CLASS_HANDLE Class) {
  return JitInfo->getArrayRank(Class);
}

/*++
  Function: rgnRangeIsEnclosedInRegion
    StartOffset : the start offset of the MSIL EH region (handler or trybody)
    EndOffset : the end offset of the MSIL EH region (handler or trybody)
    pRgn : pointer to the region that we would like to check

  Description:
    return true [StartOffset..EndOffset-1] is enclosed in
    [REGION_START_MSIL_OFFSET(pRgn)..REGION_END_MSIL_OFFSET(pRgn))
 --*/
static int rgnRangeIsEnclosedInRegion(uint32_t StartOffset, uint32_t EndOffset,
                                      EHRegion *Region) {
  // ASSERTNR(Region);
  return (rgnGetStartMSILOffset(Region) <= StartOffset) &&
         (EndOffset - 1 < rgnGetEndMSILOffset(Region));
}

/*++
  Function: rgnFindLowestEnclosingRegion
    pRgnTree : the root of the region subtree that we would like to search in
    StartOffset : the start offset of the MSIL EH region (handler or trybody)
    EndOffset : the end offset of the MSIL EH region (handler or trybody)

  Description:
    find the lowest (the region nearest to the leaf) in the region (sub)-tree
    that contains [StartOffset..EndOffset)

--*/
EHRegion *rgnFindLowestEnclosingRegion(EHRegion *RegionTree,
                                       uint32_t StartOffset,
                                       uint32_t EndOffset) {
  EHRegion *TryChild;
  EHRegion *RetVal = nullptr;

  // RegionTree is a non-empty region tree
  // ASSERTNR(RegionTree);
  //
  // assumption (rule #3 and #4), none of the children overlap, so
  // there is at most one child that enclose the region
  //
  // if (one of children of RegionTree contain the range)
  //     return findLowestEnclosingRegion(<child of RegionTree that contain the
  //     range>, start, end);

  EHRegionList *List;
  for (List = rgnGetChildList(RegionTree); List; List = rgnListGetNext(List)) {

    ReaderBaseNS::RegionKind Type;

    TryChild = rgnListGetRgn(List);
    Type = rgnGetRegionType(TryChild);

    if ((Type == ReaderBaseNS::RGN_Try) &&
        rgnRangeIsEnclosedInRegion(StartOffset, EndOffset, TryChild)) {
      RetVal = rgnFindLowestEnclosingRegion(TryChild, StartOffset, EndOffset);
      break;
    } else {
      if (Type == ReaderBaseNS::RGN_Try) {
        // if the try body doesn't contain the range,
        // check if any of its handler (non RGN_TRY) children does
        EHRegion *HandlerChild;
        // ASSERTNR(!RgnRangeIsEnclosedInRegion(StartOffset, EndOffset,
        // TryChild));
        // ASSERTNR(REGION_CHILDREN(TryChild));

        EHRegionList *InnerList;
        for (InnerList = rgnGetChildList(TryChild); InnerList;
             InnerList = rgnListGetNext(InnerList)) {
          HandlerChild = rgnListGetRgn(InnerList);
          // if there are children of type RGN_TRY in TryChild, it
          // should have been covered by TryChild start/end offset
          if ((rgnGetRegionType(HandlerChild) != ReaderBaseNS::RGN_Try) &&
              rgnRangeIsEnclosedInRegion(StartOffset, EndOffset,
                                         HandlerChild)) {
            RetVal = rgnFindLowestEnclosingRegion(HandlerChild, StartOffset,
                                                  EndOffset);
            break;
          }
        }
      } else {
        // not a try region, it should have been checked earlier
        continue;
      }
    }
  }

  // if there are no children, then the current node should be the
  // lowest enclosing region or if there is no child that enclose the
  // region
  if (!RetVal)
    RetVal = RegionTree;

  // ASSERTNR(RetVal);
  return RetVal;
}

// Push a child on the parent.
void ReaderBase::rgnPushRegionChild(EHRegion *Parent, EHRegion *Child) {
  EHRegionList *Element;

  Element = rgnAllocateRegionList();
  rgnListSetRgn(Element, Child);
  rgnListSetNext(Element, rgnGetChildList(Parent));
  rgnSetChildList(Parent, Element);
}

// Allocate a region structure
EHRegion *ReaderBase::rgnMakeRegion(ReaderBaseNS::RegionKind Type,
                                    EHRegion *Parent, EHRegion *RegionRoot,
                                    EHRegionList **AllRegionList) {
  EHRegionList *RegionList = rgnAllocateRegionList();
  EHRegion *Result = rgnAllocateRegion();

  // Push new region onto the AllRegionList.
  rgnListSetNext(RegionList, *AllRegionList);
  rgnListSetRgn(RegionList, Result);
  *AllRegionList = RegionList;

  // Convert from ReaderBaseNS region kind to compiler region kind...
  rgnSetRegionType(Result, Type);
  rgnSetParent(Result, Parent);
  rgnSetChildList(Result, nullptr);

  if (Parent) {
    rgnPushRegionChild(Parent, Result);
  }

  return Result;
}

/*

These are rules to follow when setting up the EIT:
1. Ordering: The handlers should appear with the most nested handlers
   at the top to the outermost handlers at the bottom.
2. Nesting: If one handler protects any region protected by another
   handler it must protect that entire handler including its try, catch,
   filter and fault blocks (that is if you protect a try block, you must also
protect the catch).
3. Nesting: A try block can not include its own filter, catch or finally blocks
4. Nesting: A single Handler must constitute a contiguous block of IL
instructions which
   cannot overlap those of another handler (A try may enclose another handler
completely).

*/

/*++

  Function: rgnCreateRegionTree
    pEHClauses : EIT clauses
    count : number of EIT clauses
    pRgnTree : an empty region tree, containing only RGN_ROOT node
    ciPtr : compiler instance

  Description:
    given a region tree containing one node (RGN_ROOT) and an EIT from runtime
that satisfies
    the rules above, create the region tree

    REGION_HEAD and REGION_LAST won't be set here since we don't have the IR.
yet
    ALL information from the EIT will be transferred to the region tree
    after this function is finished, we can discard the EIT

--*/

#ifndef NDEBUG

const char *const RegionTypeNames[] = {
    "RGN_ROOT",   "RGN_TRY",     "RGN_FAULT", "RGN_FINALLY",
    "RGN_FILTER", "RGN_MEXCEPT", "RGN_MCATCH"};

void dumpRegion(EHRegion *Region, int Indent = 0) {
  EHRegionList *RegionList;

  if (Indent == 0)
    dbPrint("\n<---------------\n");

  for (int I = 0; I < Indent; I++)
    dbPrint(" ");

  dbPrint("Region type=%s ", RegionTypeNames[rgnGetRegionType(Region)]);

  dbPrint("msilstart=%x msilend=%x\n", rgnGetStartMSILOffset(Region),
          rgnGetEndMSILOffset(Region));

  RegionList = rgnGetChildList(Region);

  while (RegionList) {
    Region = rgnListGetRgn(RegionList);
    dumpRegion(Region, Indent + 4);
    RegionList = rgnListGetNext(RegionList);
  }

  if (Indent == 0)
    dbPrint("----dump done-----\n\n");
}
#endif // NDEBUG

static bool clauseXInsideY(const CORINFO_EH_CLAUSE *X,
                           const CORINFO_EH_CLAUSE *Y) {
  // is X inside Y?

  // note for both checks:
  // either start OR stop offset may be same, but if both are same, no nesting
  // relationship exists
  // 1. is x's try inside y's try?
  if (X->TryOffset >= Y->TryOffset &&
      X->TryOffset + X->TryLength <= Y->TryOffset + Y->TryLength &&
      X->TryLength != Y->TryLength)
    return true;

  // 2. is x's try inside y's handler?
  if (X->TryOffset >= Y->HandlerOffset &&
      X->TryOffset + X->TryLength <= Y->HandlerOffset + Y->HandlerLength &&
      X->TryLength != Y->HandlerLength)
    return true;

  return false;
}

#ifdef _DEBUG
int __cdecl clauseSortFunction(const void *C1, const void *C2) {
  const CORINFO_EH_CLAUSE *Clause1 = *(CORINFO_EH_CLAUSE * const *)C1;
  const CORINFO_EH_CLAUSE *Clause2 = *(CORINFO_EH_CLAUSE * const *)C2;

  if (clauseXInsideY(Clause1, Clause2)) {
    return -1;
  }
  if (clauseXInsideY(Clause2, Clause1)) {
    return 1;
  } else {
    // IMPORTANT: if there is no nesting relationship between two,
    // we sort on the pointer to the clause itself, preserving the EIT ordering.
    // EIT order is significant in these cases and we cannot just ignore it.
    return C2 > C1 ? -1 : 1;
  }
}
#endif

bool clauseLessThan(const CORINFO_EH_CLAUSE *Lhs,
                    const CORINFO_EH_CLAUSE *Rhs) {
  if (clauseXInsideY(Lhs, Rhs))
    return true;
  return false;
}

void ReaderBase::rgnCreateRegionTree(void) {

  if (MethodInfo->EHcount == 0) {
    // No EH in this method. Clear out the reader EH state.
    EhRegionTree = nullptr;
    AllRegionList = nullptr;
    return;
  }

  // Get the raw EH clause data
  CORINFO_EH_CLAUSE *EHClauses = new CORINFO_EH_CLAUSE[MethodInfo->EHcount];
  for (uint32_t I = 0; I < MethodInfo->EHcount; I++) {
    JitInfo->getEHinfo(getCurrentMethodHandle(), I, &(EHClauses[I]));
  }

  CORINFO_EH_CLAUSE **ClauseList =
      (CORINFO_EH_CLAUSE **)alloca(sizeof(void *) * MethodInfo->EHcount);

  // ClauseList is an array of pointers into EIT
  for (uint32_t J = 0; J < MethodInfo->EHcount; J++)
    ClauseList[J] = &EHClauses[J];

  // now clauselist is sorted w/ inner regions first
  // qsort(ClauseList, MethodInfo->EHcount, sizeof(void*),
  // clauseSortFunction);
  for (uint32_t J = 1; J < MethodInfo->EHcount; ++J) {
    CORINFO_EH_CLAUSE *Key = ClauseList[J];
    uint32_t I = 0;
    for (; I < J && !clauseLessThan(Key, ClauseList[I]); ++I)
      ;
    if (I != J) {
      memmove(&ClauseList[I + 1], &ClauseList[I],
              (J - I) * sizeof(*ClauseList));
      ClauseList[I] = Key;
    }
  }

  for (uint32_t I = 0; I < MethodInfo->EHcount - 1; I++) {
    for (uint32_t J = I + 1; J < MethodInfo->EHcount; ++J) {
      ASSERTNR(clauseSortFunction(&ClauseList[I], &ClauseList[J]) == -1);
    }
  }

  // TODO: Find a way to enable the EIT dumper.
  // IMetaPrintCorInfoEHClause(EHClauses, MethodInfo->EHcount));

  // Create root region.
  EHRegionList *WorkingAllRegionList = nullptr;
  EHRegion *RegionTreeRoot = rgnMakeRegion(ReaderBaseNS::RGN_Root, nullptr,
                                           nullptr, &WorkingAllRegionList);
  rgnSetEndMSILOffset(RegionTreeRoot, MethodInfo->ILCodeSize);
  EHRegion *RegionTree = RegionTree = RegionTreeRoot;

  // Map the clause information into try regions for later processing
  // We need to map the EIT into the tryregion DAG as we need to
  // maintain information as to where the END of all the constructs are
  // "Just cause its an leave does not mean its the end of a region"
  //
  // Start from bottom to the top, insert things into the current tree
  // the current tree should be initialized to contain one node.

  uint16_t I = MethodInfo->EHcount;

  do {
    I--;

    CORINFO_EH_CLAUSE *CurrentEHClause = ClauseList[I];

    EHRegion *EnclosingRegion = rgnFindLowestEnclosingRegion(
        RegionTree, CurrentEHClause->TryOffset,
        CurrentEHClause->TryOffset + CurrentEHClause->TryLength);

    EHRegion *RegionTry = nullptr;
    EHRegion *RegionHandler = nullptr;

    ASSERTNR(CurrentEHClause);

    if ((rgnGetRegionType(EnclosingRegion) == ReaderBaseNS::RGN_Try) &&
        (CurrentEHClause->TryOffset ==
         rgnGetStartMSILOffset(EnclosingRegion)) &&
        ((CurrentEHClause->TryOffset + CurrentEHClause->TryLength) ==
         rgnGetEndMSILOffset(EnclosingRegion))) {

      // EnclosingRegion is a try region that is described by CurrentEHClause

      // try region already exists
      // (this is the case if there are multiple handlers for one try Block)
      // try region should be the parent of the handler region

      RegionTry = EnclosingRegion;
    } else {
      // create a new try region, make it a child of EnclosingRegion
      RegionTry = rgnMakeRegion(ReaderBaseNS::RGN_Try, EnclosingRegion,
                                RegionTreeRoot, &WorkingAllRegionList);

      rgnSetStartMSILOffset(RegionTry, CurrentEHClause->TryOffset);
      rgnSetEndMSILOffset(RegionTry, CurrentEHClause->TryOffset +
                                         CurrentEHClause->TryLength);
      rgnSetEntryRegion(RegionTry, RegionTry);
    }

    EHRegion *CheckEntryRegion;
    if (CurrentEHClause->Flags & CORINFO_EH_CLAUSE_FILTER) {
      EHRegion *RegionFilter;
      RegionHandler = rgnMakeRegion(ReaderBaseNS::RGN_MExcept, RegionTry,
                                    RegionTreeRoot, &WorkingAllRegionList);

      RegionFilter = rgnMakeRegion(ReaderBaseNS::RGN_Filter, RegionTry,
                                   RegionTreeRoot, &WorkingAllRegionList);

      rgnSetFilterHandlerRegion(RegionFilter, RegionHandler);

      rgnSetStartMSILOffset(RegionFilter, CurrentEHClause->FilterOffset);
      // The end of the filter, is the start of its handler
      rgnSetEndMSILOffset(RegionFilter, CurrentEHClause->HandlerOffset);
      // The filter might precede the try and its other children; the handler
      // will never precede the try.
      CheckEntryRegion = RegionFilter;
    } else {
      if (CurrentEHClause->Flags & CORINFO_EH_CLAUSE_FINALLY) {
        RegionHandler = rgnMakeRegion(ReaderBaseNS::RGN_Finally, RegionTry,
                                      RegionTreeRoot, &WorkingAllRegionList);
      } else if (CurrentEHClause->Flags & CORINFO_EH_CLAUSE_FAULT) {

        RegionHandler = rgnMakeRegion(ReaderBaseNS::RGN_Fault, RegionTry,
                                      RegionTreeRoot, &WorkingAllRegionList);
      } else {
        // we need to touch the class at JIT time
        // otherwise the classloader kicks in at exception time
        // (possibly stack overflow exception) in which case
        // we are in danger of going past the stack guard

        if (CurrentEHClause->ClassToken) {
          CORINFO_RESOLVED_TOKEN ResolvedToken;
          resolveToken(CurrentEHClause->ClassToken, CORINFO_TOKENKIND_Class,
                       &ResolvedToken);
        }

        // this will be a catch (EH_CLAUSE_NONE)
        // we need to keep the token somewhere
        RegionHandler = rgnMakeRegion(ReaderBaseNS::RGN_MCatch, RegionTry,
                                      RegionTreeRoot, &WorkingAllRegionList);

        rgnSetCatchClassToken(RegionHandler, CurrentEHClause->ClassToken);
      }
      // The handler might precede the try and its other children
      CheckEntryRegion = RegionHandler;
    }
    rgnSetStartMSILOffset(RegionHandler, CurrentEHClause->HandlerOffset);
    rgnSetEndMSILOffset(RegionHandler, CurrentEHClause->HandlerOffset +
                                           CurrentEHClause->HandlerLength);
    // See if this handler precedes the try and its other handlers
    EHRegion *EntryRegion = rgnGetEntryRegion(RegionTry);
    if (rgnGetStartMSILOffset(CheckEntryRegion) <
        rgnGetStartMSILOffset(EntryRegion)) {
      rgnSetEntryRegion(RegionTry, CheckEntryRegion);
    }
  } while (I != 0);

  delete[] EHClauses;

  // Propagate EH state to the reader.
  AllRegionList = WorkingAllRegionList;
  EhRegionTree = RegionTreeRoot;
}

// Builds flow graph from bytecode and initializes blocks for DFO traversal.
FlowGraphNode *ReaderBase::buildFlowGraph(FlowGraphNode **FgTail) {
  // Build a flow graph from the byte codes
  FlowGraphNode *HeadBlock =
      fgBuildBasicBlocksFromBytes(MethodInfo->ILCode, MethodInfo->ILCodeSize);
  *FgTail = fgGetTailBlock();
  // Return head FlowGraphNode.
  return HeadBlock;
}

// fgAddNodeMSILOffset
//
//  The FlowGraphNodeOffsetList acts as a work list. Each time a
//  branch is added to the the IR stream a temporary target node is
//  added to the FlowGraphNodeOffsetList.  After all the branches have
//  been added the worklist is traversed and each temporary node is
//  replaced with a real one.
//
//  During reader flow graph building the first argument must always
//  be a valid pointer. After the function call this pointer will
//  point to either a new temporary FlowGraphNode if there was no
//  previous node at the given offset (the second argument) or it will
//  point to the node stored in the NodeOffsetListArray at the given
//  offset. For new nodes the function sets the MSIL offset of the
//  node.  New entries into the NodeOffsetListArray are inserted
//  into NodeOffsetListArray (in ReaderBase). These entries are
//  inserted in order of their MSIL offset for easy retrieval. A
//  pointer to the new entry is then returned.
//
FlowGraphNodeOffsetList *ReaderBase::fgAddNodeMSILOffset(
    FlowGraphNode **Node, // A pointer to FlowGraphNode* node
    uint32_t TargetOffset // The MSIL offset of the node
    ) {
  FlowGraphNodeOffsetList *Element, *PreviousElement, *NewElement;
  uint32_t Index;

  // Check to see if we already have this offset
  PreviousElement = nullptr;

  Index = TargetOffset / FLOW_GRAPH_NODE_LIST_ARRAY_STRIDE;
  Element = NodeOffsetListArray[Index];

  while (Element) {
    if (Element->getOffset() == TargetOffset) {
      if (Node) {
        *Node = Element->getNode();
      }
      return Element;
    } else if (Element->getOffset() > TargetOffset) {
      // we must insert offsets in order, so since we've
      //  passed the offset we're looking for we're done
      break;
    }
    PreviousElement = Element;
    Element = Element->getNext();
  }

  // We need to create a new label
  NewElement =
      (FlowGraphNodeOffsetList *)getTempMemory(sizeof(FlowGraphNodeOffsetList));
  NewElement->setOffset(TargetOffset);

  if (*Node == nullptr) {
    *Node = makeFlowGraphNode(TargetOffset, nullptr);
  }
  NewElement->setNode(*Node);

  // Insert the new Element at the right spot
  if (PreviousElement) {
    NewElement->setNext(Element);
    PreviousElement->setNext(NewElement);
  } else {
    NewElement->setNext(NodeOffsetListArray[Index]);
    NodeOffsetListArray[Index] = NewElement;
  }

  return NewElement;
}

void ReaderBase::fgDeleteBlockAndNodes(FlowGraphNode *Block) {

  // TODO: decide if we want to rewrite this code.  Our implementation
  // of DeleteNodesFromBlock also deletes the successors.

  fgDeleteNodesFromBlock(Block);

  for (FlowGraphEdgeIterator SuccessorIterator = fgNodeGetSuccessors(Block);
       !fgEdgeIteratorIsEnd(SuccessorIterator);
       fgEdgeIteratorMoveNextSuccessor(SuccessorIterator)) {
    fgDeleteEdge(SuccessorIterator);
  }

  for (FlowGraphEdgeIterator PredecessorIterator = fgNodeGetPredecessors(Block);
       !fgEdgeIteratorIsEnd(PredecessorIterator);
       fgEdgeIteratorMoveNextPredecessor(PredecessorIterator)) {
    fgDeleteEdge(PredecessorIterator);
  }

  fgDeleteBlock(Block);
}

// removeUnusedBlocks
// - Iterate from fgHead to the end of the list of nodes using FgNodeGetNext
// - fgHead must have been visited
// - clear visited bit on remaining blocks
void ReaderBase::fgRemoveUnusedBlocks(FlowGraphNode *FgHead) {
  FlowGraphNode *Block;

  // Remove Unused Blocks
  for (Block = FgHead; Block != nullptr;) {
    FlowGraphNode *NextBlock;
    NextBlock = fgNodeGetNext(Block);

    if (!fgNodeIsVisited(Block)) {
      // TODO - possibly more cleanup checking is warranted.
      // Also need to issue warning when nontrivial code is removed.
      fgDeleteBlockAndNodes(Block);
    }
    Block = NextBlock;
  }
}

// fgSplitBlock
//
// Common block split routine.  Splits a block in the flow graph and
// correctly updates the starting/ending offset fields of the
// block. Additionally the the function sets the region of the block.
// Actual flow graph manipulation is performed by FgSplitBlock,
// which is not common.
FlowGraphNode *ReaderBase::fgSplitBlock(FlowGraphNode *Block,
                                        uint32_t CurrentOffset, IRNode *Node) {
  FlowGraphNode *NewBlock;
  uint32_t OldEndOffset;

  // Save off the ending bytes offset so that we can set it on the second block.
  OldEndOffset = fgNodeGetEndMSILOffset(Block);

  // Modify the old block info
  fgNodeSetEndMSILOffset(Block, CurrentOffset);

  if ((Node == nullptr) && (CurrentOffset == MethodInfo->ILCodeSize)) {
    // Avoid creating an empty block at the end of the method.
    return Block;
  }

  // Split the previous block along the given tuple using GenIR routine.
  NewBlock = fgSplitBlock(Block, Node);

  // Set the correct offsets for the new block.
  fgNodeSetStartMSILOffset(NewBlock, CurrentOffset);

  fgNodeSetEndMSILOffset(NewBlock, OldEndOffset);

  // Set the EH region
  fgNodeSetRegion(NewBlock, fgNodeGetRegion(Block));

  // Init operand stack to nullptr.
  fgNodeSetOperandStack(NewBlock, nullptr);

  // Return the new block
  return NewBlock;
}

// fgReplaceBranchTarget
//
// Given a temporary branch target and its offset this function
// searches the flow graph for the real basic block that this offset
// lives in.  It then replaces all uses of the temporary branch target
// with the real one and deletes the temporary branch target.
FlowGraphNode *
ReaderBase::fgReplaceBranchTarget(uint32_t Offset,
                                  FlowGraphNode *TempBranchTarget,
                                  FlowGraphNode *StartBlock) {
  FlowGraphNode *Block, *NextBlock;

  if (StartBlock == nullptr) {
    StartBlock = fgGetHeadBlock();
  }

  bool FoundTargetBlock = false;

  // Iterate over all blocks until block that contains the offset is found.
  for (Block = StartBlock; Block != nullptr; Block = NextBlock) {

    uint32_t Start, End;

    NextBlock = fgNodeGetNext(Block);

    Start = fgNodeGetStartMSILOffset(Block);
    End = fgNodeGetEndMSILOffset(Block);

    // Find the MSIL block corresponding to the branch target.  Note that the
    // test used here precludes selecting a point block as the branch target;
    // point blocks created in the first pass are only reachable by branches
    // explicitly made to target them in the first pass.
    if (Offset >= Start && Offset < End) {

      // Branch targets must be at the begining of basic blocks. Thus,
      // if this branch target does not Start the block we must split
      // the block along the target.
      if (Offset != Start) {
        Block = fgSplitBlock(Block, Offset, fgNodeGetEndInsertIRNode(Block));
      }

      // Found block that contains the offset, now replace the targets in
      // the branch instructions.
      replaceFlowGraphNodeUses(TempBranchTarget, Block);

      FoundTargetBlock = true;

      break;
    }
  }

  ASSERTNR(FoundTargetBlock);

  return Block;
}

int __cdecl labelSortFunction(const void *C1, const void *C2) {
  uint32_t O1, O2;

  O1 = ((const FlowGraphNodeOffsetList *)C1)->getOffset();
  O2 = ((const FlowGraphNodeOffsetList *)C2)->getOffset();

  if (O1 < O2)
    return -1;
  return (O1 > O2);
}

// Insert labels from label offset list into block stream, splitting
// blocks if necessary.  The list is currently ordered.
void ReaderBase::fgReplaceBranchTargets() {
  FlowGraphNodeOffsetList *List;
  FlowGraphNode *Block;
  uint32_t Index;

  Block = nullptr;

  for (Index = 0; Index < NodeOffsetListArraySize; Index++) {
    for (List = NodeOffsetListArray[Index]; List != nullptr;
         List = List->getNext()) {
      Block = fgReplaceBranchTarget(List->getOffset(), List->getNode(), Block);
    }
  }
}

EHRegion *ReaderBase::getInnermostFaultOrFinallyRegion(uint32_t Offset) {
  EHRegion *HandlerRegion = nullptr;
  // Walk from outer to inner regions
  for (EHRegion *TestRegion = EhRegionTree; TestRegion != nullptr;
       TestRegion = getInnerEnclosingRegion(TestRegion, Offset)) {
    // TestRegion encloses Offset
    if ((rgnGetRegionType(TestRegion) == ReaderBaseNS::RGN_Fault) ||
        (rgnGetRegionType(TestRegion) == ReaderBaseNS::RGN_Finally)) {
      // Found a fault or finally region nested inside our previous best
      HandlerRegion = TestRegion;
    }
  }

  return HandlerRegion;
}

EHRegion *ReaderBase::getInnerEnclosingRegion(EHRegion *OuterRegion,
                                              uint32_t Offset) {
  // Search the immediate children of the given region
  for (EHRegionList *ChildNode = rgnGetChildList(OuterRegion); ChildNode;
       ChildNode = rgnListGetNext(ChildNode)) {

    EHRegion *Child = rgnListGetRgn(ChildNode);

    if ((Offset < rgnGetEndMSILOffset(Child)) &&
        (Offset >= rgnGetStartMSILOffset(Child))) {
      // This offset is in this child region.
      return Child;
    }

    if (rgnGetRegionType(Child) == ReaderBaseNS::RegionKind::RGN_Try) {
      // A handler region for the try is a child of it in the tree but follows
      // it in the IR, so we explicitly have to check for grandchildren in this
      // case (the current offset falls in the grandchild's range but not the
      // child's range).
      for (EHRegionList *GrandchildNode = rgnGetChildList(Child);
           GrandchildNode; GrandchildNode = rgnListGetNext(GrandchildNode)) {

        EHRegion *Grandchild = rgnListGetRgn(GrandchildNode);

        if ((Offset < rgnGetEndMSILOffset(Grandchild)) &&
            (Offset >= rgnGetStartMSILOffset(Grandchild))) {
          // This offset is in this grandchild region
          return Grandchild;
        }
      }
    }
  }

  // No inner region encloses this offset
  return nullptr;
}

IRNode *ReaderBase::fgAddCaseToCaseListHelper(IRNode *SwitchNode,
                                              IRNode *LabelNode,
                                              uint32_t Element) {
  IRNode *CaseNode;

  CaseNode = fgAddCaseToCaseList(SwitchNode, LabelNode, Element);
  fgAddLabelToBranchList(LabelNode, CaseNode);
  return CaseNode;
}

IRNode *ReaderBase::fgMakeBranchHelper(IRNode *LabelNode, IRNode *BlockNode,
                                       uint32_t CurrentOffset,
                                       bool IsConditional, bool IsNominal) {
  IRNode *BranchNode;

  BranchNode = fgMakeBranch(LabelNode, BlockNode, CurrentOffset, IsConditional,
                            IsNominal);
  fgAddLabelToBranchList(LabelNode, BranchNode);
  return BranchNode;
}

// getRegionFromOffset
//
// Do a linear scan of the EH regions in a function looking for the
// the smallest possible region that contains the given offset.
//
// If two regions start at the same offset then we don't know which
// one to use. We just use the inner one. This mistake is then
// corrected when we add in the region start IR.
EHRegion *ReaderBase::fgGetRegionFromMSILOffset(uint32_t Offset) {
  EHRegionList *RegionList;
  EHRegion *Region, *CandidateRegion;
  uint32_t CandidateRegionSize;

  CandidateRegionSize = UINT_MAX;

  // Search each child region for a match
  // TODO: traverse tree instead of list.
  for (RegionList = AllRegionList, CandidateRegion = EhRegionTree;
       RegionList != nullptr; RegionList = rgnListGetNext(RegionList)) {
    uint32_t StartOffset, EndOffset;

    Region = rgnListGetRgn(RegionList);

    if (Offset >= (StartOffset = rgnGetStartMSILOffset(Region)) &&
        Offset < (EndOffset = rgnGetEndMSILOffset(Region))) {
      uint32_t RegionSize;

      RegionSize = EndOffset - StartOffset;
      if (RegionSize <= CandidateRegionSize) {
        CandidateRegion = Region;
        CandidateRegionSize = RegionSize;
      }
    }
  }

  return CandidateRegion;
}

// GetMSILInstrStackDelta - Returns the change in the number of items
// on the evaluation stack due to the given instruction.  This is
// accomplished via a lookup table for most operations, with special
// cases for CEE_CALL, CEE_CALLI, CEE_CALLVIRT, CEE_NEWOBJ, CEE_RET
//
// Note that any pops occur before pushes, so for underflow detection
// it is necessary to have distinct values for pushes and pops.
void ReaderBase::getMSILInstrStackDelta(ReaderBaseNS::OPCODE Opcode,
                                        uint8_t *Operand, uint16_t *Pop,
                                        uint16_t *Push) {
  static const int8_t StackPopMap[] = {
#define OPDEF_HELPER OPDEF_POPCOUNT
#include "ophelper.def"
#undef OPDEF_HELPER
  };

  static const int8_t StackPushMap[] = {
#define OPDEF_HELPER OPDEF_PUSHCOUNT
#include "ophelper.def"
#undef OPDEF_HELPER
  };

  int NumPop, NumPush;

  NumPop = 0;

  switch (Opcode) {
  case ReaderBaseNS::CEE_CALLI:
    NumPop++; // indirect involves an extra stack pop
              // intentional fall-through
  case ReaderBaseNS::CEE_CALL:
  case ReaderBaseNS::CEE_CALLVIRT:
  case ReaderBaseNS::CEE_NEWOBJ: {
    CORINFO_METHOD_HANDLE Handle;
    CORINFO_SIG_INFO Sig;
    bool HasThis, ReturnsVoid;
    mdToken Token;

    Token = readValue<mdToken>(Operand);

    if (verIsCallToken(Token) &&
        // if calli - verifier is going to reject this anyway and the
        // site signature lookup is unsafe
        (Opcode != ReaderBaseNS::CEE_CALLI || !VerificationNeeded) &&
        JitInfo->isValidToken(getCurrentModuleHandle(), Token)) {
      if (Opcode != ReaderBaseNS::CEE_CALLI) {
        CORINFO_RESOLVED_TOKEN ResolvedToken;
        resolveToken(Token, CORINFO_TOKENKIND_Method, &ResolvedToken);
        Handle = ResolvedToken.hMethod;
      } else
        Handle = nullptr;

      getCallSiteSignature(Handle, Token, &Sig, &HasThis);
      ReturnsVoid = (Sig.retType == CORINFO_TYPE_VOID);

      NumPop += (Sig.numArgs + (HasThis ? 1 : 0));
      NumPush = (ReturnsVoid ? 0 : 1);
    } else {
      // "bad token" error will show up later, global verify
      // should not complain.
      NumPop = 0;
      NumPush = 0;
    }
  } break;
  case ReaderBaseNS::CEE_RET: {
    CORINFO_SIG_INFO Sig;

    JitInfo->getMethodSig(getCurrentMethodHandle(), &Sig);
    NumPop = ((Sig.retType == CORINFO_TYPE_VOID) ? 0 : 1);
    NumPush = 0;
  } break;
  default:
    NumPop = StackPopMap[Opcode - ReaderBaseNS::CEE_NOP];
    NumPush = StackPushMap[Opcode - ReaderBaseNS::CEE_NOP];

    break;
  }

  (*Pop) = NumPop;
  (*Push) = NumPush;
}

EHRegion *getFinallyRegion(EHRegion *TryRegion) {
  // Look for the finally region.  It will be an immediate child of the try.
  for (EHRegionList *ChildNode = rgnGetChildList(TryRegion); ChildNode;
       ChildNode = rgnListGetNext(ChildNode)) {
    EHRegion *Child = rgnListGetRgn(ChildNode);
    if (rgnGetRegionType(Child) == ReaderBaseNS::RegionKind::RGN_Finally) {
      return Child;
    }
  }

  return nullptr;
}

EHRegion *ReaderBase::fgSwitchRegion(EHRegion *OldRegion, uint32_t Offset,
                                     uint32_t *NextOffset) {
  uint32_t TransitionOffset = rgnGetEndMSILOffset(OldRegion);
  if (Offset >= TransitionOffset) {
    // Exit this region; recursively check parent (may need to exit to ancestor
    // and/or may need to enter sibling/cousin).
    assert(Offset == TransitionOffset && "over-stepped region end");
    EHRegion *ParentRegion = rgnGetEnclosingAncestor(OldRegion);
    return fgSwitchRegion(ParentRegion, Offset, NextOffset);
  }

  for (EHRegionList *ChildNode = rgnGetChildList(OldRegion); ChildNode;
       ChildNode = rgnListGetNext(ChildNode)) {
    EHRegion *ChildRegion = rgnListGetRgn(ChildNode);

    uint32_t ChildStartOffset = rgnGetStartMSILOffset(ChildRegion);
    if (Offset < ChildStartOffset) {
      // Haven't reached this child yet; consider it a potential next
      // transition

      if (ChildStartOffset < TransitionOffset) {
        TransitionOffset = ChildStartOffset;
      }
    } else if (Offset < rgnGetEndMSILOffset(ChildRegion)) {
      if (ChildRegion == rgnGetEntryRegion(ChildRegion)) {
        // This try region lexically precedes all its handlers (which is the
        // common case).  Let the client do any processing necessary for its
        // entry.
        fgEnterRegion(ChildRegion);
      }
      // Switch to the child and recursively check (we may need to enter some
      // descendant(s) of it as well).
      return fgSwitchRegion(ChildRegion, Offset, NextOffset);
    }

    if (rgnGetRegionType(ChildRegion) == ReaderBaseNS::RegionKind::RGN_Try) {
      // A handler region for the try is a child of it in the tree but follows
      // (or, in some hand-crafted IL cases, precedes) it in the IR, so we
      // explicitly have to check for grandchildren in this case (the current
      // offset falls in the grandchild's range but not the child's range).
      for (EHRegionList *GrandchildNode = rgnGetChildList(ChildRegion);
           GrandchildNode; GrandchildNode = rgnListGetNext(GrandchildNode)) {

        EHRegion *Grandchild = rgnListGetRgn(GrandchildNode);

        uint32_t GrandchildStartOffset = rgnGetStartMSILOffset(Grandchild);
        if (Offset < GrandchildStartOffset) {
          // Haven't reached this child yet; consider it a potential next
          // transition

          if (GrandchildStartOffset < TransitionOffset) {
            TransitionOffset = GrandchildStartOffset;
          }

          continue;
        }

        uint32_t GrandchildEndOffset = rgnGetEndMSILOffset(Grandchild);

        if (Offset < GrandchildEndOffset) {
          if (Grandchild == rgnGetEntryRegion(ChildRegion)) {
            // This handler lexically precedes its try.  Give the client a
            // chance to set up any necessary state for the try before
            // switching to the handler.
            fgEnterRegion(ChildRegion);
          }
          // Switch to this region and recursively check -- we may need to
          // enter a some descendant(s) of it as well.
          return fgSwitchRegion(Grandchild, Offset, NextOffset);
        }
      }
    }
  }

  // Not exiting OldRegion or entering child; just report the transition offset
  *NextOffset = TransitionOffset;
  return OldRegion;
}

#define CHECKTARGET(TargetOffset, BufSize)                                     \
  {                                                                            \
    if ((TargetOffset) < 0 || (TargetOffset) >= (BufSize))                     \
      ReaderBase::verGlobalError(MVER_E_BAD_BRANCH);                           \
  }

// Parse bytecode to blocks.  Incoming argument 'block' holds dummy
// entry block. This entry block may be preceeded by another block
// that holds IRNodes (to support monitored routines.)  When this
// function is finished we have a flow graph with each fg node holding
// the block's start and end MSIL offset, its fg successors and
// predecessors; we also have a list of all labels in the
// function. These labels are inserted in the next pass.
void ReaderBase::fgBuildPhase1(FlowGraphNode *Block, uint8_t *ILInput,
                               uint32_t ILInputSize) {
  IRNode *BranchNode, *BlockNode;
  FlowGraphNode *GraphNode;
  uint32_t CurrentOffset, BranchOffset, TargetOffset, NextOffset, NumCases;
  uint32_t NextRegionTransitionOffset;
  bool IsShortInstr, IsConditional, IsTailCall, IsReadOnly, PreviousWasPrefix;
  mdToken TokenConstrained;
  uint32_t StackOffset = 0;
  ReaderBaseNS::OPCODE Opcode = ReaderBaseNS::CEE_ILLEGAL;

  // If we're doing verification build up a bit vector of legal branch targets
  if (VerificationNeeded) {
    // empty IL is a verification error.  this is the trivial case of
    // 'do not allow control to flow off the end of a func'
    if (ILInputSize == 0) {
      BADCODE(MVER_E_CODE_SIZE_ZERO);
    }

    LegalTargetOffsets =
        (ReaderBitVector *)getTempMemory(sizeof(ReaderBitVector));

    // Add 1 so that there is enough room for the offset after the
    // last instruction (asycronous flow can target this)
    LegalTargetOffsets->allocateBitVector(ILInputSize + 1, this);

    GvStackPush = (uint16_t *)getTempMemory(ILInputSize * sizeof(uint16_t));
    GvStackPop = (uint16_t *)getTempMemory(ILInputSize * sizeof(uint16_t));
    StackOffset = 0;
  }

  // init stuff prior to loop
  IsShortInstr = false;
  IsConditional = false;
  IsTailCall = false;
  IsReadOnly = false;
  PreviousWasPrefix = false;
  TokenConstrained = mdTokenNil;
  BranchesToVerify = nullptr;
  HasLocAlloc = false;
  HasAddressTaken = false;
  NextRegionTransitionOffset = NextOffset = CurrentOffset = 0;

  // Keep going through the buffer of bytecodes until we get to the end.
  while (CurrentOffset < ILInputSize) {
    if ((EhRegionTree != nullptr) &&
        (CurrentOffset == NextRegionTransitionOffset)) {

      CurrentRegion = fgSwitchRegion(CurrentRegion, CurrentOffset,
                                     &NextRegionTransitionOffset);

      if (fgNodeGetStartMSILOffset(Block) != CurrentOffset) {
        // Split the block so we can maintain a many-to-one block-to-region
        // mapping.
        Block = fgSplitBlock(Block, CurrentOffset, nullptr);
      }
      // Update the region on the current block (it will have inherited the
      // previous block's region when it was split off from it).
      fgNodeChangeRegion(Block, CurrentRegion);
    }
    uint8_t *Operand;
    NextOffset = parseILOpcode(ILInput, CurrentOffset, ILInputSize, this,
                               &Opcode, &Operand);

    // If we're doing verification, build up a bit vector of legal
    // branch targets.  note : the instruction following a prefix is
    // not a valid branch target.
    if (VerificationNeeded && !PreviousWasPrefix) {
      LegalTargetOffsets->setBit(CurrentOffset);
    }

    VerInstrStartOffset = CurrentOffset;

    PreviousWasPrefix = false;

    switch (Opcode) {
    case ReaderBaseNS::CEE_BEQ_S:
    case ReaderBaseNS::CEE_BGE_S:
    case ReaderBaseNS::CEE_BGE_UN_S:
    case ReaderBaseNS::CEE_BGT_S:
    case ReaderBaseNS::CEE_BGT_UN_S:
    case ReaderBaseNS::CEE_BLE_S:
    case ReaderBaseNS::CEE_BLE_UN_S:
    case ReaderBaseNS::CEE_BLT_S:
    case ReaderBaseNS::CEE_BLT_UN_S:
    case ReaderBaseNS::CEE_BNE_UN_S:
    case ReaderBaseNS::CEE_BRFALSE_S:
    case ReaderBaseNS::CEE_BRTRUE_S:
    case ReaderBaseNS::CEE_BR_S:
    case ReaderBaseNS::CEE_LEAVE_S:
      IsShortInstr = true;
    /* Fall Through */
    case ReaderBaseNS::CEE_BEQ:
    case ReaderBaseNS::CEE_BGE:
    case ReaderBaseNS::CEE_BGE_UN:
    case ReaderBaseNS::CEE_BGT:
    case ReaderBaseNS::CEE_BLE:
    case ReaderBaseNS::CEE_BLT:
    case ReaderBaseNS::CEE_BGT_UN:
    case ReaderBaseNS::CEE_BLE_UN:
    case ReaderBaseNS::CEE_BLT_UN:
    case ReaderBaseNS::CEE_BNE_UN:
    case ReaderBaseNS::CEE_BRFALSE:
    case ReaderBaseNS::CEE_BRTRUE:
    case ReaderBaseNS::CEE_BR:
    case ReaderBaseNS::CEE_LEAVE:
      if (Opcode != ReaderBaseNS::CEE_BR && Opcode != ReaderBaseNS::CEE_LEAVE &&
          Opcode != ReaderBaseNS::CEE_BR_S &&
          Opcode != ReaderBaseNS::CEE_LEAVE_S) {
        IsConditional = true;
      }

      if (IsShortInstr) {
        BranchOffset = readValue<int8_t>(Operand);
      } else {
        BranchOffset = readValue<int32_t>(Operand);
      }

      // Make the label node
      TargetOffset = NextOffset + BranchOffset;
      CHECKTARGET(TargetOffset, ILInputSize);

      if (Opcode == ReaderBaseNS::CEE_LEAVE ||
          Opcode == ReaderBaseNS::CEE_LEAVE_S) {
        TargetOffset =
            updateLeaveOffset(CurrentOffset, NextOffset, Block, TargetOffset);
      }

      GraphNode = nullptr;
      fgAddNodeMSILOffset(&GraphNode, TargetOffset);

      ASSERTNR(GraphNode != nullptr);

      // Make branch node
      BlockNode = fgNodeGetStartIRNode(Block);
      BranchNode = fgMakeBranchHelper((IRNode *)GraphNode, BlockNode,
                                      CurrentOffset, IsConditional, false);

      // record a branch
      verifyRecordBranchForVerification(BranchNode, CurrentOffset, TargetOffset,
                                        Opcode == ReaderBaseNS::CEE_LEAVE ||
                                            Opcode ==
                                                ReaderBaseNS::CEE_LEAVE_S);

      // split the block
      fgNodeSetEndMSILOffset(Block, NextOffset);

      Block = fgSplitBlock(Block, NextOffset, nullptr);

      // Reset flags
      IsConditional = false;
      IsShortInstr = false;
      break;

    case ReaderBaseNS::CEE_SWITCH:
      // Get the count of cases.
      NumCases = readNumberOfSwitchCases(&Operand);

      // If there are no cases, we can ignore the switch statement
      if (NumCases == 0) {
        break;
      }

      // Make the short-circuit target label
      BlockNode = fgNodeGetStartIRNode(Block);
      GraphNode = nullptr;
      CHECKTARGET(NextOffset, ILInputSize);
      fgAddNodeMSILOffset(&GraphNode, NextOffset);

      // Make the switch node.
      BranchNode = fgMakeSwitch((IRNode *)GraphNode, BlockNode);

      // Create the block to hold the switch node.
      fgNodeSetEndMSILOffset(Block, NextOffset);
      Block = fgSplitBlock(Block, NextOffset, nullptr);

      // Set up labels for each case.
      for (uint32_t I = 0; (uint32_t)I < NumCases; I++) {
        BranchOffset = readSwitchCase(&Operand);
        TargetOffset = NextOffset + BranchOffset;
        CHECKTARGET(TargetOffset, ILInputSize);

        GraphNode = nullptr;
        fgAddNodeMSILOffset(&GraphNode, TargetOffset);
        ASSERTNR(GraphNode != nullptr);
        fgAddCaseToCaseListHelper(BranchNode, (IRNode *)GraphNode, I);

        // record a branch
        verifyRecordBranchForVerification(BranchNode, CurrentOffset,
                                          TargetOffset, false);
      }
      break;

    case ReaderBaseNS::CEE_THROW:
    case ReaderBaseNS::CEE_RETHROW:
      // throw/rethrow splits a block
      BlockNode = fgNodeGetStartIRNode(Block);
      fgMakeThrow(BlockNode);

      fgNodeSetEndMSILOffset(Block, NextOffset);
      Block = fgSplitBlock(Block, NextOffset, nullptr);
      break;

    case ReaderBaseNS::CEE_ENDFILTER:
      // Make a branch to the handler, which will correspond to the filter
      // returning true.
      GraphNode = nullptr;
      fgAddNodeMSILOffset(&GraphNode, NextOffset);
      BlockNode = fgNodeGetStartIRNode(Block);
      BranchNode = fgMakeBranchHelper((IRNode *)GraphNode, BlockNode,
                                      CurrentOffset, false, false);
      // Split the block.
      fgNodeSetEndMSILOffset(Block, NextOffset);
      Block = fgSplitBlock(Block, NextOffset, nullptr);
      break;

    case ReaderBaseNS::CEE_ENDFINALLY: {
      // Treat EndFinally as a a goto to the end of the finally.
      //
      // if this endfinally is not in a finally don't do anything
      // verification will catch it later and insert throw
      //
      // note endfinally is same instruction as endfault
      EHRegion *HandlerRegion = getInnermostFaultOrFinallyRegion(CurrentOffset);

      if (HandlerRegion == nullptr ||
          (rgnGetRegionType(HandlerRegion) != ReaderBaseNS::RGN_Finally &&
           rgnGetRegionType(HandlerRegion) != ReaderBaseNS::RGN_Fault)) {
        BADCODE(MVER_E_ENDFINALLY);
      }

      // Make/insert end finally
      BlockNode = fgNodeGetStartIRNode(Block);
      if (rgnGetRegionType(HandlerRegion) == ReaderBaseNS::RGN_Finally) {
        BranchNode = fgMakeEndFinally(BlockNode, HandlerRegion, CurrentOffset);
      } else {
        BranchNode = fgMakeEndFault(BlockNode, HandlerRegion, CurrentOffset);
      }

      // And split the block
      fgNodeSetEndMSILOffset(Block, NextOffset);
      Block = fgSplitBlock(Block, NextOffset, nullptr);
      break;
    }

    case ReaderBaseNS::CEE_JMP:
      fgNodeSetEndMSILOffset(Block, NextOffset);
      if (NextOffset < ILInputSize) {
        Block = makeFlowGraphNode(NextOffset, Block);
      }
      break;

    case ReaderBaseNS::CEE_RET:
      verifyReturnFlow(CurrentOffset);
      BlockNode = fgNodeGetStartIRNode(Block);
      fgMakeReturn(BlockNode);
      fgNodeSetEndMSILOffset(Block, NextOffset);
      if (NextOffset < ILInputSize) {
        Block = makeFlowGraphNode(NextOffset, Block);
      }
      break;

    case ReaderBaseNS::CEE_CALL:
    case ReaderBaseNS::CEE_CALLVIRT:
    case ReaderBaseNS::CEE_NEWOBJ:
    case ReaderBaseNS::CEE_LDFTN:
    case ReaderBaseNS::CEE_LDVIRTFTN:
      break;

    // Need to already know about any locallocs so client knows
    // whether it is safe to recursively tail call.
    case ReaderBaseNS::CEE_LOCALLOC:
      HasLocAlloc = true;
      break;

    case ReaderBaseNS::CEE_LDLOCA:
    case ReaderBaseNS::CEE_LDLOCA_S:
    case ReaderBaseNS::CEE_LDARGA:
    case ReaderBaseNS::CEE_LDARGA_S:
    case ReaderBaseNS::CEE_ARGLIST:
      HasAddressTaken = true;
      break;

    case ReaderBaseNS::CEE_CONSTRAINED:
      TokenConstrained = readValue<mdToken>(Operand);
      PreviousWasPrefix = true;
      break;
    case ReaderBaseNS::CEE_TAILCALL:
      IsTailCall = true;
      PreviousWasPrefix = true;
      break;
    case ReaderBaseNS::CEE_READONLY:
      IsReadOnly = true;
      PreviousWasPrefix = true;
      break;
    case ReaderBaseNS::CEE_VOLATILE:
    case ReaderBaseNS::CEE_UNALIGNED:
      PreviousWasPrefix = true;
      break;

    default:
      // ignore others
      break;
    }

    if (!PreviousWasPrefix) {
      IsTailCall = false;
      IsReadOnly = false;
      TokenConstrained = mdTokenNil;
    }

    // Move the byteOffset to the next instruction
    CurrentOffset = NextOffset;

    if (VerificationNeeded) {

      // compute and store the stack contributions
      // this is required for global verification

      getMSILInstrStackDelta(Opcode, Operand, &GvStackPop[StackOffset],
                             &GvStackPush[StackOffset]);
      StackOffset++;
      while (StackOffset < CurrentOffset) {
        GvStackPop[StackOffset] = 0;
        GvStackPush[StackOffset] = 0;
        StackOffset++;
      }
    }
  }

  // make sure control didn't flow off the end.
  if (VerificationNeeded) {
    switch (Opcode) {
    case ReaderBaseNS::CEE_BR_S:
    case ReaderBaseNS::CEE_LEAVE_S:
    case ReaderBaseNS::CEE_ENDFILTER:
    case ReaderBaseNS::CEE_RET:
    case ReaderBaseNS::CEE_JMP:
    case ReaderBaseNS::CEE_THROW:
    case ReaderBaseNS::CEE_RETHROW:
    case ReaderBaseNS::CEE_BR:
    case ReaderBaseNS::CEE_LEAVE:
    case ReaderBaseNS::CEE_ENDFINALLY:
      break;
    default:
      // control cannot flow off the end of the function
      BADCODE(MVER_E_FALLTHRU);
    }
  }

  // Set the last blocks ending offset to where ever we stopped reading
  fgNodeSetEndMSILOffset(Block, CurrentOffset);
}

// When performing global verification, once the basic blocks have
// been determined we can examine the blocks to determine some values
// that will be used later on for global verification.  The stack
// contributions of each MSIL instruction have already been pre-
// computed in the first pass over the MSIL, but block boundaries
// had not yet been determined at that point.  Take the following
// example block:
//
// MSIL                               pops     pushes
// --------------------------------------------------------------
// add                                 2         1
// ldc                                 0         1
// ldc                                 0         1
// ldc                                 0         1
// call int foo(int,int)               2         1
//
// This routine scans through the contributions of the instructions
// in the block, first accounting for the pops and then for the pushes,
// tracking the net change in the number of items on the stack.  The
// minimum and maximum stack usage are tracked during this process.
//
// The sequence of stack contributions that would be examined for the
// above set of instructions is: -2, +1, 0, +1, 0, +1, 0, +1, -2, +1
//
// In computing the running sum of that sequence, the maximum value
// of the sum at any point will be +2.  The minimum value of the sum
// is -2.  The final value of the sum is +1.
//
// "MaxStack" will reflect the maximum additional entries used on
// the stack beyond any entries in use upon entry to the block.
// This number is useful for validating that the actual stack usage
// meets the declared ".maxstack" value.  In this case it is +2.
//
// "minStack", if not 0, will be negative and indicates that the block
// requires values on the stack upon entry to the block.  This number
// is useful for computing underflow as well as another number discussed
// below.  In this case it is -2.
//
// "netStack" will reflect the net stack contribution of the block.
// This is useful for globally detecting underflow/overflow conditions
// as well as computing another number discussed below.  In this case
// it is +1.
//
// "nTOSTemps" is the number of stack-carried temporaries defined in
// the block that are live-out from the block.  minStack reflects
// the number of stack-carried temporaries that were live on entry.
// If there are live-out stack temporaries occupying those same stack
// slots, they must have been defined within the block.  netStack
// reflects the increase in the number of stack-carried values on exit
// from the block, so these two numbers together yield the number of
// "Top-Of-Stack" temporaries that are defined locally by this block.
// In the example above, nTOSTemps would be 3 = (1) - (-2).
//
// Note that the same computation works when netStack is negative, as
// minStack will always be <= netStack.  If minStack is less than
// netStack, the difference still reflects the number of items on the
// stack that are live-out and defined within the block.
//
void ReaderBase::fgAttachGlobalVerifyData(FlowGraphNode *HeadBlock) {

  // set 'this' uninit if we need to track in a ctor
  // otherwise state is unknown
  InitState ThisInit;

  if (!verNeedsCtorTrack()) {
    ThisInit = ThisInited;
  } else {
    ThisInit = ThisUnreached;
  }

  for (FlowGraphNode *Block = HeadBlock; Block; Block = fgNodeGetNext(Block)) {
    int Min = 0;
    int Max = 0;
    int Current;
    int Start, End, I, NumTOSTemps;
    GlobalVerifyData *GvData;

    // compute high/low watermarks for reader stack
    // as well as net stack contribution of the block
    //
    // Note that EH funclet has the exception object
    // already placed on the stack.
    Current = isRegionStartBlock(Block) ? 1 : 0;
    Min = Current;
    Max = Current;
    Start = fgNodeGetStartMSILOffset(Block);
    End = fgNodeGetEndMSILOffset(Block);
    for (I = Start; I < End; I++) {
      Current -= GvStackPop[I];
      if (Current < Min)
        Min = Current;
      Current += GvStackPush[I];
      if (Current > Max)
        Max = Current;
    }

    // compute the number of stack temporaries defined
    // by this block and left on stack for subsequent
    // blocks to consume
    NumTOSTemps = Current - Min;

    // TODO: Debug dump for this data

    // save the data into the flowgraph
    GvData = (GlobalVerifyData *)getTempMemory(sizeof(GlobalVerifyData));

    GvData->MinStack = Min;
    GvData->MaxStack = Max;
    GvData->NetStack = Current;
    GvData->TOSTempsCount = NumTOSTemps;
    GvData->StkDepth = -1;
    GvData->TiStack = nullptr;
    GvData->IsOnWorklist = false;
    GvData->Block = Block;
    GvData->ThisInitialized = ThisInit;

    fgNodeSetGlobalVerifyData(Block, GvData);
  }
}

void ReaderBase::verifyRecordBranchForVerification(IRNode *Branch,
                                                   uint32_t SrcOffset,
                                                   uint32_t TargetOffset,
                                                   bool IsLeave) {
  // add this to the list (BranchesToVerify)
  if (VerificationNeeded) {
    VerificationBranchInfo *BranchInfo =
        (VerificationBranchInfo *)getTempMemory(sizeof(VerificationBranchInfo));

    BranchInfo->SrcOffset = SrcOffset;
    BranchInfo->TargetOffset = TargetOffset;
    BranchInfo->BranchOp = Branch;
    BranchInfo->IsLeave = IsLeave;

    BranchInfo->Next = BranchesToVerify;
    BranchesToVerify = BranchInfo;
  }
}

/// \brief This code reads through the bytes codes and builds
///        the correct basic blocks.
///
/// It operates in four phases
/// - PHASE 1: Read byte codes and create some basic blocks
///            based on branches and switches. Create the branch
///            IR for those byte codes. Populate the NodeOffsetList
///            with the information to adjust branch targets.
/// - PHASE 2: Complete the flow graph by correctly adjusting branch
///            targets and spliting basic blocks based on those
///            targets.
/// - PHASE 3: Verify branch targets.
/// - PHASE 4: Insert EH IR. This includes region start/end markers.
///
/// \param      Buffer     The buffer containing the byte codes in MSIL.
/// \param      BufferSize The length of the buffer in bytes.
///
/// \returns FlowGraphNode corresponding to the bytes.
FlowGraphNode *ReaderBase::fgBuildBasicBlocksFromBytes(uint8_t *Buffer,
                                                       uint32_t BufferSize) {
  FlowGraphNode *Block;

  // Initialize head block to root region.
  CurrentRegion = EhRegionTree;
  Block = fgGetHeadBlock();
  fgNodeSetRegion(Block, CurrentRegion);

  // PRE-PHASE
  Block = fgPrePhase(Block);

  // PHASE 1
  // parse bytecode and construct flow graph
  // gather label information
  fgBuildPhase1(Block, Buffer, BufferSize);

  // PHASE 2:
  // replace temporary branch targets that were gathered into
  // NodeOffsetList during phase 1 with real ones
  fgReplaceBranchTargets();

  insertIBCAnnotations();

  // PHASE 3:
  // verify branch targets BEFORE inserting EH Annotations
  // EH annotations phase is unsafe
  if (VerificationNeeded) {
    VerificationBranchInfo *BranchInfo = BranchesToVerify;
    while (BranchInfo) {
      VerInstrStartOffset = BranchInfo->SrcOffset;
      verifyBranchTarget(nullptr, irNodeGetEnclosingBlock(BranchInfo->BranchOp),
                         fgGetRegionFromMSILOffset(BranchInfo->SrcOffset),
                         BranchInfo->TargetOffset, BranchInfo->IsLeave);
      BranchInfo = BranchInfo->Next;
    }
  }

  // POST-PHASE - Compiler dependent flow graph cleanup
  fgPostPhase();

  if (VerificationNeeded) {
    // Add annotations to basic blocks to support global verification.
    // This involves analyzing the stack contributions of the various
    // blocks.
    // Has to occur after phase 3 since phase 3 can split blocks.
    Block = fgGetHeadBlock();
    fgAttachGlobalVerifyData(Block);
  }

  return fgGetHeadBlock();
}

// Given information about how to do a runtime lookup, generate the
// tree for the runtime lookup.
//
// Run-time lookup is required if the enclosing method is shared
// between instantiations and the token refers to formal type
// parameters whose instantiation is not known at compile-time.
//
// Class Handles and Method Handles must "be restored" in case where
// the handle is *not* being passed to a helper call that will do the
// restore for us. Field handles never need to be restored.
IRNode *ReaderBase::genericTokenToNode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                       bool EmbedParent, bool MustRestoreHandle,
                                       CORINFO_GENERIC_HANDLE *StaticHandle,
                                       bool *RuntimeLookup, bool NeedResult) {
  CORINFO_GENERICHANDLE_RESULT Result;

  embedGenericHandle(ResolvedToken, EmbedParent, &Result);
  if (Result.lookup.lookupKind.needsRuntimeLookup &&
      getCurrentMethodHandle() != MethodBeingCompiled) {
    ASSERTDBG("We inlined a runtime generic dictionary lookup!");
    ReaderBase::fatal(CORJIT_INTERNALERROR);
  }

  if (RuntimeLookup != nullptr)
    *RuntimeLookup = Result.lookup.lookupKind.needsRuntimeLookup;

  if (StaticHandle != nullptr)
    *StaticHandle = Result.compileTimeHandle;

  if (MustRestoreHandle && !Result.lookup.lookupKind.needsRuntimeLookup) {
    switch (Result.handleType) {
    case CORINFO_HANDLETYPE_CLASS:
      classMustBeLoadedBeforeCodeIsRun(
          (CORINFO_CLASS_HANDLE)Result.compileTimeHandle);
      break;

    case CORINFO_HANDLETYPE_METHOD:
      methodMustBeLoadedBeforeCodeIsRun(
          (CORINFO_METHOD_HANDLE)Result.compileTimeHandle);
      break;

    case CORINFO_HANDLETYPE_FIELD:
      classMustBeLoadedBeforeCodeIsRun(
          getFieldClass((CORINFO_FIELD_HANDLE)Result.compileTimeHandle));
      break;

    default:
      break;
    }
  }

  // No runtime lookup is required
  if (!Result.lookup.lookupKind.needsRuntimeLookup) {
    if (NeedResult) {
      mdToken Token = ResolvedToken->token;
      // If the handle really corresponds to a class and not a method, pass
      // mdtClassHandle to handleToIRNode so that a class name is used for the
      // name of the GlobalVariable created there instead of the method name.
      if ((Result.handleType == CORINFO_HANDLETYPE_CLASS) &&
          (TypeFromToken(Token) != mdtClassHandle)) {
        Token = mdtClassHandle;
      }
      if (Result.lookup.constLookup.accessType == IAT_VALUE) {
        ASSERTNR(Result.lookup.constLookup.handle != nullptr);
        return handleToIRNode(Token, Result.lookup.constLookup.handle,
                              Result.compileTimeHandle, false, false, true,
                              false);
      } else {
        ASSERTNR(Result.lookup.constLookup.accessType == IAT_PVALUE);
        // TODO: Can we mark this as readonly for aliasing?
        return handleToIRNode(Token, Result.lookup.constLookup.addr,
                              Result.compileTimeHandle, true, true, true,
                              false);
      }
    } else
      return nullptr;
  } else {
    // TODO: runtime lookup may be incompatible with inlining.
    // LLILC doesn't do any inlining yet...

    return runtimeLookupToNode(Result.lookup.lookupKind.runtimeLookupKind,
                               &Result.lookup.runtimeLookup);
  }
}

// Generics: Code sharing
//
// Run-time lookup is required if the enclosing method is shared
// between instantiations and the token refers to formal type
// parameters whose instantiation is not known at compile-time
IRNode *ReaderBase::runtimeLookupToNode(CORINFO_RUNTIME_LOOKUP_KIND Kind,
                                        CORINFO_RUNTIME_LOOKUP *Lookup) {
  // Collectible types needs the generics context when gc-ing. Keeping
  // it alive for the entire method if the generics context is
  // referenced is a conservative approach which should not have
  // significant performance issues
  methodNeedsToKeepAliveGenericsContext(true);

  // It's available only via the run-time helper function
  if (Lookup->indirections == CORINFO_USEHELPER) {
    IRNode *Arg1, *Arg2;

    Arg2 = handleToIRNode(mdtSignature, Lookup->signature, Lookup->signature,
                          false, true, true, false);

    // It requires the exact method desc argument
    if (Kind == CORINFO_LOOKUP_METHODPARAM) {
      // inst-param
      Arg1 = instParam();
    } else {
      // It requires the vtable pointer
      if (Kind == CORINFO_LOOKUP_CLASSPARAM) {
        // use inst-param
        Arg1 = instParam();
      } else {
        // use this ptr
        ASSERTNR(Kind == CORINFO_LOOKUP_THISOBJ);
        Arg1 = derefAddress(thisObj(), false, false);
      }
    }

    return callRuntimeHandleHelper(Lookup->helper, Arg1, Arg2, nullptr);
  }

  IRNode *VTableNode, *SlotPointerNode;

  // Use the method descriptor that was passed in to get at
  // instantiation info
  if (Kind == CORINFO_LOOKUP_METHODPARAM) {
    VTableNode = instParam();
  } else {
    if (Kind == CORINFO_LOOKUP_CLASSPARAM) {
      // Use the vtable pointer that was passed in
      VTableNode = instParam();
    } else {
      // Use the vtable of "this" to get at instantiation info
      ASSERTNR(Kind == CORINFO_LOOKUP_THISOBJ);
      VTableNode = derefAddress(thisObj(), false, false);
    }
  }

  // VTableNode now points into the runtime data structures.
  // Assume they are non-null when dereferencing into them below.

  if (!Lookup->testForNull) {
    // We only use VTableNode as the seed for SlotPointerNode
    SlotPointerNode = VTableNode;
  } else {
    // SlotPointerNode is used for the indirections
    // but VTableNode is passed to the call
    dup(VTableNode, &SlotPointerNode, &VTableNode);
  }

  // Use the vtable of "this" to get at instantiation info

  // Apply repeated indirections
  for (WORD I = 0; I < Lookup->indirections; I++) {
    if (I != 0) {
      SlotPointerNode = derefAddressNonNull(SlotPointerNode, false, true);
    }
    if (Lookup->offsets[I] != 0) {
      SlotPointerNode = binaryOp(ReaderBaseNS::Add, SlotPointerNode,
                                 loadConstantI(Lookup->offsets[I]));
    }
  }

  // No null test required
  if (!Lookup->testForNull) {
    if (Lookup->indirections == 0)
      return SlotPointerNode;

    SlotPointerNode = derefAddressNonNull(SlotPointerNode, false, true);
    if (!Lookup->testForFixup)
      return SlotPointerNode;

    return conditionalDerefAddress(SlotPointerNode);
  }

  ASSERTNR(Lookup->indirections != 0);

  // Extract the type handle
  IRNode *HandleNode = derefAddressNonNull(SlotPointerNode, false, true);

  IRNode *SignatureNode =
      handleToIRNode(mdtSignature, Lookup->signature, Lookup->signature, false,
                     true, true, false);

  // Call helper on null
  return callRuntimeHandleHelper(Lookup->helper, VTableNode, SignatureNode,
                                 HandleNode);
}

CorInfoHelpFunc ReaderBase::getNewArrHelper(CORINFO_CLASS_HANDLE ElementType) {
  return JitInfo->getNewArrHelper(ElementType);
}

// InitBlk - Creates a memcopy helper call/intrinsic.
void ReaderBase::cpBlk(IRNode *Count,    // byte count
                       IRNode *SrcAddr,  // source address
                       IRNode *DestAddr, // dest address
                       ReaderAlignType Alignment, bool IsVolatile) {
  const bool MayThrow = true;
  callHelper(CORINFO_HELP_MEMCPY, MayThrow, nullptr, DestAddr, SrcAddr, Count,
             nullptr, Alignment, IsVolatile);
}

// InitBlk - Creates a memset helper call/intrinsic.
void ReaderBase::initBlk(IRNode *Count,    // byte count
                         IRNode *Value,    // Value
                         IRNode *DestAddr, // dest address
                         ReaderAlignType Alignment, bool IsVolatile) {
  const bool MayThrow = true;
  callHelper(CORINFO_HELP_MEMSET, MayThrow, nullptr, DestAddr, Value, Count,
             nullptr, Alignment, IsVolatile);
}

void ReaderBase::initObj(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                         IRNode *ObjectAddr) {
  // GENERICS NOTE:
  //
  // This routine can take in a reference, in which case we need to
  // zero-init it as well.
  uint32_t Size = getClassSize(ResolvedToken->hClass);
  IRNode *SizeNode = loadConstantI4(Size);
  IRNode *ValueNode = loadConstantI4(0);

  // Use init blk to initialize the object
  initBlk(SizeNode, ValueNode, ObjectAddr, Reader_AlignUnknown, false);
}

// Box - Default reader processing for CEE_BOX.
IRNode *ReaderBase::box(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg2,
                        uint32_t *NextOffset, VerificationState *VState) {
  IRNode *Dst, *RetVal, *Arg1;
  CORINFO_CLASS_HANDLE Class = ResolvedToken->hClass;

  if ((getClassAttribs(Class) & CORINFO_FLG_VALUECLASS) == 0) {
    // We allow box of reference types.  box of reference type -> NOP
    return Arg2;
  }

  // Ensure that operand from operand stack has size that is
  // compatible with box destination, then get the (possibly
  // converted) operand's address.
  Arg2 = convertToBoxHelperArgumentType(Arg2, getClassSize(Class));

  Dst = makeBoxDstOperand(Class);

  // Use address of value here, this helps to prevent user-syms from
  // being aliased.
  Arg2 = addressOfValue(Arg2);

  // The first arg for the helper is the class handle. Derive it
  // from the token.
  Arg1 = genericTokenToNode(ResolvedToken, true);

  const bool MayThrow = true;
  RetVal = callHelper(getBoxHelper(Class), MayThrow, Dst, Arg1, Arg2);

  return RetVal;
}

// CastClass - Generate a simple helper call for the cast class.
IRNode *ReaderBase::castClass(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                              IRNode *ObjRefNode) {
  const bool IsThrowing = true;
  CorInfoHelpFunc HelperId =
      (Flags & CORJIT_FLG_READYTORUN)
          ? CORINFO_HELP_READYTORUN_CHKCAST
          : JitInfo->getCastingHelper(ResolvedToken, IsThrowing);

  return castOp(ResolvedToken, ObjRefNode, HelperId);
}

// IsInst - Default reader processing of CEE_ISINST.
IRNode *ReaderBase::isInst(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                           IRNode *ObjRefNode) {
  const bool IsThrowing = false;
  CorInfoHelpFunc HelperId =
      (Flags & CORJIT_FLG_READYTORUN)
          ? CORINFO_HELP_READYTORUN_ISINSTANCEOF
          : JitInfo->getCastingHelper(ResolvedToken, IsThrowing);

  return castOp(ResolvedToken, ObjRefNode, HelperId);
}

IRNode *ReaderBase::refAnyVal(IRNode *RefAny,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  IRNode *Dst, *Arg1;

  // first argument is class handle
  Arg1 = genericTokenToNode(ResolvedToken);

  // second argument is the refany (passed by reference)
  IRNode *Arg2 = RefAny;

  // Create dest operand
  Dst = makeRefAnyDstOperand(ResolvedToken->hClass);

  // Make the helper call
  const bool MayThrow = true;
  return callHelper(CORINFO_HELP_GETREFANY, MayThrow, Dst, Arg1, Arg2);
}

void ReaderBase::storeElemRefAny(IRNode *Value, IRNode *Index, IRNode *Obj) {
  // Make the helper call
  const bool MayThrow = true;
  callHelper(CORINFO_HELP_ARRADDR_ST, MayThrow, nullptr, Obj, Index, Value);
}

// StoreIndir - Creates an instruction to assign the value on
// top of the stack into the memory pointed at by the operand which is
// 2nd from the top of the stack.
void ReaderBase::storeIndir(ReaderBaseNS::StIndirOpcode Opcode, IRNode *Value,
                            IRNode *Address, ReaderAlignType Alignment,
                            bool IsVolatile, bool AddressMayBeNull) {
  static const CorInfoType Map[ReaderBaseNS::LastStindOpcode] = {
      CORINFO_TYPE_BYTE,      // STIND_I1
      CORINFO_TYPE_SHORT,     // STIND_I2
      CORINFO_TYPE_INT,       // STIND_I4
      CORINFO_TYPE_LONG,      // STIND_I8
      CORINFO_TYPE_NATIVEINT, // STIND_I
      CORINFO_TYPE_FLOAT,     // STIND_R4
      CORINFO_TYPE_DOUBLE,    // STIND_R8
      CORINFO_TYPE_REFANY     // STIND_REF
  };

  ASSERTNR(Opcode >= ReaderBaseNS::StindI1 &&
           Opcode < ReaderBaseNS::LastStindOpcode);
  CorInfoType TheCorInfoType = Map[Opcode];

  if (TheCorInfoType == CORINFO_TYPE_REFANY) {
    // STIND_REF requires that our type simply be a managed pointer.
    // Pass in null for CLASS_HANDLE and TOKEN.
    // And they are always naturally aligned, so ignore any other Alignment
    rdrCallWriteBarrierHelper(Address, Value, Reader_AlignNatural, IsVolatile,
                              nullptr, true, false, false, false);
  } else {
    storePrimitiveType(Value, Address, TheCorInfoType, Alignment, IsVolatile,
                       AddressMayBeNull);
  }
}

// LoadIndir -
IRNode *ReaderBase::loadIndir(ReaderBaseNS::LdIndirOpcode Opcode,
                              IRNode *Address, ReaderAlignType Alignment,
                              bool IsVolatile, bool IsInterfReadOnly,
                              bool AddressMayBeNull) {
  static const CorInfoType Map[ReaderBaseNS::LastLdindOpcode] = {
      CORINFO_TYPE_BYTE,      // LDIND_I1
      CORINFO_TYPE_UBYTE,     // LDIND_U1
      CORINFO_TYPE_SHORT,     // LDIND_I2
      CORINFO_TYPE_USHORT,    // LDIND_U2
      CORINFO_TYPE_INT,       // LDIND_I4
      CORINFO_TYPE_UINT,      // LDIND_U4
      CORINFO_TYPE_LONG,      // LDIND_I8
      CORINFO_TYPE_NATIVEINT, // LDIND_I
      CORINFO_TYPE_FLOAT,     // LDIND_R4
      CORINFO_TYPE_DOUBLE,    // LDIND_R8
      CORINFO_TYPE_REFANY     // LDIND_REF
  };

  ASSERTNR(Opcode >= ReaderBaseNS::LdindI1 &&
           Opcode < ReaderBaseNS::LastLdindOpcode);
  CorInfoType TheCorInfoType = Map[Opcode];

  return loadPrimitiveType(Address, TheCorInfoType, Alignment, IsVolatile,
                           IsInterfReadOnly, AddressMayBeNull);
}

// StoreObj - default reader processing of CEE_STOBJ
void ReaderBase::storeObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Value,
                          IRNode *Address, ReaderAlignType Alignment,
                          bool IsVolatile, bool IsField,
                          bool AddressMayBeNull) {
  CORINFO_CLASS_HANDLE Class;
  CorInfoType TheCorInfoType;

  if (IsField) {
    TheCorInfoType = getFieldType(ResolvedToken->hField, &Class);
    ASSERTNR(TheCorInfoType == CORINFO_TYPE_VALUECLASS);
  } else {
    Class = ResolvedToken->hClass;
    TheCorInfoType = getClassType(Class);
  }

  if (!(getClassAttribs(Class) & CORINFO_FLG_VALUECLASS)) {
    storeIndir(ReaderBaseNS::StindRef, Value, Address, Alignment, IsVolatile,
               AddressMayBeNull);
  } else if (isPrimitiveType(Class)) {
    storePrimitiveType(Value, Address, TheCorInfoType, Alignment, IsVolatile,
                       AddressMayBeNull);
  } else {
    storeNonPrimitiveType(Value, Address, Class, Alignment, IsVolatile,
                          ResolvedToken, IsField);
  }
}

// LoadObj - default reader processing of CEE_LDOBJ
IRNode *ReaderBase::loadObj(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                            IRNode *Address, ReaderAlignType Alignment,
                            bool IsVolatile, bool IsField,
                            bool AddressMayBeNull) {
  CORINFO_CLASS_HANDLE Class;
  CorInfoType TheCorInfoType;

  if (IsField) {
    TheCorInfoType = getFieldType(ResolvedToken->hField, &Class);
    ASSERTNR(TheCorInfoType == CORINFO_TYPE_VALUECLASS);
  } else {
    Class = ResolvedToken->hClass;
    TheCorInfoType = getClassType(Class);
  }
  if (!(getClassAttribs(Class) & CORINFO_FLG_VALUECLASS)) {
    return loadIndir(ReaderBaseNS::LdindRef, Address, Alignment, IsVolatile,
                     false, AddressMayBeNull);
  } else if (isPrimitiveType(TheCorInfoType)) {
    return loadPrimitiveType(Address, TheCorInfoType, Alignment, IsVolatile,
                             false, AddressMayBeNull);
  } else {
    return loadNonPrimitiveObj(Address, Class, Alignment, IsVolatile,
                               AddressMayBeNull);
  }
}

// CpObj - default reader processing of CEE_CPOBJ
void ReaderBase::cpObj(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Src,
                       IRNode *Dst, ReaderAlignType Alignment,
                       bool IsVolatile) {
  IRNode *Value = loadObj(ResolvedToken, Src, Alignment, IsVolatile, false);
  storeObj(ResolvedToken, Value, Dst, Alignment, IsVolatile, false);
}

// UnboxAny - Default reader processing for CEE_UNBOXANY.
IRNode *ReaderBase::unboxAny(CORINFO_RESOLVED_TOKEN *ResolvedToken, IRNode *Arg,
                             ReaderAlignType AlignmentPrefix,
                             bool IsVolatilePrefix) {
  // GENERICS NOTE: New Instruction
  //   Value Types: unbox followed by ldobj
  //   Reference Types: castclass
  if (getClassAttribs(ResolvedToken->hClass) & CORINFO_FLG_VALUECLASS) {
    return unbox(ResolvedToken, Arg, true, AlignmentPrefix, IsVolatilePrefix);
  } else {
    return castClass(ResolvedToken, Arg);
  }
}

// Break - Default reader processing for CEE_BREAK.
void ReaderBase::breakOpcode() {
  // Make the helper call
  const bool MayThrow = true;
  callHelper(CORINFO_HELP_USER_BREAKPOINT, MayThrow, nullptr);
}

// InsertClassConstructor - Insert a call to the class constructor helper.
void ReaderBase::insertClassConstructor() {
  CORINFO_CLASS_HANDLE Class = getCurrentMethodClass();
  CORINFO_METHOD_HANDLE Method = getCurrentMethodHandle();
  mdToken MethodToken = mdtMethodDef;
  IRNode *MethodNode, *ClassNode;
  bool IsIndirect;

  // Generics: Code sharing
  // Special care must be taken when inserting constructors for shared classes
  CORINFO_LOOKUP_KIND Kind = JitInfo->getLocationOfThisType(Method);

  if (Kind.needsRuntimeLookup) {
    // Collectible types needs the generics context when
    // gc-ing. Keeping it alive for the entire method if the generics
    // context is referenced is a conservative approach which should
    // not have significant performance issues
    methodNeedsToKeepAliveGenericsContext(true);

    switch (Kind.runtimeLookupKind) {
    case CORINFO_LOOKUP_THISOBJ: {
      // call CORINFO_HELP_INITINSTCLASS(thisobj, embedMethodHandle(M))
      Method = embedMethodHandle(Method, &IsIndirect);
      // TODO: Aliasing -- always readonly?
      MethodNode = handleToIRNode(MethodToken, Method, 0, IsIndirect,
                                  IsIndirect, true, false);
      ClassNode = derefAddress(thisObj(), false, false);
      const bool MayThrow = true;
      callHelper(CORINFO_HELP_INITINSTCLASS, MayThrow, nullptr, ClassNode,
                 MethodNode);
      return;
    }
    case CORINFO_LOOKUP_CLASSPARAM: {
      // will only be returned when you are compiling code that takes
      // a hidden parameter P.  You should emit a call
      // CORINFO_HELP_INITCLASS(P)
      ClassNode = instParam();
      const bool MayThrow = true;
      callHelper(CORINFO_HELP_INITCLASS, MayThrow, nullptr, ClassNode);
      return;
    }
    case CORINFO_LOOKUP_METHODPARAM: {
      // will only be returned when you are compiling code that takes
      // a hidden parameter P.  You should emit a call
      // CORINFO_HELP_INITINSTCLASS(nullptr, P)
      MethodNode = instParam();
      ClassNode = loadConstantI8(0);
      const bool MayThrow = true;
      callHelper(CORINFO_HELP_INITINSTCLASS, MayThrow, nullptr, ClassNode,
                 MethodNode);
      return;
    }
    default:
      ASSERTNR(!"NYI");
    }
  } else {
    if (AreInlining && domInfoDominatorHasClassInit(CurrentFgNode, Class))
      return;

    // Record that this block initialized the class.
    domInfoRecordClassInit(CurrentFgNode, Class);

    if (Flags & CORJIT_FLG_READYTORUN) {
      CORINFO_RESOLVED_TOKEN ResolvedToken;
      memset(&ResolvedToken, 0, sizeof(ResolvedToken));
      ResolvedToken.hClass = Class;
      const bool MayThrow = false;
      IRNode *Dst = nullptr;
      callReadyToRunHelper(CORINFO_HELP_READYTORUN_STATIC_BASE, MayThrow, Dst,
                           &ResolvedToken);
      return;
    }

    // Use the shared static base helper as it is faster than InitClass
    CorInfoHelpFunc HelperId = getSharedCCtorHelper(Class);

    if (HelperId == CORINFO_HELP_GETGENERICS_NONGCSTATIC_BASE) {
      void *ClassHandle = embedClassHandle(Class, &IsIndirect);

      ClassNode = handleToIRNode(MethodToken, ClassHandle, Class, IsIndirect,
                                 IsIndirect, true, false);

      const bool MayThrow = false;
      callHelper(HelperId, MayThrow, nullptr, ClassNode);
    } else {
      rdrCallGetStaticBase(Class, MethodToken, HelperId, false, false, nullptr);
    }
  }
}

// rdrGetCritSect
//
// In order to get the correct lock for static generic methods we need
// to call a helper. This method deals with this runtime quirk.  The
// method also safely handles non-static methods and non-generic
// static methods. The return value of the method is an IRNode that
// can be passed to any of the locking methods.
IRNode *ReaderBase::rdrGetCritSect() {
  // For non-static methods, simply use the "This" pointer
  if ((getCurrentMethodAttribs() & CORINFO_FLG_STATIC) == 0) {
    return thisObj();
  }

  IRNode *HandleNode = nullptr;

  CORINFO_LOOKUP_KIND Kind =
      JitInfo->getLocationOfThisType(getCurrentMethodHandle());

  if (!Kind.needsRuntimeLookup) {
    bool IsIndirect = false;
    void *CritSect = getMethodSync(&IsIndirect);
    HandleNode = handleToIRNode(mdtSyncHandle, CritSect, nullptr, IsIndirect,
                                IsIndirect, IsIndirect, false);
  } else {
    // Collectible types needs the generics context when
    // gc-ing. Keeping it alive for the entire method if the generics
    // context is referenced is a conservative approach which should
    // not have significant performance issues
    methodNeedsToKeepAliveGenericsContext(true);

    switch (Kind.runtimeLookupKind) {
    case CORINFO_LOOKUP_THISOBJ:
      ASSERTNR(!"Should never get this for static method.");
      break;
    case CORINFO_LOOKUP_CLASSPARAM:
      // In this case, the hidden param is the class handle.
      HandleNode = instParam();
      break;
    case CORINFO_LOOKUP_METHODPARAM: {
      // In this case, the hidden param is the method handle.
      HandleNode = instParam();
      // Call helper CORINFO_HELP_GETCLASSFROMMETHODPARAM to get the
      // class handle from the method handle.
      const bool MayThrow = false;
      HandleNode = callHelper(CORINFO_HELP_GETCLASSFROMMETHODPARAM, MayThrow,
                              makePtrNode(), HandleNode);
      break;
    }
    default:
      ASSERTNR(!"Unknown LOOKUP_KIND");
      break;
    }

    ASSERTNR(HandleNode); // HandleNode should now contain the
                          // CORINFO_CLASS_HANDLE for the exact class.

    // Given the class handle, get the pointer to the Monitor.
    const bool MayThrow = false;
    HandleNode = callHelper(CORINFO_HELP_GETSYNCFROMCLASSHANDLE, MayThrow,
                            makePtrNode(), HandleNode);
  }

  ASSERTNR(HandleNode);
  return HandleNode;
}

// If accessing the field requires a helper call, then call the
// appropriate helper. This can apply to loading from or storing
// to the field.
//
// For loads, the prototype is 'type ldfld(object, fieldHandle)'.
// For stores, the prototype is 'void stfld(object, fieldHandle, value)'.
IRNode *ReaderBase::rdrCallFieldHelper(
    CORINFO_RESOLVED_TOKEN *ResolvedToken, CorInfoHelpFunc HelperId,
    bool IsLoad,
    IRNode *Dst, // Dst node if this is a load, otherwise nullptr
    IRNode *Obj, IRNode *Value, ReaderAlignType Alignment, bool IsVolatile) {
  IRNode *Arg1, *Arg2, *Arg3, *Arg4;

  if (IsLoad) {
    ASSERTNR(Value == nullptr);

    if (HelperId == CORINFO_HELP_GETFIELDSTRUCT) {
      // For a GetFieldStruct, we want to create the following:
      //
      // HCIMPL4(VOID, JIT_GetFieldStruct, LPVOID retBuff, Object *obj,
      // FieldDesc *pFD, MethodTable *pFieldMT)
      //
      // What this means, is that the helper will *not* return the
      // value that we are interested in.  Instead, it will pass a
      // pointer to the return value as the first param.
      Arg1 = addressOfValue(Dst);

      // Arg 2
      Arg2 = Obj;

      // Arg 3 is the field handle.
      Arg3 = genericTokenToNode(ResolvedToken);

      // Arg 4 is the field type handle.
      CORINFO_CLASS_HANDLE Class = nullptr;
      getFieldType(ResolvedToken->hField, &Class);

      bool IsIndirect;
      void *ClassHandle = embedClassHandle(Class, &IsIndirect);

      Arg4 = handleToIRNode(ResolvedToken->token, ClassHandle, Class,
                            IsIndirect, IsIndirect, true, false);

      // Make the helper call
      const bool MayThrow = true;
      callHelper(HelperId, MayThrow, nullptr, Arg1, Arg2, Arg3, Arg4, Alignment,
                 IsVolatile);
      return Dst;
    } else {
      // OTHER LOAD

      // Arg2 - the field handle.
      Arg2 = genericTokenToNode(ResolvedToken);

      // Arg1 - this pointer
      Arg1 = Obj;

      // Make the helper call
      const bool MayThrow = true;
      return callHelper(HelperId, MayThrow, Dst, Arg1, Arg2, nullptr, nullptr,
                        Alignment, IsVolatile);
    }
  } else {
    // STORE

    if (HelperId == CORINFO_HELP_SETFIELDSTRUCT) {
      // For a SetFieldStruct, we want to create the following:
      //
      // HCIMPL4(VOID, JIT_SetFieldStruct, Object *obj, FieldDesc *pFD,
      // MethodTable *pFieldMT, LPVOID valuePtr)
      //
      // The idea here is that we must pass a *pointer* to the value
      // that we are setting, rather than the value itself.  Simple
      // enough... MSILAddressOf is your friend!
      Arg4 = addressOfValue(Value);

      // The third argument is the field type handle.
      CORINFO_CLASS_HANDLE Class = nullptr;
      getFieldType(ResolvedToken->hField, &Class);

      bool IsIndirect;
      void *ClassHandle = embedClassHandle(Class, &IsIndirect);

      Arg3 = handleToIRNode(ResolvedToken->token, ClassHandle, Class,
                            IsIndirect, IsIndirect, true, false);

      // The second argument to the helper is the field handle.
      Arg2 = genericTokenToNode(ResolvedToken);

      // The first argument to the helper is the this pointer.
      Arg1 = Obj;

      // Make the helper call
      const bool MayThrow = true;
      return callHelper(HelperId, MayThrow, nullptr, Arg1, Arg2, Arg3, Arg4,
                        Alignment, IsVolatile);
    } else {
      // assert that the helper id is expected
      ASSERTNR(HelperId == CORINFO_HELP_SETFIELD8 ||
               HelperId == CORINFO_HELP_SETFIELD16 ||
               HelperId == CORINFO_HELP_SETFIELD32 ||
               HelperId == CORINFO_HELP_SETFIELD64 ||
               HelperId == CORINFO_HELP_SETFIELDOBJ ||
               HelperId == CORINFO_HELP_SETFIELDFLOAT ||
               HelperId == CORINFO_HELP_SETFIELDDOUBLE);

      Arg3 = Value;

      // The second argument to the helper is the field handle.
      Arg2 = genericTokenToNode(ResolvedToken);

      // The first argument to the helper is the this pointer.
      Arg1 = Obj;

      // Make the helper call
      const bool MayThrow = true;
      return callHelper(HelperId, MayThrow, nullptr, Arg1, Arg2, Arg3, nullptr,
                        Alignment, IsVolatile);
    }
  }
}

// rdrCallWriteBarrierHelper
//
// This code adds a call to the WriteBarrier helper to the code
// stream.  What this is is a helper function which copies a value
// into a particular field of a class.  We use this helper when the
// runtime tells us to by putting the CORINFO_FLG_WRITE_BARRIER_HELPER
// on the fieldAttribs for the field.  The runtime only does this if
// the field we are writing to is a pointer to the GC heap (or is a
// value class which contains such a pointer).  The helper is used in
// these situations so that the GC runtime can update its tables and
// know that this piece of memory has been updated.
//
// Do note that there are actually 2 slightly different versions of
// the helper that we must call.  One if we are simply writing into a
// field which is a pointer, and a different one if we are writing in
// a value class.
//
// Alignment is necessary in case the JIT wants to turn a struct write
// barrier into a struct copy.
void ReaderBase::rdrCallWriteBarrierHelper(
    IRNode *Dst, IRNode *Src, ReaderAlignType Alignment, bool IsVolatile,
    CORINFO_RESOLVED_TOKEN *ResolvedToken, bool IsNotValueClass,
    bool IsValueIsPointer, bool IsFieldToken, bool IsUnchecked) {
  if (IsNotValueClass) {
    // This is the non-value class case.  That is, we are simply
    // writing to a field in a class which happens to be a GC pointer.
    //
    // HCIMPL2(void, JIT_CheckedWriteBarrier, Object** dest, Object * value)
    const bool MayThrow = true;
    callHelper(
        IsUnchecked ? CORINFO_HELP_ASSIGN_REF : CORINFO_HELP_CHECKED_ASSIGN_REF,
        MayThrow, nullptr, Dst, Src, nullptr, nullptr, Alignment, IsVolatile);
  } else {
    // This is the case in which we will be copying a value class into
    // the field of this struct.  The runtime will need to be passed
    // the classHandle of the struct so it knows which fields are of
    // importance, w.r.t. GC.
    //
    // HCIMPL2(void, JIT_StructWriteBarrier, void* dest, void* src,
    //         CORINFO_CLASS_HANDLE *fieldsClassHandle)

    if (!IsValueIsPointer) {
      // Do note that in this case we want a pointer to the source,
      // but we will actually have is the struct itself, therefore we
      // need to get its address.
      Src = (IRNode *)addressOfValue(Src);
    }

    CORINFO_CLASS_HANDLE Class = nullptr;
    if (IsFieldToken) {
      getFieldType(ResolvedToken->hField, &Class);
    } else {
      Class = ResolvedToken->hClass;
    }

    copyStruct(Class, Dst, Src, Alignment, IsVolatile, IsUnchecked);
  }
}

IRNode *ReaderBase::rdrCallGetStaticBase(CORINFO_CLASS_HANDLE Class,
                                         mdToken Token,
                                         CorInfoHelpFunc HelperId, bool NoCtor,
                                         bool CanMoveUp, IRNode *Dst) {
  bool IsIndirect, IsIndirect2;

  void *EmbedModuleDomainID =
      getEmbedModuleDomainIDForStatics(Class, &IsIndirect);
  void *EmbedClassDomainID = getEmbedClassDomainID(Class, &IsIndirect2);

  // Can't use the moduleHandle directly because we're not embedding a
  // handle to the module, but a handle to the ModuleDomainID.  Since
  // handles are pointers in disguise, take advantage of them always
  // being aligned and create a fake handle to represent the
  // ModuleDomainID. Because the module returned may not be the proper
  // unique value, use the class handle here to provide uniqueness
  ASSERTNR(((size_t)Class & 2) == 0);
  // TODO: Aliasing -- always readonly?
  IRNode *ModuleDomainIDNode =
      handleToIRNode(mdtModuleID, EmbedModuleDomainID,
                     (CORINFO_MODULE_HANDLE)((size_t)Class | 2), IsIndirect,
                     IsIndirect, IsIndirect, false);
  // Can't use the Class directly because we're not embedding a
  // handle to the class, but a handle to the ClassDomainID.  Since
  // handles are pointers in disguise, take advantage of them always
  // being aligned and create a fake handle to represent the
  // ClassDomainID
  ASSERTNR(((size_t)Class & 1) == 0);
  IRNode *ClassDomainIDNode = handleToIRNode(
      Token, EmbedClassDomainID, (CORINFO_CLASS_HANDLE)((size_t)Class | 1),
      IsIndirect2, IsIndirect2, IsIndirect2, false);

  const bool MayThrow = false;
  return callHelper(HelperId, MayThrow, Dst, ModuleDomainIDNode,
                    ClassDomainIDNode, nullptr, nullptr, Reader_AlignUnknown,
                    false, NoCtor, CanMoveUp);
}

IRNode *
ReaderBase::rdrGetStaticFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                     CORINFO_FIELD_INFO *FieldInfo) {
  IRNode *AddressNode;

  ASSERTNR(FieldInfo->fieldFlags & CORINFO_FLG_FIELD_STATIC);

  handleMemberAccess(FieldInfo->accessAllowed, FieldInfo->accessCalloutHelper);

  switch (FieldInfo->fieldAccessor) {
  case CORINFO_FIELD_STATIC_TLS:
  case CORINFO_FIELD_STATIC_ADDR_HELPER: {
    // Need to generate a helper call to get the address.
    // The first argument to the helper is the field handle.
    IRNode *FieldHandleNode = genericTokenToNode(ResolvedToken);
    IRNode *PointerNode = makePtrDstGCOperand(true);

    // Now make the call and attach the arguments.
    const bool MayThrow = false;
    return callHelper(FieldInfo->helper, MayThrow, PointerNode,
                      FieldHandleNode);
  }

  case CORINFO_FIELD_STATIC_SHARED_STATIC_HELPER:
  case CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER: {
    // We access shared statics using a helper call to get the address
    // of the base of the shared statics, then we add on the offset
    // for the static field that we are accessing.
    IRNode *SharedStaticsBaseNode;

    if (FieldInfo->fieldAccessor ==
        CORINFO_FIELD_STATIC_GENERICS_STATIC_HELPER) {
      IRNode *ClassHandleNode = genericTokenToNode(ResolvedToken, true, false);

      // classHandle is not sufficient to uniquely identify the static base
      // when we are dealing with generics (as identified by the runtime
      // telling us to use these particular helper calls).
      //
      // Again, we happen to know that the results of these helper calls should
      // be interpreted as interior GC pointers.
      IRNode *TempNode = makePtrNode(Reader_PtrGcInterior);

      // Now make the call and attach the arguments.
      const bool MayThrow = false;
      SharedStaticsBaseNode =
          callHelper(FieldInfo->helper, MayThrow, TempNode, ClassHandleNode);
    } else {
      CorInfoHelpFunc HelperId = FieldInfo->helper;
      CORINFO_CLASS_HANDLE Class = ResolvedToken->hClass;

      bool NoCtor = false;

      // Class is only unique *IF* we have one of these non-generic
      // helper calls.  Otherwise DomInfoDominatorDefinesSharedStaticBase
      // could return the wrong answer (due to multiple classes having the
      // same Class).
      ASSERTNR(
          HelperId == CORINFO_HELP_GETSHARED_GCSTATIC_BASE ||
          HelperId == CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE ||
          HelperId == CORINFO_HELP_GETSHARED_GCSTATIC_BASE_NOCTOR ||
          HelperId == CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR ||
          HelperId == CORINFO_HELP_GETSHARED_GCSTATIC_BASE_DYNAMICCLASS ||
          HelperId == CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_DYNAMICCLASS ||
          HelperId == CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE ||
          HelperId == CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE ||
          HelperId == CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR ||
          HelperId == CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE_NOCTOR ||
          HelperId == CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_DYNAMICCLASS ||
          HelperId ==
              CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE_DYNAMICCLASS ||
          HelperId == CORINFO_HELP_READYTORUN_STATIC_BASE);

      // If possible, use the result of a previous sharedStaticBase call.
      // This will possibly switch the HelperId to a NoCtor version
      // It will also set NoCtor in the case where no static ctor is needed
      // but there's not a helper call for that, just different intrinsics.
      SharedStaticsBaseNode = domInfoDominatorDefinesSharedStaticBase(
          CurrentFgNode, HelperId, Class, &NoCtor);

      if (SharedStaticsBaseNode == nullptr) {
        bool CanMoveUp = false;

        if (HelperId == CORINFO_HELP_GETSHARED_GCSTATIC_BASE ||
            HelperId == CORINFO_HELP_GETSHARED_GCSTATIC_BASE_NOCTOR ||
            HelperId == CORINFO_HELP_GETSHARED_GCSTATIC_BASE_DYNAMICCLASS ||
            HelperId == CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_DYNAMICCLASS ||
            HelperId == CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE ||
            HelperId == CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR ||
            HelperId ==
                CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_DYNAMICCLASS) {
          // Again, we happen to know that the results of these helper
          // calls should be interpreted as interior GC pointers.
          SharedStaticsBaseNode = makePtrNode(Reader_PtrGcInterior);
        } else {
          SharedStaticsBaseNode = makePtrNode();
        }

        // This intrinsic is 'moveable' if it either doesn't call the .cctor
        // or the classs is marked with the relaxed semmantics
        // (BeforeFieldInit).
        if (NoCtor) {
          CanMoveUp = true;
        } else {
          // Check to see if this is a relaxed static init class, and
          // thus the .cctor can be CSE'd and otherwise moved around.
          // If yes, then try and move it outside loops.
          uint32_t ClassAttribs = getClassAttribs(Class);
          if (ClassAttribs & CORINFO_FLG_BEFOREFIELDINIT) {
            CanMoveUp = true;
          }
        }

        // Record that this block initialized class typeRef.
        domInfoRecordClassInit(CurrentFgNode, Class);

        if (Flags & CORJIT_FLG_READYTORUN) {
          assert(HelperId == CORINFO_HELP_READYTORUN_STATIC_BASE);
          InfoAccessType AccessType = FieldInfo->fieldLookup.accessType;
          void *Address = FieldInfo->fieldLookup.addr;
          assert(Address != nullptr);
          assert(AccessType != IAT_PPVALUE);
          bool IsIndirect = (AccessType != IAT_VALUE);
          const bool IsRelocatable = true;
          const bool IsCallTarget = false;
          void *RealHandle = nullptr;
          IRNode *HelperAddress = handleToIRNode(
              ResolvedToken->token, Address, RealHandle, IsIndirect, IsIndirect,
              IsRelocatable, IsCallTarget);
          const bool MayThrow = false;
          SharedStaticsBaseNode = callHelper(HelperId, HelperAddress, MayThrow,
                                             SharedStaticsBaseNode);
        } else {
          // Call helper under rdrCallGetStaticBase uses dst operand
          // to infer type for the call instruction.
          // Ideally, we should pass the type, which needs refactoring.
          // So, SharedStaticsBaseNode now becomes the defined instruction.
          SharedStaticsBaseNode =
              rdrCallGetStaticBase(Class, ResolvedToken->token, HelperId,
                                   NoCtor, CanMoveUp, SharedStaticsBaseNode);
        }

        // Record (def) instruction that holds shared statics base
        domInfoRecordSharedStaticBaseDefine(CurrentFgNode, HelperId, Class,
                                            SharedStaticsBaseNode);
      }
    }

    // This is an offset into the shared static table, it is usually
    // non-zero, it won't do much to optimize this. FURTHERMORE: the
    // add tells the garbage collector that the base pointer is
    // interior (even for an add of zero), so this add is necessary
    // for GC to work.
    AddressNode = binaryOp(ReaderBaseNS::Add, SharedStaticsBaseNode,
                           loadConstantI(FieldInfo->offset));

    // This occurs for a static value classes
    //
    // In such a case the fieldaddress is the address of a handle The
    // handle points at a boxed value class (which is an object
    // reference) The value class data is at a pointer-sized offset in
    // the object
    if (FieldInfo->fieldFlags & CORINFO_FLG_FIELD_STATIC_IN_HEAP) {
      // If the field is a boxed valueclass, the address returned
      // points to the boxed data. So the real address is at
      // [fieldAddress]+sizeof(void*).
      IRNode *BoxedAddressNode;
      const uint32_t Offset = getPointerByteSize();

      BoxedAddressNode = derefAddressNonNull(AddressNode, true, true);
      AddressNode =
          binaryOp(ReaderBaseNS::Add, BoxedAddressNode, loadConstantI(Offset));
    }

    return AddressNode;
  }

  case CORINFO_FIELD_STATIC_ADDRESS:
  case CORINFO_FIELD_STATIC_RVA_ADDRESS:
    break;

  default:
    ASSERTMNR(UNREACHED, "Unknown fieldAccessor");
  }

  CORINFO_FIELD_HANDLE Field = ResolvedToken->hField;

  // Emit a call to run the Class Constructor if necessary
  if (accessStaticFieldRequiresClassConstructor(Field)) {
    CORINFO_CLASS_HANDLE Class = ResolvedToken->hClass;

    // If class hasn't been initialized on this path
    if (!domInfoDominatorHasClassInit(CurrentFgNode, Class)) {
      // Use the shared static base helper as it is faster than InitClass
      // getSharedCCtorHelper returns one of these three values:
      //     CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE
      //     CORINFO_HELP_GETGENERICS_NONGCSTATIC_BASE
      //     CORINFO_HELP_CLASSINIT_SHARED_DYNAMICCLASS

      CorInfoHelpFunc HelperId = getSharedCCtorHelper(Class);

      bool NoCtor = false;
      bool CanMoveUp = false;

      // This will possibly switch the helperId to a NoCtor version
      // It will also set NoCtor in the case where no static ctor is needed
      // but there's not helper call for that, just different intrinsics.
      domInfoDominatorDefinesSharedStaticBase(CurrentFgNode, HelperId, Class,
                                              &NoCtor);

      // We shouldn't have gotten here if the Ctor has already run.
      ASSERTNR(!NoCtor);

      // Check to see if this is a relaxed static init class, and thus
      // the .cctor can be CSE'd and otherwise moved around.  If yes,
      // then try and move it outside loops.
      uint32_t ClassAttribs = getClassAttribs(Class);
      if (ClassAttribs & CORINFO_FLG_BEFOREFIELDINIT) {
        CanMoveUp = true;
      }

      // call the faster init class helper
      rdrCallGetStaticBase(Class, ResolvedToken->token, HelperId, NoCtor,
                           CanMoveUp, nullptr);

      // Record that this block initialized this class.
      domInfoRecordClassInit(CurrentFgNode, Class);
    }
  }

  // The EE knows that address already, so get it and
  // stuff it into an INTCONST.

  // Get the address of the field.
  bool IsIndirect;
  void *FieldAddress = getStaticFieldAddress(Field, &IsIndirect);
  ASSERTNR(FieldAddress != nullptr);

#if !defined(NODEBUG)
  CORINFO_CLASS_HANDLE Class;
  CorInfoType TheCorInfoType;
  uint32_t MinClassAlign;

  TheCorInfoType = FieldInfo->fieldType;
  Class = FieldInfo->structType;

  if ((TheCorInfoType == CORINFO_TYPE_REFANY) ||
      (TheCorInfoType == CORINFO_TYPE_VALUECLASS)) {
    if (FieldInfo->fieldFlags & CORINFO_FLG_FIELD_STATIC_IN_HEAP) {
      MinClassAlign = sizeof(char *); // alignment is size of pointer
    } else {
      MinClassAlign = getClassAlignmentRequirement(Class);
    }
  } else {
    MinClassAlign = 0;
  }

  verifyStaticAlignment(FieldAddress, TheCorInfoType, MinClassAlign);
#endif

  // We can't associate the field address with the field handle,
  // because there might also be a ldtoken on the same field, which
  // wants to associate a different value with the field handle.  So
  // we take advantage of the fact that handles are really pointers
  // and thus never odd.
  ASSERTNR((CORINFO_FIELD_HANDLE)((size_t)Field & 1) == nullptr);
  AddressNode = handleToIRNode(ResolvedToken->token, FieldAddress,
                               (CORINFO_FIELD_HANDLE)((size_t)Field | 1),
                               IsIndirect, IsIndirect, true, false);

  // We can be have both IsIndirect and fBoxed This occurs for a static
  // value classes
  //
  // In such a case the fieldaddress is the address of a handle The
  // handle points at a boxed value class (which is an object
  // reference) The value class data is at offset 8 in the object
  if (FieldInfo->fieldFlags & CORINFO_FLG_FIELD_STATIC_IN_HEAP) {
    // If the field is a boxed valueclass, the address returned points
    // to the boxed data. So the real address is at
    // [FieldAddress]+sizeof(void*).
    IRNode *BoxedAddressNode;
    const uint32_t Offset = getPointerByteSize();
    BoxedAddressNode = derefAddressNonNull(AddressNode, true, true);
    AddressNode =
        binaryOp(ReaderBaseNS::Add, BoxedAddressNode, loadConstantI(Offset));
  }

  return AddressNode;
}

IRNode *ReaderBase::rdrGetFieldAddress(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                       CORINFO_FIELD_INFO *FieldInfo,
                                       IRNode *Obj, bool BaseIsGCObj,
                                       bool MustNullCheck) {
  if (FieldInfo->fieldFlags & CORINFO_FLG_FIELD_STATIC) {
    return rdrGetStaticFieldAddress(ResolvedToken, FieldInfo);
  }

  handleMemberAccess(FieldInfo->accessAllowed, FieldInfo->accessCalloutHelper);

  if ((FieldInfo->fieldAccessor != CORINFO_FIELD_INSTANCE) &&
      (FieldInfo->fieldAccessor != CORINFO_FIELD_INSTANCE_WITH_BASE)) {
    // Need to generate a helper call to get the address.
    // the helper calls do an explicit null check
    IRNode *Arg1, *Arg2, *Dst;

    Arg1 = Obj;

    // The second argument to the helper is the field handle.
    Arg2 = genericTokenToNode(ResolvedToken);

    // Get the func. descr.
    if (BaseIsGCObj) {
      Dst = makePtrDstGCOperand(true);
    } else {
      Dst = makePtrNode();
    }

    const bool MayThrow = true;
    return callHelper(FieldInfo->helper, MayThrow, Dst, Arg1, Arg2);
  } else {
    // Get the offset, add it to the this pointer to calculate the
    // actual address of the field.
    const uint32_t FieldOffset = FieldInfo->offset;

    // If the offset is bigger than MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT,
    //  then we need to insert an explicit null check on the object pointer.
    if (MustNullCheck ||
        (FieldOffset >= MAX_UNCHECKED_OFFSET_FOR_NULL_OBJECT)) {
      Obj = genNullCheck(Obj);
    }

    IRNode *Address = simpleFieldAddress(Obj, ResolvedToken, FieldInfo);

    return Address;
  }
}

IRNode *ReaderBase::loadToken(CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  CORINFO_GENERIC_HANDLE CompileTimeHandleValue = nullptr;
  bool RequiresRuntimeLookup = true;
  IRNode *GetTokenNumericNode =
      genericTokenToNode(ResolvedToken, false, true, &CompileTimeHandleValue,
                         &RequiresRuntimeLookup);
  IRNode *LdtokenNode;

  CorInfoHelpFunc Helper = CORINFO_HELP_UNDEF;
  CORINFO_CLASS_HANDLE TokenType = JitInfo->getTokenTypeAsHandle(ResolvedToken);

  if (ResolvedToken->hMethod != nullptr) {
    Helper = CORINFO_HELP_METHODDESC_TO_STUBRUNTIMEMETHOD;
  } else if (ResolvedToken->hField != nullptr) {
    Helper = CORINFO_HELP_FIELDDESC_TO_STUBRUNTIMEFIELD;
  } else {
    Helper = CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE;
  }

  LdtokenNode = convertHandle(GetTokenNumericNode, Helper, TokenType);

  // TODO: use intrinsic instead of a helper call for
  // CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE.

  bool ConvertToIntrinsic = false;

  if (ConvertToIntrinsic) {
    if (Helper == CORINFO_HELP_TYPEHANDLE_TO_RUNTIMETYPE) {
      bool CanCompareToGetType = false;

      if (!RequiresRuntimeLookup &&
          canInlineTypeCheckWithObjectVTable(
              (CORINFO_CLASS_HANDLE)CompileTimeHandleValue)) {
        CanCompareToGetType = true;
      }

      convertTypeHandleLookupHelperToIntrinsic(CanCompareToGetType);
    }
  }
  return LdtokenNode;
}

void ReaderBase::getCallSiteSignature(CORINFO_METHOD_HANDLE Method,
                                      mdToken Token, CORINFO_SIG_INFO *Sig,
                                      bool *HasThis) {
  getCallSiteSignature(Method, Token, Sig, HasThis, getCurrentContext(),
                       getCurrentModuleHandle());
}

void ReaderBase::getCallSiteSignature(CORINFO_METHOD_HANDLE Method,
                                      mdToken Token, CORINFO_SIG_INFO *Sig,
                                      bool *HasThis,
                                      CORINFO_CONTEXT_HANDLE Context,
                                      CORINFO_MODULE_HANDLE Scope) {
  CORINFO_CLASS_HANDLE ActualMethodRetTypeSigClass = nullptr;

  // See if we have been given a method token or a signature token.
  if (Method) {
    // Get the complete signature information for this method.
    // At the moment, findCallSiteSig() does not ever say that a this
    // pointer is needed, so use getMethodSig() to find that out, and then
    // use findCallSiteSig() so that we correctly handle varargs functions.
    JitInfo->getMethodSig(Method, Sig);
    *HasThis = (Sig->hasThis() != 0);
    ActualMethodRetTypeSigClass = Sig->retTypeSigClass;

    if (Sig->isVarArg()) {
      JitInfo->findCallSiteSig(Scope, Token, Context, Sig);
    }
  } else {
    // Get the signature information using the given signature token.
    JitInfo->findSig(Scope, Token, Context, Sig);
    *HasThis = (Sig->hasThis() != 0);
  }

#ifndef CC_PEVERIFY
  // We need to ensure that we have loaded the classes of
  // any value type return values that we have (including enums).
  CorInfoType CorType = Sig->retType;
  if (CorType != CORINFO_TYPE_CLASS && CorType != CORINFO_TYPE_BYREF &&
      CorType != CORINFO_TYPE_PTR && Sig->retTypeSigClass != nullptr) {
    // Ensure that the class is restored
    JitInfo->classMustBeLoadedBeforeCodeIsRun(Sig->retTypeSigClass);

    // For vararg calls we must be sure to load the return type of the
    // method actually being called, as well as the return types of the
    // specified in the vararg signature. With type equivalency, these types
    // may not be the same.
    if ((ActualMethodRetTypeSigClass != Sig->retTypeSigClass) &&
        (ActualMethodRetTypeSigClass != nullptr)) {
      JitInfo->classMustBeLoadedBeforeCodeIsRun(ActualMethodRetTypeSigClass);
    }
  }
#endif // CC_PEVERIFY
}

IRNode *ReaderBase::rdrMakeNewObjThisArg(ReaderCallTargetData *CallTargetData,
                                         CorInfoType CorType,
                                         CORINFO_CLASS_HANDLE Class) {
  uint32_t ClassAttribs = CallTargetData->getClassAttribs();

  bool IsMDArray = ((ClassAttribs & CORINFO_FLG_ARRAY) != 0);
  if (IsMDArray) {
    // Storage for MDArrays is allocated by the callee; these do not take a this
    // argument.
    return nullptr;
  }

  return genNewObjThisArg(CallTargetData, CorType, Class);
}

IRNode *
ReaderBase::rdrMakeNewObjReturnNode(ReaderCallTargetData *CallTargetData,
                                    IRNode *ThisArg, IRNode *CallReturnNode) {
  uint32_t ClassAttribs = CallTargetData->getClassAttribs();

  bool IsVarObjSize = ((ClassAttribs & CORINFO_FLG_VAROBJSIZE) != 0);

  // MDArrays should already have been taken care of.
  ASSERT((ClassAttribs & CORINFO_FLG_ARRAY) == 0);

  if (IsVarObjSize) {
    // Storage for variably-sized objects is allocated by the callee; the result
    // of the call is already correct.
    return CallReturnNode;
  }

  return genNewObjReturnNode(CallTargetData, ThisArg);
}

inline CorInfoHelpFunc eeGetHelperNum(CORINFO_METHOD_HANDLE Method) {
  // Helpers are marked by the fact that they are odd numbers
  if (!(((size_t)Method) & 1))
    return (CORINFO_HELP_UNDEF);
  return ((CorInfoHelpFunc)(((size_t)Method) >> 2));
}

inline bool eeIsNativeMethod(CORINFO_METHOD_HANDLE Method) {
  return ((((size_t)Method) & 0x2) == 0x2);
}

const char *ReaderBase::getMethodName(CORINFO_METHOD_HANDLE Method,
                                      const char **ClassNamePtr,
                                      ICorJitInfo *JitInfo) {
  if (eeGetHelperNum(Method)) {
    return 0;
  }

  if (eeIsNativeMethod(Method)) {
    return 0;
  }

  return JitInfo->getMethodName(Method, ClassNamePtr);
}

// Constraint calls in generic code.  Constraint calls are operations on generic
// type variables,
// e.g. "x.Incr()" where "x" has type "T" in generic code and where "T" supports
// an interface
// (e.g. IIncr) with an "Incr" method.  They are prefixed by the "constrained."
// prefix, indicating which "T" is being invoked.
//
// Contraint calls have a very simple interpretation when generating specialized
// code: if T is a value type VC then
//        <load-a-byref-to-T>
//        constrained.
//        callvirt I::Incr()
// becomes
//        <load-a-byref-to-VC>
//        call VC::Incr()      <-- this is the MethodDesc that accpets a unboxed
//        "this" pointer
// and if T is a reference type R then this becomes
//        <load-a-byref-to-R>
//        <dereference-the-byref>
//        callvirt R::Incr()
// There are some situations where a MethodDesc that accepts an unboxed "this"
// pointer is not available, in which case the call becomes
//        <load-a-byref-to-VC>
//        <box-the-byref>
//        callvirt R::Incr()      <-- this will call is the MethodDesc that
//        accpets a boxed "this" pointer
//
// The above interpretations make sense even when "Incr" is a generic
// method, or when VC or I are types that are shared amongst generic
// instantiations, e.g. VC<object> or I<object>.
//
// The idea is that for value types the actual target of the constraint call
// is determined here.  Depending on the nature of the constraint call we may
// have to either dereference or box the "this" pointer.
//
// LoadFtnToken parameter is used only to optimize the delegate
// constructor codepath.  It contains the argument to the CEE_LDFTN or
// CEE_LDVIRTFUNC opcode when we have a CEE_NEWOBJ opcode.  The
// CEE_LDFTN opcode must immedatiately preceed the CEE_NEWOBJ opcode
// and both must be in the same basic block.  If any of these
// conditions are not true then LoadFtnToken will be mdTokenNil
IRNode *
ReaderBase::rdrCall(ReaderCallTargetData *Data, ReaderBaseNS::CallOpcode Opcode,
                    IRNode **CallNode) { // out param is defined by GenCall
  IRNode *ReturnNode;
  uint32_t NumArgs;
  int32_t FirstArgNum;
  int32_t TypeArgNum;
  int32_t VarArgNum;
  bool HasThis;
  bool IsVarArg;
  bool HasTypeArg;
  uint32_t TotalArgs;
  int32_t Index;
  CORINFO_CALL_INFO *CallInfo = Data->getCallInfo();

  // Tail call is only permitted for call, calli and callvirt.
  ASSERTNR(!Data->isTailCall() || (Opcode == ReaderBaseNS::Call) ||
           (Opcode == ReaderBaseNS::Calli) ||
           (Opcode == ReaderBaseNS::CallVirt));

  // Constrained call is only permitted on callvirt
  ASSERTNR((Data->getConstraintToken() == mdTokenNil) ||
           (Opcode == ReaderBaseNS::CallVirt));

  // Readonly call prefix only for Address operation on arrays
  ASSERTNR(!Data->isReadOnlyCall() || (Opcode == ReaderBaseNS::Call) ||
           (Opcode == ReaderBaseNS::Calli) ||
           (Opcode == ReaderBaseNS::CallVirt));

  // LoadFtnToken is only permitted when we are processing newobj call
  ASSERTNR((Data->getLoadFtnToken() == mdTokenNil) ||
           (Opcode == ReaderBaseNS::NewObj));

  // For certain intrinsics, we can determine that the call has no
  // side effects ...
  bool CallCanSideEffect = true;
  bool MayThrow = true;

  // TODO: readonly work for calls

  // Get the number of parameters from the signature (sig.NumArgs +
  // fHasThis) Also work out the target of the call.  See if this is a
  // direct or indirect call.
  if (ReaderBaseNS::Calli == Opcode) {
    Data->CallTargetNode = ReaderOperandStack->pop();
  } else {
    handleMemberAccess(CallInfo->accessAllowed,
                       CallInfo->callsiteCalloutHelper);

    // If the current method calls a method which needs a security
    // check, we need to reserve a slot for the security object in
    // the current method's stack frame
    if (Data->getMethodAttribs() & CORINFO_FLG_SECURITYCHECK) {
      methodNeedsSecurityCheck();
    }

#ifndef CC_PEVERIFY
    // An intrinsic! Ask if client would like to expand it.
    if ((Data->getMethodAttribs() & CORINFO_FLG_INTRINSIC)
        // if we're going to have to mess around with Args don't bother
        && (!Data->hasThis() ||
            CallInfo->thisTransform == CORINFO_NO_THIS_TRANSFORM)) {
      // assert(!(mflags & CORINFO_FLG_VIRTUAL) ||
      //        (mflags & CORINFO_FLG_FINAL) ||
      //        (clsFlags & CORINFO_FLG_FINAL));

      CorInfoIntrinsics IntrinsicID =
          JitInfo->getIntrinsicID(Data->getMethodHandle());

      if ((0 <= IntrinsicID) && (IntrinsicID < CORINFO_INTRINSIC_Count)) {
        IRNode *IntrinsicArg1, *IntrinsicArg2, *IntrinsicArg3, *IntrinsicRet;

        Data->CorIntrinsicId = IntrinsicID;
        IntrinsicRet = nullptr;
        switch (IntrinsicID) {
        case CORINFO_INTRINSIC_Array_GetDimLength:
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicArg2 = (IRNode *)ReaderOperandStack->pop();

          IntrinsicRet =
              arrayGetDimLength(IntrinsicArg1, IntrinsicArg2, CallInfo);
          if (IntrinsicRet) {
            return IntrinsicRet;
          }

          ReaderOperandStack->push(IntrinsicArg2);
          ReaderOperandStack->push(IntrinsicArg1);
          CallCanSideEffect = false;
          break;

        case CORINFO_INTRINSIC_Sin:
          break;
        case CORINFO_INTRINSIC_Cos:
          break;

        case CORINFO_INTRINSIC_Sqrt:
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();

          if (sqrt(IntrinsicArg1, &IntrinsicRet))
            return IntrinsicRet;

          ReaderOperandStack->push(IntrinsicArg1);
          break;

        case CORINFO_INTRINSIC_InterlockedXAdd32:
        case CORINFO_INTRINSIC_InterlockedXAdd64:
        case CORINFO_INTRINSIC_InterlockedXchg32:
        case CORINFO_INTRINSIC_InterlockedXchg64: {
          IntrinsicArg2 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();

          if (interlockedIntrinsicBinOp(IntrinsicArg1, IntrinsicArg2,
                                        &IntrinsicRet, IntrinsicID))
            return IntrinsicRet;

          ReaderOperandStack->push(IntrinsicArg1);
          ReaderOperandStack->push(IntrinsicArg2);
          CallCanSideEffect = true;
          break;
        }

        case CORINFO_INTRINSIC_InterlockedCmpXchg32:
        case CORINFO_INTRINSIC_InterlockedCmpXchg64:
          IntrinsicArg3 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicArg2 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();

          if (interlockedCmpXchg(IntrinsicArg1, IntrinsicArg2, IntrinsicArg3,
                                 &IntrinsicRet, IntrinsicID))
            return IntrinsicRet;

          ReaderOperandStack->push(IntrinsicArg1);
          ReaderOperandStack->push(IntrinsicArg2);
          ReaderOperandStack->push(IntrinsicArg3);
          CallCanSideEffect = true;
          break;

        case CORINFO_INTRINSIC_MemoryBarrier:
          if (memoryBarrier())
            return IntrinsicRet;

          CallCanSideEffect = true;
          break;

        case CORINFO_INTRINSIC_Abs:
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();

          if (abs(IntrinsicArg1, &IntrinsicRet))
            return IntrinsicRet;

          ReaderOperandStack->push(IntrinsicArg1);
          break;

        case CORINFO_INTRINSIC_Array_Get:
          if (arrayGet(Data->getSigInfo(), &IntrinsicRet)) {
            return IntrinsicRet;
          }
          break;

        case CORINFO_INTRINSIC_Array_Address:
          if (arrayAddress(Data->getSigInfo(), &IntrinsicRet)) {
            return IntrinsicRet;
          }
          break;

        case CORINFO_INTRINSIC_Array_Set:
          if (arraySet(Data->getSigInfo())) {
            return nullptr;
          }
          break;

        case CORINFO_INTRINSIC_Round:
          break;

        case CORINFO_INTRINSIC_StringLength:
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();

          IntrinsicRet = loadStringLen(IntrinsicArg1);
          if (IntrinsicRet) {
            return IntrinsicRet;
          }

          ReaderOperandStack->push(IntrinsicArg1);
          CallCanSideEffect = false;
          break;

        case CORINFO_INTRINSIC_StringGetChar:
          IntrinsicArg2 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();

          IntrinsicRet = stringGetChar(IntrinsicArg1, IntrinsicArg2);
          if (IntrinsicRet) {
            return IntrinsicRet;
          }

          ReaderOperandStack->push(IntrinsicArg1);
          ReaderOperandStack->push(IntrinsicArg2);
          CallCanSideEffect = true;
          break;

        case CORINFO_INTRINSIC_InitializeArray:
          break;
        case CORINFO_INTRINSIC_GetChar:
          break;

        case CORINFO_INTRINSIC_GetTypeFromHandle:
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicRet = getTypeFromHandle(IntrinsicArg1);

          if (IntrinsicRet) {
            return IntrinsicRet;
          }

          ReaderOperandStack->push(IntrinsicArg1);
          CallCanSideEffect = false;
          break;

        case CORINFO_INTRINSIC_RTH_GetValueInternal:
          IntrinsicArg1 = (IRNode *)ReaderOperandStack->pop();
          IntrinsicRet = getValueFromRuntimeHandle(IntrinsicArg1);

          if (IntrinsicRet) {
            return IntrinsicRet;
          }

          ReaderOperandStack->push(IntrinsicArg1);
          CallCanSideEffect = false;
          break;

        case CORINFO_INTRINSIC_Object_GetType:
          break;

        case CORINFO_INTRINSIC_StubHelpers_GetStubContext:
          IntrinsicRet = secretParam();
          if (IntrinsicRet) {
            return IntrinsicRet;
          }
          break;

#ifdef _WIN64
        case CORINFO_INTRINSIC_StubHelpers_GetStubContextAddr:
          IntrinsicRet = secretParam();
          IntrinsicRet = addressOfValue(IntrinsicRet);
          if (IntrinsicRet) {
            return IntrinsicRet;
          }
          break;
#endif

        default:
          break;
        }
      }
    }

    CORINFO_CLASS_HANDLE Class = Data->getClassHandle();
    CORINFO_SIG_INFO *SigInfo = Data->getSigInfo();
    if (doSimdIntrinsicOpt() && JitInfo->isInSIMDModule(Class)) {
      IRNode *ReturnNode = nullptr;
      CORINFO_METHOD_HANDLE Method = Data->getMethodHandle();
      ReturnNode = generateSIMDIntrinsicCall(Class, Method, SigInfo, Opcode);
      if (ReturnNode) {
        if (SigInfo->retType == CorInfoType::CORINFO_TYPE_VOID &&
            Opcode != ReaderBaseNS::CallOpcode::NewObj) {
          return 0;
        }
        return ReturnNode;
      }
    }

    // Check for Delegate Constructor optimization
    if (rdrCallIsDelegateConstruct(Data)) {
      if (Data->getLoadFtnToken() != mdTokenNil) {
        CORINFO_RESOLVED_TOKEN ResolvedFtnToken;
        resolveToken(Data->getLoadFtnToken(), CORINFO_TOKENKIND_Method,
                     &ResolvedFtnToken);

        CORINFO_METHOD_HANDLE TargetMethod = ResolvedFtnToken.hMethod;
#ifdef FEATURE_CORECLR
        {
          // Do the CoreClr delegate transparency rule check before calling
          // the delegate constructor
          CORINFO_CLASS_HANDLE DelegateType = Data->getClassHandle();
          CORINFO_METHOD_HANDLE CalleeMethod = TargetMethod;
          mdToken TargetMethodToken = Data->getMethodToken();
          rdrInsertCalloutForDelegate(DelegateType, CalleeMethod,
                                      TargetMethodToken);
        }
#endif // FEATURE_CORECLR
        if (Flags & CORJIT_FLG_READYTORUN) {
          ReaderOperandStack->pop();
          IRNode *TargetObject = ReaderOperandStack->pop();
          assert((Data->getClassAttribs() & CORINFO_FLG_VALUECLASS) == 0);
          IRNode *NewObjThisArg = rdrMakeNewObjThisArg(Data, CORINFO_TYPE_CLASS,
                                                       Data->getClassHandle());
          const bool MayThrow = false;
          callReadyToRunHelper(CORINFO_HELP_READYTORUN_DELEGATE_CTOR, MayThrow,
                               nullptr, &ResolvedFtnToken, NewObjThisArg,
                               TargetObject);
          return NewObjThisArg;
        } else if (TargetMethod) {
          ASSERTNR(Data->hasThis());
          ASSERTNR(Data->isNewObj());

          CORINFO_CLASS_HANDLE Class = Data->getClassHandle();
          CORINFO_METHOD_HANDLE Method = Data->getMethodHandle();
          ASSERTNR(CallInfo);

          CORINFO_METHOD_HANDLE AlternateCtor = nullptr;

          DelegateCtorArgs *CtorData =
              (DelegateCtorArgs *)getTempMemory(sizeof(DelegateCtorArgs));
          memset(CtorData, 0, sizeof(DelegateCtorArgs));
          CtorData->pMethod = getCurrentMethodHandle();

          AlternateCtor =
              JitInfo->GetDelegateCtor(Method, Class, TargetMethod, CtorData);

          if (AlternateCtor != Method) {
            // TODO: Ideally we would like the JIT to inline this alternate
            // ctor method

            Data->setOptimizedDelegateCtor(AlternateCtor);

            IRNode *ArgIR;
            mdToken TargetMethodToken = Data->getMethodToken();

            // Add the additional Args (if any)
            if (CtorData->pArg3) {
              ArgIR =
                  handleToIRNode(TargetMethodToken, CtorData->pArg3,
                                 CtorData->pArg3, false, false, true, false);
              ReaderOperandStack->push(ArgIR);
              if (CtorData->pArg4) {
                ArgIR =
                    handleToIRNode(TargetMethodToken, CtorData->pArg4,
                                   CtorData->pArg4, false, false, true, false);
                ReaderOperandStack->push(ArgIR);
                if (CtorData->pArg5) {
                  ArgIR = handleToIRNode(TargetMethodToken, CtorData->pArg5,
                                         CtorData->pArg5, false, false, true,
                                         false);
                  ReaderOperandStack->push(ArgIR);
                }
              }
            }
          }
        }
      }
    }

#endif // not CC_PEVERIFY
  }
  CORINFO_SIG_INFO *SigInfo = Data->getSigInfo();
  // Set the calling convention.
  Data->CallTargetSignature.HasThis = Data->hasThis();
  Data->CallTargetSignature.CallingConvention = SigInfo->getCallConv();

  // Get the number of arguments to this method.
  NumArgs = (uint32_t)SigInfo->numArgs;
  HasThis = Data->hasThis();
  IsVarArg = SigInfo->isVarArg();

  // Extra argument containing instantiation information is passed in the
  // following circumstances:
  // (a) To the AddressAt method on array classes; the extra parameter is the
  //     array's type handle (a TypeDesc)
  // (b) To shared-code instance methods in generic structs; the extra parameter
  //     is the struct's type handle (a vtable ptr)
  // (c) To shared-code per-instantiation non-generic static methods in generic
  //     classes and structs; the extra parameter is the type handle
  // (d) To shared-code generic methods; the extra parameter is an
  //     exact-instantiation MethodDesc
  //
  // For shared instance target, we must add the instantiation parameter
  // as an argument.  It is ordered after the vararg cookie and before the
  // regular arguments.
  // However, if a helper call provides the function address, it always gives us
  // an instantiating stub, so don't add an instantiation parameter.
  HasTypeArg = SigInfo->hasTypeArg() &&
               (CallInfo->kind != CORINFO_VIRTUALCALL_LDVIRTFTN);
  TotalArgs =
      (HasThis ? 1 : 0) + (IsVarArg ? 1 : 0) + (HasTypeArg ? 1 : 0) + NumArgs;

  FirstArgNum = 0;

  // Special case for newobj: the first argument is not on the stack.
  IRNode *NewObjThisArg = nullptr;
  if (Data->isNewObj()) {
    ASSERTNR(HasThis); // new obj better have "this"
    FirstArgNum = 1;
  }

  VarArgNum = IsVarArg ? (HasThis ? 1 : 0) : -1;
  TypeArgNum = HasTypeArg ? ((HasThis ? 1 : 0) + (IsVarArg ? 1 : 0)) : -1;

  // Create return info.
  Data->CallTargetSignature.ResultType = {SigInfo->retType,
                                          SigInfo->retTypeClass};

  // Create arg array and populate with stack arguments.
  //
  // Note that array is populated with two loops: the first traverses the EE's
  // argument list, the second pops arguments from the stack. Two loops are
  // clearer because the data is stored with opposite orderings.

  std::vector<CallArgType> &ArgumentTypes =
      Data->CallTargetSignature.ArgumentTypes;

  // Initialize ArgumentTypes for the first time
  //
  // CallTargetSignature's constructor has not been called at this
  // point. Therefore, a placement new is required to initialize
  // ArgumentTypes.

  new (&ArgumentTypes) std::vector<CallArgType>(TotalArgs);
  std::vector<IRNode *> Arguments(TotalArgs);

  // First populate argument type information.
  if (TotalArgs > 0) {
    CORINFO_ARG_LIST_HANDLE Args;
    CorInfoType CorType;
    CORINFO_CLASS_HANDLE ArgType, Class;
    bool HasInferredThisType = false;

    Args = SigInfo->args;
    Index = 0;

    // If this call passes a this ptr, then it is first in array.
    if (HasThis) {
      if (Data->getMethodHandle() != nullptr) {
        if ((Data->getClassAttribs() & CORINFO_FLG_VALUECLASS) == 0) {
          CorType = CORINFO_TYPE_CLASS;
        } else {
          CorType = CORINFO_TYPE_BYREF;
        }

        ArgumentTypes[Index] = {CorType, Data->getClassHandle()};
      } else {
        HasInferredThisType = true;
      }
      Index++;
    }

    if (IsVarArg) {
      // TODO: we'll need to handle varargs here.
      Index++;
    }

    // If this call has a type arg, then it's right before fixed params.
    if (HasTypeArg) {
      ArgumentTypes[Index] = {CORINFO_TYPE_NATIVEINT, nullptr};
      Index++;
    }

    // Populate remaining argument list
    for (; Index < (int)TotalArgs; Index++) {
      CorType = strip(JitInfo->getArgType(SigInfo, Args, &ArgType));
      ASSERTNR(CorType != CORINFO_TYPE_VAR); // common generics trouble

      if ((CorType == CORINFO_TYPE_CLASS) ||
          (CorType == CORINFO_TYPE_VALUECLASS) ||
          (CorType == CORINFO_TYPE_BYREF) || (CorType == CORINFO_TYPE_PTR)) {
        Class = JitInfo->getArgClass(SigInfo, Args);
      } else if (CorType == CORINFO_TYPE_REFANY) {
        Class = JitInfo->getBuiltinClass(CLASSID_TYPED_BYREF);
      } else {
        Class = nullptr;
      }

#ifndef CC_PEVERIFY
      // We need to ensure that we have loaded all the classes
      // of any value type arguments that we push (including enums).
      if (CorType != CORINFO_TYPE_CLASS && CorType != CORINFO_TYPE_BYREF &&
          CorType != CORINFO_TYPE_PTR) {
        CORINFO_CLASS_HANDLE ArgRealClass = JitInfo->getArgClass(SigInfo, Args);
        if (ArgRealClass) {
          // Ensure that the class is restored
          JitInfo->classMustBeLoadedBeforeCodeIsRun(ArgRealClass);
        }
      }
#endif // CC_PEVERIFY

      ArgumentTypes[Index] = {CorType, Class};
      Args = JitInfo->getArgNext(Args);
    }

    // Now pop args from argument stack (including this)
    // - populating argument list in reverse order.
    // For newobj the this pointer is not yet on the stack
    // so don't pop it! Type arg and vararg cookie are not on the stack either.
    for (Index = TotalArgs - 1; Index >= FirstArgNum; Index--) {
      if (Index == VarArgNum) {
        // TODO: we'll need to handle varargs here.
        continue;
      }
      if (Index == TypeArgNum) {
        Arguments[Index] = Data->getTypeContextNode();
        ASSERTNR(Arguments[Index] != nullptr);
        ASSERTMNR(!Data->isCallI(), "CALLI on parameterized type");
        continue;
      }
      Arguments[Index] = (IRNode *)ReaderOperandStack->pop();
    }

    // this-pointer specific stuff
    if (HasThis) {
      if (Data->isNewObj()) {
        // Create the this argument for newobj now.
        NewObjThisArg = rdrMakeNewObjThisArg(Data, ArgumentTypes[0].CorType,
                                             ArgumentTypes[0].Class);
        Arguments[0] = NewObjThisArg;
      } else {
        Arguments[0] = Data->applyThisTransform(Arguments[0]);
      }

      if (HasInferredThisType) {
        CorInfoType CorType = CORINFO_TYPE_CLASS;

        CORINFO_CLASS_HANDLE Class = inferThisClass(Arguments[0]);
        if (Class == nullptr) {
          // Default to (CORINFO_TYPE_CLASS, Object)
          Class = getBuiltinClass(CorInfoClassId::CLASSID_SYSTEM_OBJECT);
        } else if (JitInfo->isValueClass(Class)) {
          CorType = CORINFO_TYPE_BYREF;
        }

        ArgumentTypes[0] = {CorType, Class};
      }
    }
  }

  // Remove stack interference for stack elements that will be
  // live across the call.
  //
  // We want to avoid this if possible for two reasons:
  //     1) It will reduce the number of lifetimes
  //     2) It will improve the effectiveness of optimizations.
  //        that want to see the induction variable
  //        incremented after all uses of the induction variable.

  if (CallCanSideEffect) {
    removeStackInterference();
  }

  // Perform any post-processing necessary for newobj.
  if (Data->isNewObj()) {
    uint32_t ClassAttribs = Data->getClassAttribs();

    bool IsMDArray = ((ClassAttribs & CORINFO_FLG_ARRAY) != 0);
    if (IsMDArray) {
      return genNewMDArrayCall(Data, Arguments, CallNode);
    }

    bool IsVarObjSize = ((ClassAttribs & CORINFO_FLG_VAROBJSIZE) != 0);
    if (IsVarObjSize) {
      // We are allocating an object whose size depends on constructor args
      // (e.g., string). In this case the call to the constructor will allocate
      // the object.

      Data->CallTargetSignature.ResultType = {ArgumentTypes[0].CorType,
                                              ArgumentTypes[0].Class};
    }
  }

  // Get the call target
  if (!Data->isCallI()) {
    rdrMakeCallTargetNode(Data,
                          Arguments.size() == 0 ? nullptr : &Arguments[0]);
    ASSERT(!Data->isNewObj() || Arguments[0] == NewObjThisArg);
  }

  // Ask GenIR to emit call, optionally returns a ReturnNode.
  ReturnNode = genCall(Data, MayThrow, Arguments, CallNode);

  if (Data->isNewObj()) {
    ReturnNode = rdrMakeNewObjReturnNode(Data, NewObjThisArg, ReturnNode);
  }

  return ReturnNode;
}

bool ReaderBase::rdrCallIsDelegateInvoke(ReaderCallTargetData *CallTargetData) {
  uint32_t MethodAttribs = CallTargetData->getMethodAttribs();
  if ((MethodAttribs & CORINFO_FLG_DELEGATE_INVOKE) != 0) {
    ASSERTNR(!(MethodAttribs & CORINFO_FLG_STATIC));
    ASSERTNR(MethodAttribs & CORINFO_FLG_FINAL);
    return true;
  }
  return false;
}

void ReaderBase::rdrMakeCallTargetNode(ReaderCallTargetData *CallTargetData,
                                       IRNode **ThisPtr) {
  CORINFO_CALL_INFO *CallInfo = CallTargetData->getCallInfo();
  ASSERTNR(CallInfo);
  IRNode *Target;

  if (CallInfo == nullptr) {
    return;
  }

  // Check for Delegate Invoke optimization
  if (rdrCallIsDelegateInvoke(CallTargetData)) {
    Target = rdrGetDelegateInvokeTarget(CallTargetData, ThisPtr);
  } else {
    // Insert the code sequence to load a pointer to the target function.
    // If that sequence involves dereferencing the current method's instance
    // parameter (e.g. to find dynamic type parameter information for shared
    // generic code) or the target method's instance argument (e.g. to look up
    // its VTable), the sequence  generated here will include that dereference
    // and any null checks necessary for it.
    // Additionally, the NeedsNullCheck flag will be set if the ensuing call
    // sequence needs to explicitly null-check the target method's instance
    // argument (e.g. because the callvirt opcode was used to call a non-virtual
    // method).  The NeedsNullCheck flag will not be set if the sequence
    // generated here already dereferences (and therefore null-checks) the
    // target
    // method's instance argument (e.g. if callvirt was used but the method is
    // virtual and lookup is performed via the target's VTable).
    switch (CallInfo->kind) {
    case CORINFO_CALL:
      // Direct Call
      CallTargetData->NeedsNullCheck = CallInfo->nullInstanceCheck == TRUE;
      Target = rdrGetDirectCallTarget(CallTargetData);
      break;
    case CORINFO_CALL_CODE_POINTER:
      // Runtime lookup required (code sharing w/o using inst param)
      CallTargetData->NeedsNullCheck = CallInfo->nullInstanceCheck == TRUE;
      Target = rdrGetCodePointerLookupCallTarget(CallTargetData);
      break;
    case CORINFO_VIRTUALCALL_STUB:
      // Virtual Call via virtual dispatch stub
      CallTargetData->NeedsNullCheck = true;
      Target = rdrGetVirtualStubCallTarget(CallTargetData);
      break;
    case CORINFO_VIRTUALCALL_LDVIRTFTN:
      // Virtual Call via indirect virtual call
      Target = rdrGetIndirectVirtualCallTarget(CallTargetData, ThisPtr);
      break;
    case CORINFO_VIRTUALCALL_VTABLE:
      // Virtual call via table lookup (vtable)
      Target = rdrGetVirtualTableCallTarget(CallTargetData, ThisPtr);
      break;
    default:
      ASSERTMNR(UNREACHED, "Unexpected call kind");
      Target = nullptr;
    }
  }

  CallTargetData->CallTargetNode = Target;
}

IRNode *
ReaderBase::rdrMakeLdFtnTargetNode(CORINFO_RESOLVED_TOKEN *ResolvedToken,
                                   CORINFO_CALL_INFO *CallInfo) {
  bool Unused = false;

  switch (CallInfo->kind) {
  case CORINFO_CALL:
    // Direct Call
    return rdrGetDirectCallTarget(CallInfo->hMethod, ResolvedToken->token,
                                  CallInfo->codePointerLookup,
                                  CallInfo->nullInstanceCheck == TRUE, true);
  case CORINFO_CALL_CODE_POINTER:
    // Runtime lookup required (code sharing w/o using inst param)
    return rdrGetCodePointerLookupCallTarget(CallInfo, Unused);

  default:
    ASSERTNR("Bad CORINFO_CALL_KIND for ldftn");
    return nullptr;
  }
}

IRNode *
ReaderBase::rdrGetDirectCallTarget(ReaderCallTargetData *CallTargetData) {
  return rdrGetDirectCallTarget(
      CallTargetData->getMethodHandle(), CallTargetData->getMethodToken(),
      CallTargetData->CallInfo.codePointerLookup,
      CallTargetData->NeedsNullCheck, canMakeDirectCall(CallTargetData));
}

// Generate the target for a direct call. "Direct call" can either be
// true direct calls if the runtime allows, or they can be indirect
// calls through the method descriptor.
IRNode *ReaderBase::rdrGetDirectCallTarget(CORINFO_METHOD_HANDLE Method,
                                           mdToken MethodToken,
                                           CORINFO_LOOKUP CodePointerLookup,
                                           bool NeedsNullCheck,
                                           bool CanMakeDirectCall) {
  InfoAccessType AccessType;
  void *Address;
  if (Flags & CORJIT_FLG_READYTORUN) {
    AccessType = CodePointerLookup.constLookup.accessType;
    Address = CodePointerLookup.constLookup.addr;
    assert(Address != nullptr);
  } else {
    // Ask for the generic "ANY" entry point. If NeedsNullCheck is true,
    // we could instead ask for the CORINFO_ACCESS_NONNULL entry,
    // but then we'd have to generalize the key used to look things
    // up in the HandleToGlobalObjectMap.
    CORINFO_CONST_LOOKUP AddressInfo;
    getFunctionEntryPoint(Method, &AddressInfo, CORINFO_ACCESS_ANY);
    AccessType = AddressInfo.accessType;
    Address = AddressInfo.addr;
  }

  IRNode *TargetNode;
  if ((AccessType == IAT_VALUE) && CanMakeDirectCall) {
    TargetNode = makeDirectCallTargetNode(Method, MethodToken, Address);
  } else {
    bool IsIndirect = AccessType != IAT_VALUE;
    const bool IsReadOnly = false;
    const bool IsRelocatable = true;
    TargetNode = handleToIRNode(MethodToken, Address, 0, IsIndirect, IsReadOnly,
                                IsRelocatable, IsIndirect);

    // TODO: call to same constant dominates this load then the load is
    // invariant
    if (AccessType == IAT_PPVALUE) {
      TargetNode = derefAddressNonNull(TargetNode, false, true);
    }
  }

  return TargetNode;
}

// Generate a runtime lookup for the call target. This should only be
// done when calling a shared method. This shared entry point actually
// invokes an IL stub that will push the correct type arg and then
// call the actual target.
//
// This lookup is avoided if the JIT requests the true entry point. In
// that case the runtime will return the true entry point aperrnd
// expect the JIT to call it with the typeArg parameter directly.
IRNode *ReaderBase::rdrGetCodePointerLookupCallTarget(
    ReaderCallTargetData *CallTargetData) {
  // These calls always follow a uniform calling convention
  ASSERTNR(!CallTargetData->getSigInfo()->hasTypeArg());
  ASSERTNR(!CallTargetData->getSigInfo()->isVarArg());

  return rdrGetCodePointerLookupCallTarget(CallTargetData->getCallInfo(),
                                           CallTargetData->IsIndirect);
}

IRNode *
ReaderBase::rdrGetCodePointerLookupCallTarget(CORINFO_CALL_INFO *CallInfo,
                                              bool &IsIndirect) {
  // The EE has asked us to call by computing a code pointer and then
  // doing an indirect call. This is because a runtime lookup is
  // required to get the code entry point.
  ASSERTNR(CallInfo);

  if (CallInfo == nullptr) {
    return nullptr;
  }

  ASSERTNR(CallInfo->codePointerLookup.lookupKind.needsRuntimeLookup);

  // treat as indirect call
  IsIndirect = true;

  return runtimeLookupToNode(
      CallInfo->codePointerLookup.lookupKind.runtimeLookupKind,
      &CallInfo->codePointerLookup.runtimeLookup);
}

// This is basically a runtime look up for virtual calls. A JIT helper
// call is invoked at runtime which finds the virtual call target. The
// return value of this helper call is then called indirectly.
IRNode *ReaderBase::rdrGetIndirectVirtualCallTarget(
    ReaderCallTargetData *CallTargetData, IRNode **ThisPtr) {
  if (Flags & CORJIT_FLG_READYTORUN) {
    return getReadyToRunVirtFuncPtr(*ThisPtr, &CallTargetData->ResolvedToken,
                                    &CallTargetData->CallInfo);
  } else {
    IRNode *ClassHandle = CallTargetData->getClassHandleNode();
    IRNode *MethodHandle = CallTargetData->getMethodHandleNode();

    ASSERTMNR(!CallTargetData->getSigInfo()->isVarArg(),
              "varargs + generics is not supported\n");
    ASSERTNR(ThisPtr); // ensure we have a this pointer
    ASSERTNR(ClassHandle);
    ASSERTNR(MethodHandle);

    // treat as indirect call
    CallTargetData->IsIndirect = true;

    // We need to make a copy because the "this"
    // pointer will be used twice:
    //     1) to look up the virtual function
    //     2) to call the method itself
    IRNode *ThisPtrCopy;
    dup(*ThisPtr, &ThisPtrCopy, ThisPtr);

    // Get the address of the target function by calling helper.
    // Type it as a native int, it will be recast later.
    IRNode *Dst = loadConstantI(0);
    const bool MayThrow = true;
    return callHelper(CORINFO_HELP_VIRTUAL_FUNC_PTR, MayThrow, Dst, ThisPtrCopy,
                      ClassHandle, MethodHandle);
  }
}

// Generate the target for a virtual stub dispatch call. This is the
// normal path for a virtual call.
IRNode *
ReaderBase::rdrGetVirtualStubCallTarget(ReaderCallTargetData *CallTargetData) {
  CORINFO_CALL_INFO *CallInfo = CallTargetData->getCallInfo();
  ASSERTNR(CallInfo);

  if (CallInfo == nullptr) {
    return nullptr;
  }

  IRNode *IndirectionCell, *IndirectionCellCopy;
  if (CallInfo->stubLookup.lookupKind.needsRuntimeLookup) {
    IndirectionCell =
        runtimeLookupToNode(CallInfo->stubLookup.lookupKind.runtimeLookupKind,
                            &CallInfo->stubLookup.runtimeLookup);
  } else {
    ASSERTNR(CallInfo->stubLookup.constLookup.accessType == IAT_PVALUE);
    IndirectionCell = handleToIRNode(CallTargetData->getMethodToken(),
                                     CallInfo->stubLookup.constLookup.addr, 0,
                                     false, false, true, false);
  }

  // For Stub Dispatch we need to pass the address of the indirection
  // cell as a secret param
  dup(IndirectionCell, &IndirectionCellCopy, &IndirectionCell);
  CallTargetData->setIndirectionCellNode(IndirectionCellCopy);

  // One indrection leads to the target
  return derefAddressNonNull(IndirectionCell, false, true);
}

// Generate the target for a virtual call that will use the virtual
// call table.
IRNode *
ReaderBase::rdrGetVirtualTableCallTarget(ReaderCallTargetData *CallTargetData,
                                         IRNode **ThisPtr) {
  // We need to make a copy because the "this" pointer
  IRNode *ThisPtrCopy;
  dup(*ThisPtr, &ThisPtrCopy, ThisPtr);
  IRNode *VTableAddress = derefAddress(ThisPtrCopy, true, true);

  // Get the VTable offset of the method.
  uint32_t OffsetOfIndirection;
  uint32_t OffsetAfterIndirection;
  getMethodVTableOffset(CallTargetData->getMethodHandle(), &OffsetOfIndirection,
                        &OffsetAfterIndirection);

  // Get the appropriate VTable chunk
  IRNode *OffsetNode = loadConstantI4(OffsetOfIndirection);
  IRNode *IndirectionSlot =
      binaryOp(ReaderBaseNS::Add, VTableAddress, OffsetNode);
  IRNode *VTableChunkAddress =
      derefAddressNonNull(IndirectionSlot, false, true);

  // Return the appropriate VTable slot
  OffsetNode = loadConstantI4(OffsetAfterIndirection);
  IRNode *VTableSlot =
      binaryOp(ReaderBaseNS::Add, VTableChunkAddress, OffsetNode);
  return derefAddressNonNull(VTableSlot, false, true);
}

// Generate the target for a delegate invoke.
IRNode *
ReaderBase::rdrGetDelegateInvokeTarget(ReaderCallTargetData *CallTargetData,
                                       IRNode **ThisPtr) {
  ASSERTNR(CallTargetData->hasThis());

  IRNode *ThisPtrCopy, *AddressNode;
  dup(*ThisPtr, &ThisPtrCopy, ThisPtr);

  CORINFO_EE_INFO EEInfo;
  JitInfo->getEEInfo(&EEInfo);
  uint32_t Instance = EEInfo.offsetOfDelegateInstance;
  uint32_t TargetPointerValue = EEInfo.offsetOfDelegateFirstTarget;
  IRNode *InstanceNode = loadConstantI4(Instance);
  IRNode *TargetPointerValueNode = loadConstantI4(TargetPointerValue);

  // Create a new this pointer
  AddressNode = binaryOp(ReaderBaseNS::Add, *ThisPtr, InstanceNode);
  *ThisPtr = derefAddress(AddressNode, true, false);

  // Locate the call target
  AddressNode =
      binaryOp(ReaderBaseNS::Add, ThisPtrCopy, TargetPointerValueNode);
  return derefAddress(AddressNode, false, true);
}

bool ReaderBase::rdrCallIsDelegateConstruct(
    ReaderCallTargetData *CallTargetData) {
  if (CallTargetData->isNewObj()) {
    uint32_t ClassAttribs = CallTargetData->getClassAttribs();
    uint32_t MethodAttribs = CallTargetData->getMethodAttribs();

    if (((ClassAttribs & CORINFO_FLG_DELEGATE) != 0) &&
        ((MethodAttribs & CORINFO_FLG_CONSTRUCTOR) != 0)) {
      ASSERTNR(!(MethodAttribs & CORINFO_FLG_STATIC)); // Implied by NewObj
      return true;
    }
  }
  return false;
}

#ifdef FEATURE_CORECLR

void ReaderBase::rdrInsertCalloutForDelegate(CORINFO_CLASS_HANDLE DelegateType,
                                             CORINFO_METHOD_HANDLE CalleeMethod,
                                             mdToken MethodToken) {
  if (!JitInfo->isDelegateCreationAllowed(DelegateType, CalleeMethod)) {
    IRNode *Arg1, *Arg2;
    bool IsIndirect;

    CORINFO_CLASS_HANDLE DelegateTypeHandle =
        embedClassHandle(DelegateType, &IsIndirect);
    Arg1 = handleToIRNode(MethodToken, DelegateTypeHandle, DelegateType,
                          IsIndirect, IsIndirect, true, false);

    CORINFO_METHOD_HANDLE CalleeMethodHandle =
        embedMethodHandle(CalleeMethod, &IsIndirect);
    Arg2 = handleToIRNode(MethodToken, CalleeMethodHandle, CalleeMethod,
                          IsIndirect, IsIndirect, true, false);

    // Make the helper call
    const bool MayThrow = true;
    callHelper(CORINFO_HELP_DELEGATE_SECURITY_CHECK, MayThrow, nullptr, Arg1,
               Arg2);
  }
}

#endif // FEATURE_CORECLR

void ReaderBase::clearStack() {
  while (!ReaderOperandStack->empty()) {
    pop(ReaderOperandStack->pop());
  }
}

// Iterator for reading types from arg type list.Used by
// buildUpAutos to gather types.  Used by inliner to ensure that
// inlinee types are available.
CORINFO_ARG_LIST_HANDLE
ReaderBase::argListNext(CORINFO_ARG_LIST_HANDLE ArgListHandle,
                        CORINFO_SIG_INFO *Sig,
                        CorInfoType *CorType,        // default to nullptr
                        CORINFO_CLASS_HANDLE *Class, // default to nullptr
                        bool *IsPinned               // default to nullptr
                        ) {
  CORINFO_CLASS_HANDLE TheClass = nullptr, ArgType;
  CORINFO_ARG_LIST_HANDLE NextArg = JitInfo->getArgNext(ArgListHandle);
  CorInfoTypeWithMod TheCorTypeWithMod =
      JitInfo->getArgType(Sig, ArgListHandle, &ArgType);
  CorInfoType TheCorType = strip(TheCorTypeWithMod);

  if ((TheCorType == CORINFO_TYPE_CLASS) ||
      (TheCorType == CORINFO_TYPE_VALUECLASS) ||
      (TheCorType == CORINFO_TYPE_BYREF) || (TheCorType == CORINFO_TYPE_PTR)) {
    TheClass = JitInfo->getArgClass(Sig, ArgListHandle);
  } else if (TheCorType == CORINFO_TYPE_REFANY) {
    TheClass = JitInfo->getBuiltinClass(CLASSID_TYPED_BYREF);
  }

  if (CorType != nullptr)
    *CorType = TheCorType;
  if (Class != nullptr)
    *Class = TheClass;
  if (IsPinned != nullptr)
    *IsPinned = ((TheCorTypeWithMod & CORINFO_TYPE_MOD_PINNED) != 0);

  return NextArg;
}

void ReaderBase::initMethodInfo(bool HasSecretParameter,
                                ReaderMethodSignature &Signature,
                                uint32_t &NumAutos) {
  // Get the number of arguments to this method.
  CORINFO_SIG_INFO &SigInfo = MethodInfo->args;
  uint32_t NormalArgs = (uint32_t)SigInfo.numArgs;
  bool HasThis = SigInfo.hasThis();
  bool IsVarArg = SigInfo.isVarArg();
  bool HasTypeArg = SigInfo.hasTypeArg();
  uint32_t NumArgs = (HasSecretParameter ? 1 : 0) + (HasThis ? 1 : 0) +
                     (IsVarArg ? 1 : 0) + (HasTypeArg ? 1 : 0) + NormalArgs;

  // Get the number of autos for this method.
  NumAutos = MethodInfo->locals.numArgs;

  // Init verification maps
  if (VerificationNeeded) {
    NumVerifyParams = NumArgs;
    if (NumArgs > 0) {
      ParamVerifyMap = (TypeInfo *)getTempMemory(NumArgs * sizeof(LocalDescr));
    }

    NumVerifyAutos = NumAutos;
    if (NumAutos > 0) {
      AutoVerifyMap = (TypeInfo *)getTempMemory(NumAutos * sizeof(LocalDescr));
    }
  } else {
    NumVerifyParams = 0;
    NumVerifyAutos = 0;
  }

  // Set signature flags, calling convention, and return type.
  Signature.HasThis = HasThis;
  Signature.IsVarArg = IsVarArg;
  Signature.HasTypeArg = HasTypeArg;
  Signature.HasSecretParameter = HasSecretParameter;
  Signature.CallingConvention = SigInfo.getCallConv();
  Signature.ResultType = {SigInfo.retType, SigInfo.retTypeClass};

  // Build up parameter info
  std::vector<CallArgType> &ArgumentTypes = Signature.ArgumentTypes;
  ArgumentTypes.resize(NumArgs);

  uint32_t ParamIndex = 0;

  if (HasSecretParameter) {
    assert(ParamIndex == Signature.getSecretParameterIndex());

    const CORINFO_CLASS_HANDLE Class = nullptr;
    ArgumentTypes[ParamIndex++] = {CORINFO_TYPE_NATIVEINT, Class};
  }

  // If the method has a this parameter, we must synthesize its type.
  if (HasThis) {
    assert(ParamIndex == Signature.getThisIndex());

    // Use the current method's containing class as the type of the "this"
    // argument.
    CORINFO_CLASS_HANDLE Class = getCurrentMethodClass();

    // See if the current class is an valueclass
    uint32_t Attribs = getClassAttribs(Class);

    bool IsValClass;
    CorInfoType CorType;
    if ((Attribs & CORINFO_FLG_VALUECLASS) == 0) {
      IsValClass = false;
      CorType = CORINFO_TYPE_CLASS;
    } else {
      IsValClass = true;
      CorType = CORINFO_TYPE_VALUECLASS;
    }

    if (VerificationNeeded) {
      verifyRecordParamType(ParamIndex, CorType, Class, IsValClass, true);
    }

    ArgumentTypes[ParamIndex++] = {IsValClass ? CORINFO_TYPE_BYREF : CorType,
                                   Class};
  }

  // For varargs, we have to synthesize the varargs cookie.  This
  // comes after the this pointer (if any) and before any fixed
  // params.
  if (IsVarArg) {
    assert(ParamIndex == Signature.getVarArgIndex());

    // this is not a real arg.  we do not record it for verification
    CORINFO_CLASS_HANDLE Class =
        getBuiltinClass(CorInfoClassId::CLASSID_ARGUMENT_HANDLE);
    ArgumentTypes[ParamIndex++] = {CORINFO_TYPE_PTR, Class};
  }

  // GENERICS: Code Sharing: After varargs, before fixed params
  // comes instParam (typeArg cookie)
  if (HasTypeArg) {
    assert(ParamIndex == Signature.getTypeArgIndex());

    // this is not a real arg.  we do not record it for verification
    const CORINFO_CLASS_HANDLE Class = nullptr;
    ArgumentTypes[ParamIndex++] = {CORINFO_TYPE_NATIVEINT, Class};
  }

  assert(ParamIndex == Signature.getNormalParamStart());
  uint32_t NormalParamEnd = Signature.getNormalParamEnd();

  // Get the types of all of the parameters.
  CORINFO_ARG_LIST_HANDLE Args = SigInfo.args;
  for (; ParamIndex < NormalParamEnd; ParamIndex++) {

    if (VerificationNeeded) {
      verifyRecordParamType(Signature.getILArgForArgIndex(ParamIndex), &SigInfo,
                            Args);
    }

    CallArgType &Arg = ArgumentTypes[ParamIndex];
    Args = argListNext(Args, &SigInfo, &Arg.CorType, &Arg.Class);
  }
}

void ReaderBase::initParamsAndAutos(const ReaderMethodSignature &Signature) {
  buildUpParams(Signature);
  buildUpAutos();
}

void ReaderBase::buildUpAutos() {
  // Get the types of all of the automatics.
  CORINFO_SIG_INFO &LocalInfo = MethodInfo->locals;
  CORINFO_ARG_LIST_HANDLE Locs = LocalInfo.args;
  for (UINT I = 0; I < LocalInfo.numArgs; I++) {
    // don't do anything until we've verified the local
    if (VerificationNeeded) {
      verifyRecordLocalType(I, &LocalInfo, Locs);
    }

    CorInfoType CorType;
    CORINFO_CLASS_HANDLE Class;
    bool IsPinned;
    Locs = argListNext(Locs, &LocalInfo, &CorType, &Class, &IsPinned);
    createSym(I, true, CorType, Class, IsPinned);
  }
}

void ReaderBase::buildUpParams(const ReaderMethodSignature &Signature) {
  const std::vector<CallArgType> &Args = Signature.getArgumentTypes();

  if (Signature.hasSecretParameter()) {
    const uint32_t I = Signature.getSecretParameterIndex();
    const CallArgType &Arg = Args[I];
    createSym(I, false, Arg.CorType, Arg.Class, false, Reader_SecretParam);
  }

  if (Signature.hasThis()) {
    const uint32_t I = Signature.getThisIndex();
    const CallArgType &Arg = Args[I];
    createSym(I, false, Arg.CorType, Arg.Class, false, Reader_ThisPtr);
  }

  if (Signature.isVarArg()) {
    const uint32_t I = Signature.getVarArgIndex();
    const CallArgType &Arg = Args[I];
    createSym(I, false, Arg.CorType, Arg.Class, false, Reader_VarArgsToken);
  }

  if (Signature.hasTypeArg()) {
    const uint32_t I = Signature.getTypeArgIndex();
    const CallArgType &Arg = Args[I];
    createSym(I, false, Arg.CorType, Arg.Class, false, Reader_InstParam);
  }

  for (uint32_t I = Signature.getNormalParamStart(),
                E = Signature.getNormalParamEnd();
       I != E; ++I) {
    const CallArgType &Arg = Args[I];
    createSym(I, false, Arg.CorType, Arg.Class, false);
  }
}

// =================================================================
// DOM_INFO - method of recording information about blocks
// =================================================================

// Block data cache, structure used to cache data discovered during
// block processing.  In general, this data can only be used if the
// previously processed block dominates the current block.
//
// Currently used to store the following information.
// 1) Did this block, or one of its dominators generate a call to
// obtain the shared static base of this class instance? If so return
// the operand that holds the shared static base (this can create long
// operand lifetimes).
// 2) Did this block, or one of its dominators already init a particular class?
// 3) DO we already have a pointer to the ThreadControlBlock (TCB)
//
// Since both of these opportunites are both rare, the data is stored
// in an unsorted list. If more common information is to be cached
// here then an alternative datastructure will need to be used.
//
// Note shared-static hashing needs to be done using
// (HelperID,moduleID,classID); hashing solely on the typeref is
// incorrect.
//
struct FgData {

  // Simple unsorted list for caching sparse information.
  struct FgDataListHash {

    struct HashListNode {
      CorInfoHelpFunc Key1;      // For (1), this is the HelperID;
                                 // for (2) & (3) this is unused
      CORINFO_CLASS_HANDLE Key2; // For (1) & (2), this is the ClassHandle; for
                                 // (3) this is unused
      void *Data;
      HashListNode *Next;
    };

    HashListNode *ListBase;

    void init(void) { ListBase = nullptr; }

    void insert(ReaderBase *Reader, CorInfoHelpFunc Key1,
                CORINFO_CLASS_HANDLE Key2, void *Data) {
      HashListNode *NewNode;

      NewNode = (HashListNode *)Reader->getTempMemory(sizeof(HashListNode));
      NewNode->Key1 = Key1;
      NewNode->Key2 = Key2;
      NewNode->Data = Data;
      NewNode->Next = ListBase;
      ListBase = NewNode;
    }

    void *get(CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2) {
      HashListNode *Node;

      Node = ListBase;
      while (Node != nullptr) {
        if ((Node->Key1 == Key1) && (Node->Key2 == Key2))
          return Node->Data;
        Node = Node->Next;
      }
      return nullptr;
    }
  };

  class FgDataArrayHash {
    const static int DataArrayHashSize = 63;

    FgDataListHash *Hash;

  public:
    void init(void) { Hash = nullptr; }

    void insert(ReaderBase *Reader, CorInfoHelpFunc Key1,
                CORINFO_CLASS_HANDLE Key2, void *Data) {
      if (Hash == nullptr) {
        Hash = (FgDataListHash *)Reader->getTempMemory(sizeof(FgDataListHash) *
                                                       DataArrayHashSize);
      }
      Hash[Key1 % DataArrayHashSize].insert(Reader, Key1, Key2, Data);
    }

    void *get(CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2) {
      if (Hash == nullptr) {
        return nullptr;
      }
      return Hash[Key1 % DataArrayHashSize].get(Key1, Key2);
    }
  };

  FgDataListHash StaticBaseHash, ClassInitHash;

  // Init routine, since this structure will be allocated into a pool.
  // This init is not strictly necessary since the lower structures
  // only want to be initialized to zero.
  void init(void) {
    StaticBaseHash.init();
    ClassInitHash.init();
  }

  // Getters and setters for properties tracked in FgData.
  //
  // Getters are used via function pointer so must have prototype
  // void* f(Token)
  void *getSharedStaticBase(CorInfoHelpFunc Key1HelperID,
                            CORINFO_CLASS_HANDLE Key2ClassHandle,
                            bool *Key3NoCtor) {
    void *RetVal = StaticBaseHash.get(Key1HelperID, Key2ClassHandle);
    if (RetVal != nullptr)
      return RetVal;

    // We didn't find the getter we're looking for, so try some
    // alternatives and gather extra useful information.
    // Specifically, key4_typeRef is set to 0 if we can determine that
    // the class .cctor has already been run.
    CorInfoHelpFunc OtherHelperID;
    switch (Key1HelperID) {
    case CORINFO_HELP_GETSHARED_GCSTATIC_BASE:
      OtherHelperID = CORINFO_HELP_GETSHARED_GCSTATIC_BASE_NOCTOR;
      break;
    case CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE:
      OtherHelperID = CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR;
      break;
    case CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE:
      OtherHelperID = CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR;
      break;
    case CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE:
      OtherHelperID = CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE_NOCTOR;
      break;

    case CORINFO_HELP_GETSHARED_GCSTATIC_BASE_NOCTOR:
    case CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR:
      // The runtime only gives us these helpers if there is no
      // .cctor to run ever.  Thus we don't need to bother looking
      // for the other variant, but we do know that the .cctor never
      // needs to be run.
      *Key3NoCtor = true;
      return nullptr;

    default:
      // If we're still not sure about whether the .cctor has run, check
      // before returning nullptr (because there are no other equivalent
      // helpers).
      if (!*Key3NoCtor &&
          ClassInitHash.get(CorInfoHelpFunc::CORINFO_HELP_UNDEF,
                            Key2ClassHandle)) {
        *Key3NoCtor = true;
      }
      return nullptr;
    }

    // We have an equivalent helper, look it up to see if it exists.
    RetVal = StaticBaseHash.get(OtherHelperID, Key2ClassHandle);

    // If we found a NoCtor variant or this class has already had it's
    // .cctor run, then we know the .cctor will not be run again.
    if (RetVal != nullptr ||
        (!*Key3NoCtor &&
         ClassInitHash.get(CorInfoHelpFunc::CORINFO_HELP_UNDEF,
                           Key2ClassHandle))) {
      *(bool *)Key3NoCtor = true;
    }
    return RetVal;
  }

  void *getClassInit(CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2ClassHandle,
                     bool *Key3) {
    ASSERTNR(Key1 == 0);
    ASSERTNR(Key2ClassHandle != nullptr);
    ASSERTNR(Key3 == nullptr);
    return (ClassInitHash.get(CorInfoHelpFunc::CORINFO_HELP_UNDEF,
                              Key2ClassHandle));
  }

  void setSharedStaticBase(ReaderBase *Reader, CorInfoHelpFunc HelperID,
                           CORINFO_CLASS_HANDLE ClassHandle,
                           IRNode *BasePointer) {
    StaticBaseHash.insert(Reader, HelperID, ClassHandle, BasePointer);
  }

  void setClassInit(ReaderBase *Reader, CORINFO_CLASS_HANDLE ClassHandle) {
    ClassInitHash.insert(Reader, CorInfoHelpFunc::CORINFO_HELP_UNDEF,
                         ClassHandle, (void *)1);
  }
};

void ReaderBase::initBlockArray(uint32_t BlockCount) {
  BlockArray = (FgData **)getTempMemory(BlockCount * sizeof(void *));
}

// Shared routine obtains existing block data for block,
// If DoCreate is true then create block data if it isn't present.
FgData *ReaderBase::domInfoGetBlockData(FlowGraphNode *Fg, bool DoCreate) {
  FgData *TheFgData;
  uint32_t BlockNum;

  if (BlockArray == nullptr)
    return nullptr;

  BlockNum = fgNodeGetBlockNum(Fg);
  ASSERTNR(BlockNum != (uint32_t)-1);

  TheFgData = BlockArray[BlockNum];
  if (!TheFgData && DoCreate) {
    TheFgData = (FgData *)getTempMemory(sizeof(FgData));
    BlockArray[BlockNum] = TheFgData;
  }

  return TheFgData;
}

// Shared routine checks current block and all dominators for first
// non-null response from FgData method.
void *ReaderBase::domInfoGetInfoFromDominator(
    FlowGraphNode *Fg, CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2,
    bool *Key3, bool RequireSameRegion,
    void *(FgData::*Pmfn)(CorInfoHelpFunc Key1, CORINFO_CLASS_HANDLE Key2,
                          bool *Key3)) {
  FgData *FgData;
  void *RetVal;
  FlowGraphNode *FgCurrent;

  RetVal = nullptr;
  FgCurrent = Fg;

  do {
    // We don't want to keep the temp live accross region boundaries,
    // so if this dominator is not the same region as the block in
    // question, we won't consider it a candidate.  WHY? Some
    // compilers can't tolerate temporaries that live across EH
    // boundaries.
    if (!(RequireSameRegion &&
          fgNodeGetRegion(Fg) != fgNodeGetRegion(FgCurrent))) {
      // if we get an operand back then we have succeeded,
      if ((FgData = domInfoGetBlockData(FgCurrent, false)) &&
          (RetVal = (FgData->*Pmfn)(Key1, Key2, Key3)))
        break;
    }
    FgCurrent = fgNodeGetIDom(FgCurrent);
  } while (FgCurrent);

  return RetVal;
}

// Returns node that holds previously calculated shared static base
// address, otherwise nullptr.
IRNode *ReaderBase::domInfoDominatorDefinesSharedStaticBase(
    FlowGraphNode *Fg, CorInfoHelpFunc &HelperID,
    CORINFO_CLASS_HANDLE ClassHandle, bool *NoCtor) {
  IRNode *RetVal;
  *NoCtor = false;

  if (generateDebugCode())
    return nullptr;

  RetVal = (IRNode *)domInfoGetInfoFromDominator(
      Fg, HelperID, ClassHandle, NoCtor, true, &FgData::getSharedStaticBase);
  if (*NoCtor) {
    if (RetVal == nullptr) {
      switch (HelperID) {
      case CORINFO_HELP_GETSHARED_GCSTATIC_BASE:
        HelperID = CORINFO_HELP_GETSHARED_GCSTATIC_BASE_NOCTOR;
        break;
      case CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE:
        HelperID = CORINFO_HELP_GETSHARED_NONGCSTATIC_BASE_NOCTOR;
        break;
      case CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE:
        HelperID = CORINFO_HELP_GETSHARED_GCTHREADSTATIC_BASE_NOCTOR;
        break;
      case CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE:
        HelperID = CORINFO_HELP_GETSHARED_NONGCTHREADSTATIC_BASE_NOCTOR;
        break;
      default:
        break;
      }
    }
  }
  return RetVal;
}

// DomInfo getters/setters

// Records that fg has calculated shared static base.
void ReaderBase::domInfoRecordSharedStaticBaseDefine(
    FlowGraphNode *Fg, CorInfoHelpFunc HelperID,
    CORINFO_CLASS_HANDLE ClassHandle, IRNode *BasePointer) {
  if (generateDebugCode())
    return;

  FgData *FgData = domInfoGetBlockData(Fg, true);
  if (FgData != nullptr) {
    FgData->setSharedStaticBase(this, HelperID, ClassHandle, BasePointer);
  }
}

// Returns whether particular class has already been initialized
// by current block, or any of its dominators.
bool ReaderBase::domInfoDominatorHasClassInit(
    FlowGraphNode *Fg, CORINFO_CLASS_HANDLE ClassHandle) {
  if (generateDebugCode())
    return false;

  bool RetVal;

  RetVal = (domInfoGetInfoFromDominator(Fg, CorInfoHelpFunc::CORINFO_HELP_UNDEF,
                                        ClassHandle, nullptr, false,
                                        &FgData::getClassInit) != nullptr);
  return RetVal;
}

// Records that current block has initialized class typeRef.
void ReaderBase::domInfoRecordClassInit(FlowGraphNode *Fg,
                                        CORINFO_CLASS_HANDLE ClassHandle) {
  if (generateDebugCode())
    return;

  FgData *FgData = domInfoGetBlockData(Fg, true);
  if (FgData != nullptr) {
    FgData->setClassInit(this, ClassHandle);
  }
}

// =================================================================
// End DOMINFO
// =================================================================

// Default routine to insert verification throw.
void ReaderBase::insertThrow(CorInfoHelpFunc ThrowHelper, uint32_t Offset) {
  IRNode *IntConstant = loadConstantI4(Offset);
  const bool MayThrow = true;
  callHelper(ThrowHelper, MayThrow, nullptr, IntConstant);
}

// Macro used by main reader loop for distinguishing verify-only passes
#define BREAK_ON_VERIFY_ONLY                                                   \
  if (Param->IsVerifyOnly || Param->LocalFault)                                \
  break

LONG objectFilter(PEXCEPTION_POINTERS ExceptionPointersPtr, void *Param) {
  ReadBytesForFlowGraphNodeHelperParam *ReadParam =
      (ReadBytesForFlowGraphNodeHelperParam *)Param;

  if (ExceptionPointersPtr->ExceptionRecord->ExceptionCode ==
      LLILCJIT_READEREXCEPTION_CODE) {
    ReadParam->Excep =
        *(ReaderException **)
             ExceptionPointersPtr->ExceptionRecord->ExceptionInformation;
    return EXCEPTION_EXECUTE_HANDLER;
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

// The method/signature lookups for tail call detection must be
// wrapped in a try/filter simlar to inlining.  This is required to
// maintain the same exception ordering provided by the classic CLR
// JITs.
bool ReaderBase::isUnmarkedTailCall(uint8_t *ILInput, uint32_t ILInputSize,
                                    uint32_t NextOffset, mdToken Token) {
  struct Param : JITFilterParam {
    ReaderBase *This;
    uint8_t *ILInput;
    uint32_t ILInputSize;
    uint32_t NextOffset;
    mdToken Token;
    bool DoTailCallOpt;
  } TheParam;
  TheParam.JitInfo = JitInfo;
  TheParam.This = this;
  TheParam.ILInput = ILInput;
  TheParam.ILInputSize = ILInputSize;
  TheParam.NextOffset = NextOffset;
  TheParam.Token = Token;
  TheParam.DoTailCallOpt = false;

  PAL_TRY(Param *, Param, &TheParam) {
    Param->DoTailCallOpt = Param->This->isUnmarkedTailCallHelper(
        Param->ILInput, Param->ILInputSize, Param->NextOffset, Param->Token);
  }
  PAL_EXCEPT_FILTER(eeJITFilter) {
    TheParam.JitInfo->HandleException(&TheParam.ExceptionPointers);
    TheParam.DoTailCallOpt = false;
  }
  PAL_ENDTRY;

  return TheParam.DoTailCallOpt;
}

// Determine if the current instruction is an unmarked tail call.
// We check that:
//     1) The next instruction is a return
//     2) The return type from the current function matches the
//        return type of the called function.
//
// NOTE: Other necessary checks are performed later
bool ReaderBase::isUnmarkedTailCallHelper(uint8_t *ILInput,
                                          uint32_t ILInputSize, uint32_t Offset,
                                          mdToken Token) {
  // Get the next instruction (if any)
  uint8_t *UnusedOperand;
  uint32_t NumPops = 0;
  ReaderBaseNS::OPCODE Opcode;

  do {
    Offset = parseILOpcode(ILInput, Offset, ILInputSize, this, &Opcode,
                           &UnusedOperand, false);
  } while (Offset < ILInputSize &&
           ((Opcode == ReaderBaseNS::CEE_NOP) ||
            ((Opcode == ReaderBaseNS::CEE_POP) && (++NumPops == 1))));

  if (Opcode == ReaderBaseNS::CEE_RET && NumPops <= 1) {
    // Check the return types of the two functions
    CORINFO_RESOLVED_TOKEN ResolvedToken;
    resolveToken(Token, CORINFO_TOKENKIND_Method, &ResolvedToken);

    CORINFO_METHOD_HANDLE TargetMethod = ResolvedToken.hMethod;
    ASSERTNR(TargetMethod);
    CORINFO_METHOD_HANDLE CurrentMethod = getCurrentMethodHandle();
    ASSERTNR(CurrentMethod);

    CORINFO_SIG_INFO SigTarget;
    getMethodSig(TargetMethod, &SigTarget);
    CorInfoType RetTypeTarget = SigTarget.retType;
    CORINFO_CLASS_HANDLE RetClassTarget = SigTarget.retTypeClass;

    CORINFO_SIG_INFO SigCurrentMethod;
    getMethodSig(CurrentMethod, &SigCurrentMethod);
    CorInfoType RetTypeCurrent = SigCurrentMethod.retType;
    CORINFO_CLASS_HANDLE RetClassCurrent = SigCurrentMethod.retTypeClass;

#if !defined(TAMD64)
    // Bail if we're returning a value class
    // TODO: If the value class fits in the return reg,
    //         we could still do it.
    if (RetTypeTarget == CORINFO_TYPE_VALUECLASS ||
        RetTypeCurrent == CORINFO_TYPE_VALUECLASS) {

      return false;
    }
#endif // TAMD64

#if defined(TAMD64)

    // If the caller returns nothing and the callee returns something,
    // allow a single pop before the RET
    if (NumPops == 1) {
      if ((RetTypeCurrent != CORINFO_TYPE_VOID) ||
          (RetTypeTarget == CORINFO_TYPE_VOID)) {
        return false;
      }
    } else
#else // TAMD64

    if (NumPops > 0) {
      return false;
    }

#endif // TAMD64

    {

      // The return types must match
      // TODO: Loosen this up for scalar types
      //         Will depend on target - are representations compatible?
      if (RetTypeTarget != RetTypeCurrent ||
          RetClassTarget != RetClassCurrent) {

        return false;
      }
    }
    return true;
  }

  return false;
}

// Determine if the current instruction is an valid explicit tail
// call.  We check that the next instruction is a return, while
// allowing multiple nops, and a single pop iff the caller is void and
// the callee is non-void this prevents code from doing a tail call in
// the middle of a method.
bool ReaderBase::checkExplicitTailCall(uint32_t ILOffset, bool AllowPop) {
  // Get the next instruction (if any)
  const uint32_t ILInputSize = MethodInfo->ILCodeSize;
  uint8_t *ILInput = MethodInfo->ILCode;
  uint8_t *UnusedOperand;
  uint32_t Offset = ILOffset + SizeOfCEECall;
  ReaderBaseNS::OPCODE Opcode;

  do {
    Offset = parseILOpcode(ILInput, Offset, ILInputSize, this, &Opcode,
                           &UnusedOperand, false);
    if (AllowPop && (Opcode == ReaderBaseNS::CEE_POP)) {
      AllowPop = false;
      Opcode = ReaderBaseNS::CEE_NOP;
    }
  } while (Offset < ILInputSize && (Opcode == ReaderBaseNS::CEE_NOP));

  if (Opcode != ReaderBaseNS::CEE_RET) {
    BADCODE(MVER_E_TAIL_RET);
    return false;
  }
  return true;
}

// Main reader loop, called once for each reachable block.
void ReaderBase::readBytesForFlowGraphNodeHelper(
    ReadBytesForFlowGraphNodeHelperParam *Param) {

  FlowGraphNode *&Fg = Param->Fg;
  ReaderBaseNS::OPCODE Opcode = ReaderBaseNS::CEE_ILLEGAL;
  IRNode *Arg1;
  IRNode *Arg2;
  IRNode *Arg3;
  IRNode *ResultIR;
  uint8_t *ILInput = nullptr;
  uint32_t ILSize;
  uint32_t CurrentOffset = Param->CurrentOffset;
  uint32_t NextOffset;
  uint32_t TargetOffset;
  uint8_t *Operand;
  mdToken Token;
  ReaderAlignType AlignmentPrefix = Reader_AlignNatural;
  bool HasVolatilePrefix = false;
  bool HasTailCallPrefix = false;
  bool HasReadOnlyPrefix = false;
  bool HasConstrainedPrefix = false;
  mdToken ConstraintTypeRef = mdTokenNil;
  mdToken LoadFtnToken = mdTokenNil;

  VerificationState *&TheVerificationState = Param->VState;

  int MappedValue;

  TheVerificationState = verifyInitializeBlock(Fg, CurrentOffset);

  ILInput = MethodInfo->ILCode;
  ILSize = MethodInfo->ILCodeSize;
  NextOffset = CurrentOffset;
  LastLoadToken = mdTokenNil;

  while (CurrentOffset < fgNodeGetEndMSILOffset(Fg)) {
    CORINFO_RESOLVED_TOKEN ResolvedToken;

#if !defined(NODEBUG)
    memset(&ResolvedToken, 0xCC, sizeof(ResolvedToken));
#endif

    ReaderBaseNS::OPCODE PrevOp = Opcode;
    NextOffset =
        parseILOpcode(ILInput, CurrentOffset, ILSize, this, &Opcode, &Operand);
    CurrInstrOffset = CurrentOffset;
    NextInstrOffset = NextOffset;

    // Set debug locations
    if (ReaderOperandStack->empty()) {
      const bool IsCall = false;
      setDebugLocation(CurrentOffset, IsCall);
    }

    // If we have cached a LoadFtnToken from LDFTN or LDVIRTFTN
    // then clear it if the next opcode is not NEWOBJ
    if (Opcode != ReaderBaseNS::CEE_NEWOBJ) {
      LoadFtnToken = mdTokenNil;
    }

    // If we have cached a loadToken from LDTOKEN
    // then clear it if the next opcode is not CEE_CALL
    if (Opcode != ReaderBaseNS::CEE_CALL) {
      LastLoadToken = mdTokenNil;
    }

    VerInstrStartOffset = CurrentOffset;
    VerInstrOpcode = Opcode;

    // SEQUENCE POINTS
    if (!Param->IsVerifyOnly && needSequencePoints()) {
      sequencePoint(CurrentOffset, PrevOp);
    }

#if !defined(NODEBUG)
    opcodeDebugPrint(ILInput, CurrentOffset, NextOffset);
#endif

    MappedValue = OpcodeRemap[Opcode];

    // Switch on msil opcode
    switch (Opcode) {
    case ReaderBaseNS::CEE_ADD_OVF_UN:
    case ReaderBaseNS::CEE_ADD:
    case ReaderBaseNS::CEE_ADD_OVF:
    case ReaderBaseNS::CEE_AND:
    case ReaderBaseNS::CEE_DIV:
    case ReaderBaseNS::CEE_DIV_UN:
    case ReaderBaseNS::CEE_MUL:
    case ReaderBaseNS::CEE_MUL_OVF:
    case ReaderBaseNS::CEE_MUL_OVF_UN:
    case ReaderBaseNS::CEE_OR:
    case ReaderBaseNS::CEE_REM:
    case ReaderBaseNS::CEE_REM_UN:
    case ReaderBaseNS::CEE_SUB:
    case ReaderBaseNS::CEE_SUB_OVF:
    case ReaderBaseNS::CEE_SUB_OVF_UN:
    case ReaderBaseNS::CEE_XOR:
      verifyBinary(TheVerificationState, Opcode);
      BREAK_ON_VERIFY_ONLY;

      Arg2 = ReaderOperandStack->pop();
      Arg1 = ReaderOperandStack->pop();
      ResultIR = binaryOp((ReaderBaseNS::BinaryOpcode)MappedValue, Arg1, Arg2);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_ARGLIST:
      verifyArgList(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = argList();
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_BOX:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Box, &ResolvedToken);
      verifyBox(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the valuetype we need to box
      removeStackInterference();
      handleClassAccess(&ResolvedToken);
      ResultIR = box(&ResolvedToken, Arg1, &NextOffset, TheVerificationState);
      NextInstrOffset = NextOffset;
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_BEQ:
    case ReaderBaseNS::CEE_BGE:
    case ReaderBaseNS::CEE_BGE_UN:
    case ReaderBaseNS::CEE_BGT:
    case ReaderBaseNS::CEE_BGT_UN:
    case ReaderBaseNS::CEE_BLE:
    case ReaderBaseNS::CEE_BLE_UN:
    case ReaderBaseNS::CEE_BLT:
    case ReaderBaseNS::CEE_BLT_UN:
    case ReaderBaseNS::CEE_BNE_UN:
      TargetOffset = NextOffset + readValue<int32_t>(Operand);
      goto GEN_COND_BRANCH;

    case ReaderBaseNS::CEE_BEQ_S:
    case ReaderBaseNS::CEE_BGE_S:
    case ReaderBaseNS::CEE_BGE_UN_S:
    case ReaderBaseNS::CEE_BGT_S:
    case ReaderBaseNS::CEE_BGT_UN_S:
    case ReaderBaseNS::CEE_BLE_S:
    case ReaderBaseNS::CEE_BLE_UN_S:
    case ReaderBaseNS::CEE_BLT_S:
    case ReaderBaseNS::CEE_BLT_UN_S:
    case ReaderBaseNS::CEE_BNE_UN_S:
      TargetOffset = NextOffset + readValue<int8_t>(Operand);
      goto GEN_COND_BRANCH;

    GEN_COND_BRANCH:
      verifyCompare(TheVerificationState, Opcode);
      Param->VerifiedEndBlock = true;
      verifyFinishBlock(TheVerificationState,
                        Fg); // before MaintainOperandStack
      BREAK_ON_VERIFY_ONLY;

      Arg2 = ReaderOperandStack->pop();
      Arg1 = ReaderOperandStack->pop();

      if (!ReaderOperandStack->empty()) {
        maintainOperandStack(Fg);
        ReaderOperandStack->clearStack();
      }

      // First pass create the branch and target to label
      condBranch((ReaderBaseNS::CondBranchOpcode)MappedValue, Arg1, Arg2);
      break;

    case ReaderBaseNS::CEE_BRTRUE:
    case ReaderBaseNS::CEE_BRFALSE:
      TargetOffset = NextOffset + readValue<int32_t>(Operand);
      goto GEN_BOOL_BRANCH;

    case ReaderBaseNS::CEE_BRTRUE_S:
    case ReaderBaseNS::CEE_BRFALSE_S:
      TargetOffset = NextOffset + readValue<int8_t>(Operand);
      goto GEN_BOOL_BRANCH;

    GEN_BOOL_BRANCH:
      verifyBoolBranch(TheVerificationState, NextOffset, TargetOffset);
      Param->VerifiedEndBlock = true;
      verifyFinishBlock(TheVerificationState,
                        Fg); // before MaintainOperandStack
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      if (!ReaderOperandStack->empty()) {
        maintainOperandStack(Fg);
        ReaderOperandStack->clearStack();
      }
      boolBranch((ReaderBaseNS::BoolBranchOpcode)MappedValue, Arg1);
      break;

    case ReaderBaseNS::CEE_BR:
      TargetOffset = NextOffset + readValue<int32_t>(Operand);
      goto GEN_BRANCH;

    case ReaderBaseNS::CEE_BR_S:
      TargetOffset = NextOffset + readValue<int8_t>(Operand);
      goto GEN_BRANCH;

    GEN_BRANCH:
      // We need to record the source location of branches, so insert a nop
      nop();
      Param->HasFallThrough = false;
      Param->VerifiedEndBlock = true;
      verifyFinishBlock(TheVerificationState,
                        Fg); // before MaintainOperandStack
      BREAK_ON_VERIFY_ONLY;

      // Assumes first pass created branch label, here we just assist
      // any live stack operands across the block boundary.
      if (!ReaderOperandStack->empty()) {
        maintainOperandStack(Fg);
        ReaderOperandStack->clearStack();
      }
      branch();
      break;

    case ReaderBaseNS::CEE_BREAK:
      // CEE_BREAK is always verifiable
      BREAK_ON_VERIFY_ONLY;
      breakOpcode();
      break;

    case ReaderBaseNS::CEE_CALL:
    case ReaderBaseNS::CEE_CALLI:
    case ReaderBaseNS::CEE_CALLVIRT: {
      const bool IsCall = true;
      setDebugLocation(CurrentOffset, IsCall);
      bool IsUnmarkedTailCall = false;
      Token = readValue<mdToken>(Operand);

      verifyCall(TheVerificationState, Opcode, HasTailCallPrefix,
                 HasReadOnlyPrefix, HasConstrainedPrefix, ThisPtrModified,
                 ConstraintTypeRef, Token);
      BREAK_ON_VERIFY_ONLY;

      if (!HasTailCallPrefix && Opcode != ReaderBaseNS::CEE_CALLI &&
          doTailCallOpt() && isUnmarkedTailCall(ILInput, MethodInfo->ILCodeSize,
                                                NextOffset, Token)) {
        HasTailCallPrefix = true;
        IsUnmarkedTailCall = true;
      }

      ResultIR = call((ReaderBaseNS::CallOpcode)MappedValue, Token,
                      ConstraintTypeRef, mdTokenNil, HasReadOnlyPrefix,
                      HasTailCallPrefix, IsUnmarkedTailCall, CurrentOffset);
      if (ResultIR != nullptr) {
        ReaderOperandStack->push(ResultIR);
      }

      // Only good for one call
      LastLoadToken = mdTokenNil;
    } break;

    case ReaderBaseNS::CEE_CASTCLASS:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Casting, &ResolvedToken);
      verifyCastClass(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      handleClassAccess(&ResolvedToken);
      removeStackInterference();
      ResultIR = castClass(&ResolvedToken, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_ISINST:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Casting, &ResolvedToken);
      verifyIsInst(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      handleClassAccess(&ResolvedToken);
      removeStackInterference();
      ResultIR = isInst(&ResolvedToken, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_CEQ:
    case ReaderBaseNS::CEE_CGT:
    case ReaderBaseNS::CEE_CGT_UN:
    case ReaderBaseNS::CEE_CLT:
    case ReaderBaseNS::CEE_CLT_UN:
      verifyCompare(TheVerificationState, Opcode);
      BREAK_ON_VERIFY_ONLY;

      Arg2 = ReaderOperandStack->pop();
      Arg1 = ReaderOperandStack->pop();
      ResultIR = cmp((ReaderBaseNS::CmpOpcode)MappedValue, Arg1, Arg2);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_CKFINITE:
      // Add overflow check for top of stack
      verifyCkFinite(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      removeStackInterference();
      ResultIR = ckFinite(Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_CONV_I1:
    case ReaderBaseNS::CEE_CONV_I2:
    case ReaderBaseNS::CEE_CONV_I4:
    case ReaderBaseNS::CEE_CONV_I8:
    case ReaderBaseNS::CEE_CONV_R4:
    case ReaderBaseNS::CEE_CONV_R8:
    case ReaderBaseNS::CEE_CONV_U1:
    case ReaderBaseNS::CEE_CONV_U2:
    case ReaderBaseNS::CEE_CONV_U4:
    case ReaderBaseNS::CEE_CONV_U8:
    case ReaderBaseNS::CEE_CONV_I:
    case ReaderBaseNS::CEE_CONV_U:

    case ReaderBaseNS::CEE_CONV_OVF_I1:
    case ReaderBaseNS::CEE_CONV_OVF_I2:
    case ReaderBaseNS::CEE_CONV_OVF_I4:
    case ReaderBaseNS::CEE_CONV_OVF_I8:
    case ReaderBaseNS::CEE_CONV_OVF_U1:
    case ReaderBaseNS::CEE_CONV_OVF_U2:
    case ReaderBaseNS::CEE_CONV_OVF_U4:
    case ReaderBaseNS::CEE_CONV_OVF_U8:
    case ReaderBaseNS::CEE_CONV_OVF_I:
    case ReaderBaseNS::CEE_CONV_OVF_U:

    case ReaderBaseNS::CEE_CONV_OVF_I1_UN:
    case ReaderBaseNS::CEE_CONV_OVF_I2_UN:
    case ReaderBaseNS::CEE_CONV_OVF_I4_UN:
    case ReaderBaseNS::CEE_CONV_OVF_I8_UN:
    case ReaderBaseNS::CEE_CONV_OVF_U1_UN:
    case ReaderBaseNS::CEE_CONV_OVF_U2_UN:
    case ReaderBaseNS::CEE_CONV_OVF_U4_UN:
    case ReaderBaseNS::CEE_CONV_OVF_U8_UN:
    case ReaderBaseNS::CEE_CONV_OVF_I_UN:
    case ReaderBaseNS::CEE_CONV_OVF_U_UN:
    case ReaderBaseNS::CEE_CONV_R_UN:
      verifyConvert(TheVerificationState,
                    (ReaderBaseNS::ConvOpcode)MappedValue);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Operand to be converted
      ResultIR = conv((ReaderBaseNS::ConvOpcode)MappedValue, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_CPBLK:
      verifyFailure(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Copy the number of bytes to copy
      Arg2 = ReaderOperandStack->pop(); // Pop the source address
      Arg3 = ReaderOperandStack->pop(); // Pop the dest   address
      removeStackInterference();
      cpBlk(Arg1, Arg2, Arg3, AlignmentPrefix, HasVolatilePrefix);
      ResultIR = nullptr;
      break;

    case ReaderBaseNS::CEE_CPOBJ:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyCpObj(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Source object
      Arg2 = ReaderOperandStack->pop(); // Dest   object
      removeStackInterference();
      cpObj(&ResolvedToken, Arg1, Arg2, AlignmentPrefix, HasVolatilePrefix);
      break;

    case ReaderBaseNS::CEE_DUP: {
      IRNode *Result1, *Result2;

      verifyDup(TheVerificationState, ILInput + CurrentOffset);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      dup(Arg1, &Result1, &Result2);
      ReaderOperandStack->push(Result1);
      ReaderOperandStack->push(Result2);
    } break;

    case ReaderBaseNS::CEE_ENDFILTER:
      verifyEndFilter(TheVerificationState, NextOffset);
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      // The endfilter instruction needs special handling,
      // similar to return. It's source is the return value  of the filter,
      // which must be one of the following 32-bit values:
      //
      //    EXCEPTION_EXECUTE_HANDLER    (1)
      //    EXCEPTION_CONTINUE_SEARCH    (0)
      //    EXCEPTION_CONTINUE_EXECUTION (-1, not supported in CLR currently)
      Arg1 = ReaderOperandStack->pop(); // Pop the object pointer
      endFilter(Arg1);
      break;

    case ReaderBaseNS::CEE_ENDFINALLY:
      verifyEndFinally(TheVerificationState);
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      // Doesn't turn into anything,
      // but it's a block marker, so it needs to consume some bytes
      // However, this information should also been conveyed by
      // the EIT so we don't need to do anything.
      clearStack();
      break;

    case ReaderBaseNS::CEE_INITOBJ:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyInitObj(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop address of object
      removeStackInterference();
      initObj(&ResolvedToken, Arg1);
      break;

    case ReaderBaseNS::CEE_INITBLK:
      verifyFailure(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the number of bytes
      Arg2 = ReaderOperandStack->pop(); // Pop the value to assign to each byte
      Arg3 = ReaderOperandStack->pop(); // Pop the destination address
      removeStackInterference();
      initBlk(Arg1, Arg2, Arg3, AlignmentPrefix, HasVolatilePrefix);
      ResultIR = nullptr;
      break;

    case ReaderBaseNS::CEE_JMP: {
      CORINFO_METHOD_HANDLE Handle;
      CORINFO_SIG_INFO Sig, Sig2;
      bool HasThis;

      // check before verification otherwise we get different exceptions
      // badcode vs verification exception depending if verifier is on
      if (CurrentRegion != nullptr &&
          ReaderBaseNS::RGN_Root != rgnGetRegionType(CurrentRegion)) {

        BADCODE(MVER_E_TAILCALL_INSIDE_EH);
      }

      verifyFailure(TheVerificationState);
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Method, &ResolvedToken);

      // The stack must be empty when jmp is reached. If it is not
      // empty (and verification is off), then pop stack until it
      // is empty.
      clearStack();

      // Jmp needs to know about this and vararg params
      // so fetch the information.
      Handle = ResolvedToken.hMethod;
      getCallSiteSignature(Handle, Token, &Sig, &HasThis);
      // While we are at it, make sure that the jump prototype
      // matches this function's prototype, otherwise it makes
      // no sense to abandon frame and transfer control.
      JitInfo->getMethodSig(getCurrentMethodHandle(), &Sig2);

      if ((Sig.numArgs != Sig2.numArgs) ||
          (Sig.isVarArg() != Sig2.isVarArg()) ||
          (Sig.hasTypeArg() != Sig2.hasTypeArg()) ||
          (Sig.hasThis() != Sig2.hasThis())) {
        // This is meant to catch illegal use of JMP
        // While it allows some flexibility in the arguments
        // that shouldn't really even be allowed, it serves
        // as a basic sanity check.  It will also catch cases
        // such as the following bad MSIL where the JMP is
        // overspecifying the prototype given that it isn't
        // a true callsite:
        //
        //   jmp varargs instance void foo(int,...,int)
        BADCODE("Signature of jump target inconsistent with current routine\n");
      }

      // TODO: Populate stack with current method's incoming
      // parameters, currently the genIR method needs to obtain it
      // from information gathered during the prepass.
      jmp((ReaderBaseNS::CallOpcode)MappedValue, Token);

      // NOTE: jmp's stack transition shows that no value is placed on
      // the stack
    } break;

    case ReaderBaseNS::CEE_LDARG:
      MappedValue = readValue<uint16_t>(Operand);
      goto LOAD_ARG;
    case ReaderBaseNS::CEE_LDARG_S:
      MappedValue = readValue<uint8_t>(Operand);
      goto LOAD_ARG;
    case ReaderBaseNS::CEE_LDARG_0:
    case ReaderBaseNS::CEE_LDARG_1:
    case ReaderBaseNS::CEE_LDARG_2:
    case ReaderBaseNS::CEE_LDARG_3:
    LOAD_ARG:
      verifyLdarg(TheVerificationState, MappedValue, Opcode);
      BREAK_ON_VERIFY_ONLY;
      ResultIR = loadArg(MappedValue, false);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDLOC:
      MappedValue = readValue<uint16_t>(Operand);
      goto LOAD_LOCAL;
    case ReaderBaseNS::CEE_LDLOC_S:
      MappedValue = readValue<uint8_t>(Operand);
      goto LOAD_LOCAL;
    case ReaderBaseNS::CEE_LDLOC_0:
    case ReaderBaseNS::CEE_LDLOC_1:
    case ReaderBaseNS::CEE_LDLOC_2:
    case ReaderBaseNS::CEE_LDLOC_3:
    LOAD_LOCAL:
      verifyLdloc(TheVerificationState, MappedValue, Opcode);
      BREAK_ON_VERIFY_ONLY;
      ResultIR = loadLocal(MappedValue);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDARGA: {
      uint16_t ArgNumber;

      ArgNumber = readValue<uint16_t>(Operand);
      verifyLdarg(TheVerificationState, ArgNumber, Opcode);
      verifyLoadAddr(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadArgAddress(ArgNumber);
      ReaderOperandStack->push(ResultIR);
    } break;

    case ReaderBaseNS::CEE_LDARGA_S: {
      uint8_t ArgNumber;

      ArgNumber = readValue<uint8_t>(Operand);
      verifyLdarg(TheVerificationState, ArgNumber, Opcode);
      verifyLoadAddr(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadArgAddress(ArgNumber);
      ReaderOperandStack->push(ResultIR);
    } break;

    case ReaderBaseNS::CEE_LDLOCA: {
      uint16_t ArgNumber;

      ArgNumber = readValue<uint16_t>(Operand);
      verifyLdloc(TheVerificationState, ArgNumber, Opcode);
      verifyLoadAddr(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadLocalAddress(ArgNumber);
      ReaderOperandStack->push(ResultIR);
    } break;

    case ReaderBaseNS::CEE_LDLOCA_S: {
      uint8_t ArgNumber;

      ArgNumber = readValue<uint8_t>(Operand);
      verifyLdloc(TheVerificationState, ArgNumber, Opcode);
      verifyLoadAddr(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadLocalAddress(ArgNumber);
      ReaderOperandStack->push(ResultIR);
    } break;

    case ReaderBaseNS::CEE_LDC_I8:
      verifyLoadConstant(TheVerificationState, Opcode);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadConstantI8(readValue<int64_t>(Operand));
      ReaderOperandStack->push(ResultIR);
      break;
    case ReaderBaseNS::CEE_LDC_R4:
      verifyLoadConstant(TheVerificationState, Opcode);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadConstantR4(readValue<float>(Operand));
      ReaderOperandStack->push(ResultIR);
      break;
    case ReaderBaseNS::CEE_LDC_R8:
      verifyLoadConstant(TheVerificationState, Opcode);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadConstantR8(readValue<double>(Operand));
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDC_I4:
      MappedValue = readValue<int32_t>(Operand);
      goto LOAD_CONSTANT;
    case ReaderBaseNS::CEE_LDC_I4_S:
      MappedValue = readValue<int8_t>(Operand);
      goto LOAD_CONSTANT;
    case ReaderBaseNS::CEE_LDC_I4_0:
    case ReaderBaseNS::CEE_LDC_I4_1:
    case ReaderBaseNS::CEE_LDC_I4_2:
    case ReaderBaseNS::CEE_LDC_I4_3:
    case ReaderBaseNS::CEE_LDC_I4_4:
    case ReaderBaseNS::CEE_LDC_I4_5:
    case ReaderBaseNS::CEE_LDC_I4_6:
    case ReaderBaseNS::CEE_LDC_I4_7:
    case ReaderBaseNS::CEE_LDC_I4_8:
    case ReaderBaseNS::CEE_LDC_I4_M1:
    LOAD_CONSTANT:
      // We need to record the source locations of ldcs, so insert a nop
      // to record the source location.
      nop();
      verifyLoadConstant(TheVerificationState, Opcode);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadConstantI4(MappedValue);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDELEM:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      goto LOAD_ELEMENT;
    case ReaderBaseNS::CEE_LDELEM_I1:
    case ReaderBaseNS::CEE_LDELEM_U1:
    case ReaderBaseNS::CEE_LDELEM_I2:
    case ReaderBaseNS::CEE_LDELEM_U2:
    case ReaderBaseNS::CEE_LDELEM_I4:
    case ReaderBaseNS::CEE_LDELEM_U4:
    case ReaderBaseNS::CEE_LDELEM_I8:
    case ReaderBaseNS::CEE_LDELEM_I:
    case ReaderBaseNS::CEE_LDELEM_R4:
    case ReaderBaseNS::CEE_LDELEM_R8:
    case ReaderBaseNS::CEE_LDELEM_REF:
      Token = mdTokenNil;
    LOAD_ELEMENT:
      verifyLoadElem(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      Arg2 = ReaderOperandStack->pop();
      ResultIR = loadElem((ReaderBaseNS::LdElemOpcode)MappedValue,
                          &ResolvedToken, Arg1, Arg2);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDELEMA:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyLoadElemA(TheVerificationState, HasReadOnlyPrefix, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      Arg2 = ReaderOperandStack->pop();
      ResultIR = loadElemA(&ResolvedToken, Arg1, Arg2, HasReadOnlyPrefix);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDFLD:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Field, &ResolvedToken);
      verifyFieldAccess(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // pop load address
      ResultIR =
          loadField(&ResolvedToken, Arg1, AlignmentPrefix, HasVolatilePrefix);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDFTN: {
      LoadFtnToken = readValue<mdToken>(Operand);
      resolveToken(LoadFtnToken, CORINFO_TOKENKIND_Method, &ResolvedToken);

      CORINFO_CALL_INFO CallInfo;
      getCallInfo(&ResolvedToken, nullptr /*constraint*/,
                  CORINFO_CALLINFO_LDFTN, &CallInfo);

      // Currently ReadyToRun has delegate constructor optimization only for
      // non-virtual function pointers resolved at compile time.
      if ((Flags & CORJIT_FLG_READYTORUN) && (CallInfo.kind != CORINFO_CALL)) {
        LoadFtnToken = mdTokenNil;
      }

      verifyLoadFtn(TheVerificationState, Opcode, &ResolvedToken,
                    ILInput + CurrentOffset, &CallInfo);
      BREAK_ON_VERIFY_ONLY;

      handleMemberAccess(CallInfo.accessAllowed,
                         CallInfo.callsiteCalloutHelper);

      ResultIR = rdrMakeLdFtnTargetNode(&ResolvedToken, &CallInfo);

      ReaderOperandStack->push(ResultIR);
    } break;

    case ReaderBaseNS::CEE_LDIND_I1:
    case ReaderBaseNS::CEE_LDIND_U1:
    case ReaderBaseNS::CEE_LDIND_I2:
    case ReaderBaseNS::CEE_LDIND_U2:
    case ReaderBaseNS::CEE_LDIND_I4:
    case ReaderBaseNS::CEE_LDIND_U4:
    case ReaderBaseNS::CEE_LDIND_I8:
    case ReaderBaseNS::CEE_LDIND_I:
    case ReaderBaseNS::CEE_LDIND_R4:
    case ReaderBaseNS::CEE_LDIND_R8:
    case ReaderBaseNS::CEE_LDIND_REF:
      verifyLoadIndirect(TheVerificationState,
                         (ReaderBaseNS::LdIndirOpcode)MappedValue);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      ResultIR = loadIndir((ReaderBaseNS::LdIndirOpcode)MappedValue, Arg1,
                           AlignmentPrefix, HasVolatilePrefix, false);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDLEN:
      verifyLoadLen(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      ResultIR = loadLen(Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDNULL:
      verifyLoadNull(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadNull();
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDSTR:
      Token = readValue<mdToken>(Operand);
      verifyLoadStr(TheVerificationState, Token);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadStr(Token);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDSFLD:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Field, &ResolvedToken);
      verifyFieldAccess(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadStaticField(&ResolvedToken, HasVolatilePrefix);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDSFLDA:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Field, &ResolvedToken);
      verifyFieldAccess(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = getStaticFieldAddress(&ResolvedToken);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDFLDA:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Field, &ResolvedToken);
      verifyFieldAccess(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      ResultIR = loadFieldAddress(&ResolvedToken, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDOBJ:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyLoadObj(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      removeStackInterference();
      ResultIR = loadObj(&ResolvedToken, Arg1, AlignmentPrefix,
                         HasVolatilePrefix, false);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDTOKEN:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Ldtoken, &ResolvedToken);
      verifyLoadToken(TheVerificationState, &ResolvedToken);
      LastLoadToken = Token;
      BREAK_ON_VERIFY_ONLY;

      ResultIR = loadToken(&ResolvedToken);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_LDVIRTFTN: {
      LoadFtnToken = readValue<mdToken>(Operand);
      resolveToken(LoadFtnToken, CORINFO_TOKENKIND_Method, &ResolvedToken);

      CORINFO_CALL_INFO CallInfo;
      CORINFO_CALLINFO_FLAGS CallFlags = (CORINFO_CALLINFO_FLAGS)(
          CORINFO_CALLINFO_LDFTN | CORINFO_CALLINFO_CALLVIRT);
      getCallInfo(&ResolvedToken, nullptr /*constraint*/, CallFlags, &CallInfo);

      // Currently ReadyToRun has delegate constructor optimization only for
      // non-virtual function pointers resolved at compile time.
      if ((Flags & CORJIT_FLG_READYTORUN) && (CallInfo.kind != CORINFO_CALL)) {
        LoadFtnToken = mdTokenNil;
      }

      verifyLoadFtn(TheVerificationState, Opcode, &ResolvedToken,
                    ILInput + CurrentOffset, &CallInfo);
      BREAK_ON_VERIFY_ONLY;

      handleMemberAccess(CallInfo.accessAllowed,
                         CallInfo.callsiteCalloutHelper);

      Arg1 = ReaderOperandStack->pop();
      ResultIR = loadVirtFunc(Arg1, &ResolvedToken, &CallInfo);
      ReaderOperandStack->push(ResultIR);
    } break;

    case ReaderBaseNS::CEE_LEAVE:
      TargetOffset = NextOffset + readValue<int32_t>(Operand);
      goto GEN_LEAVE;

    case ReaderBaseNS::CEE_LEAVE_S:
      TargetOffset = NextOffset + readValue<int8_t>(Operand);
      goto GEN_LEAVE;

    GEN_LEAVE:
      verifyLeave(TheVerificationState);
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      {
        clearStack();
        leave(TargetOffset);
      }
      break;

    case ReaderBaseNS::CEE_LOCALLOC:
      verifyFailure(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();

      if (!ReaderOperandStack->empty()) {
        BADCODE("LOCALLOC requires that the evaluation stack be empty, apart "
                "from the size parameter");
      }
      if (CurrentRegion != nullptr &&
          ReaderBaseNS::RGN_Root != rgnGetRegionType(CurrentRegion) &&
          ReaderBaseNS::RGN_Try != rgnGetRegionType(CurrentRegion)) {
        BADCODE("LOCALLOC cannot occur within an exception block: filter, "
                "catch, finally, or fault");
      }

      ResultIR = localAlloc(Arg1, isZeroInitLocals());
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_MKREFANY:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyMkRefAny(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      removeStackInterference();
      handleClassAccess(&ResolvedToken);
      ResultIR = makeRefAny(&ResolvedToken, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_NEG:
    case ReaderBaseNS::CEE_NOT:
      verifyUnary(TheVerificationState, (ReaderBaseNS::UnaryOpcode)MappedValue);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      ResultIR = unaryOp((ReaderBaseNS::UnaryOpcode)MappedValue, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_NEWARR:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Newarr, &ResolvedToken);
      verifyNewArr(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      handleClassAccess(&ResolvedToken);

      Arg1 = ReaderOperandStack->pop();
      removeStackInterference();
      ResultIR = newArr(&ResolvedToken, Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_NEWOBJ:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Method, &ResolvedToken);
      verifyNewObj(TheVerificationState, Opcode, HasTailCallPrefix,
                   &ResolvedToken, ILInput + CurrentOffset);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = newObj(Token, LoadFtnToken, CurrentOffset);
      // If we inlined the .ctor, then we won't have the return value
      // until after the inlinee
      if (ResultIR != nullptr) {
        ReaderOperandStack->push(ResultIR);
      }

      // Only the next newobj can 'consume' the LoadFtnToken
      LoadFtnToken = mdTokenNil;
      break;

    case ReaderBaseNS::CEE_NOP:
      BREAK_ON_VERIFY_ONLY;
      nop();
      break;

    case ReaderBaseNS::CEE_POP:
      verifyPop(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      pop(Arg1);
      break;

    case ReaderBaseNS::CEE_REFANYTYPE:
      verifyRefAnyType(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      removeStackInterference();
      ResultIR = refAnyType(Arg1);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_REFANYVAL:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyRefAnyVal(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      removeStackInterference();
      ResultIR = refAnyVal(Arg1, &ResolvedToken);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_RETHROW:
      // We have to clear the stack here because we're leaving the handler
      clearStack();
      verifyRethrow(TheVerificationState,
                    fgGetRegionFromMSILOffset(CurrentOffset));
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      rethrow();
      break;

    case ReaderBaseNS::CEE_RET: {
      // Insert a nop for debug and set return's IL offset to signify
      // that it is part of the epilogue
      nop();
      const bool IsCall = false;
      setDebugLocation(ICorDebugInfo::EPILOG, IsCall);
      CorInfoCallConv Conv;
      CorInfoType CorType;
      CORINFO_CLASS_HANDLE RetTypeClass;
      uint32_t NumArgs;
      bool IsVarArg, HasThis;
      uint8_t RetSig;
      bool SynchronizedMethod;

      verifyReturn(TheVerificationState, CurrentRegion);
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      // Get method return type (CorType)
      getMethodSigData(&Conv, &CorType, &RetTypeClass, &NumArgs, &IsVarArg,
                       &HasThis, &RetSig);

      SynchronizedMethod =
          ((getCurrentMethodAttribs() & CORINFO_FLG_SYNCH) != 0);

      // If no return type then stack must be empty
      if (CorType == CORINFO_TYPE_VOID) {
        Arg1 = nullptr;
      } else {
        Arg1 = ReaderOperandStack->pop();
      }

      // Generate call to monitor helper (if synchronized)
      // if return type is non-void, Return performs:
      //   - Convert return value to return type (if necessary)
      //   - Generate return instruction
      returnOpcode(Arg1, SynchronizedMethod);
    } break;

    case ReaderBaseNS::CEE_SHL:
    case ReaderBaseNS::CEE_SHR:
    case ReaderBaseNS::CEE_SHR_UN:
      verifyShift(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Shift amount
      Arg2 = ReaderOperandStack->pop(); // Operand to be shifted

      // The shift opcodes operate on 32-bit or larger operands
      // if the operand was < 32 we need to insert the conversion
      // to mimic the implicit conversion done by the abstract machine
      // Also if the shift is signed and the operand is no then force it

      ResultIR = shift((ReaderBaseNS::ShiftOpcode)MappedValue, Arg1, Arg2);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_SIZEOF:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifySizeOf(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      ResultIR = sizeofOpcode(&ResolvedToken);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_STARG: {
      uint16_t ArgNumber;

      ArgNumber = readValue<uint16_t>(Operand);
      verifyStarg(TheVerificationState, ArgNumber);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();

      removeStackInterferenceForLocalStore(Opcode, ArgNumber);
      storeArg(ArgNumber, Arg1, AlignmentPrefix, HasVolatilePrefix);
    } break;

    case ReaderBaseNS::CEE_STARG_S: {
      uint8_t ArgNumber;

      ArgNumber = readValue<uint8_t>(Operand);
      verifyStarg(TheVerificationState, ArgNumber);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();

      removeStackInterferenceForLocalStore(Opcode, ArgNumber);
      storeArg(ArgNumber, Arg1, AlignmentPrefix, HasVolatilePrefix);
    } break;

    case ReaderBaseNS::CEE_STELEM:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      goto STORE_ELEMENT;
    case ReaderBaseNS::CEE_STELEM_I:
    case ReaderBaseNS::CEE_STELEM_I1:
    case ReaderBaseNS::CEE_STELEM_I2:
    case ReaderBaseNS::CEE_STELEM_I4:
    case ReaderBaseNS::CEE_STELEM_I8:
    case ReaderBaseNS::CEE_STELEM_R4:
    case ReaderBaseNS::CEE_STELEM_R8:
    case ReaderBaseNS::CEE_STELEM_REF:
      Token = mdTokenNil;
    STORE_ELEMENT:
      verifyStoreElem(TheVerificationState,
                      (ReaderBaseNS::StElemOpcode)MappedValue, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the value to store
      Arg2 = ReaderOperandStack->pop(); // Pop the array index
      Arg3 = ReaderOperandStack->pop(); // Pop the address of the array base
      removeStackInterference();
      storeElem((ReaderBaseNS::StElemOpcode)MappedValue, &ResolvedToken, Arg1,
                Arg2, Arg3);
      break;

    case ReaderBaseNS::CEE_STFLD:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Field, &ResolvedToken);
      verifyFieldAccess(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the value to store
      Arg2 = ReaderOperandStack->pop(); // Pop the address of the object
      removeStackInterference();
      storeField(&ResolvedToken, Arg1, Arg2, AlignmentPrefix,
                 HasVolatilePrefix);
      break;

    case ReaderBaseNS::CEE_STLOC:
      MappedValue = readValue<uint16_t>(Operand);
      goto STORE_LOC;
    case ReaderBaseNS::CEE_STLOC_S:
      MappedValue = readValue<uint8_t>(Operand);
      goto STORE_LOC;
    case ReaderBaseNS::CEE_STLOC_0:
    case ReaderBaseNS::CEE_STLOC_1:
    case ReaderBaseNS::CEE_STLOC_2:
    case ReaderBaseNS::CEE_STLOC_3:
    STORE_LOC:
      verifyStloc(TheVerificationState, MappedValue);
      BREAK_ON_VERIFY_ONLY;
      Arg1 = ReaderOperandStack->pop();
      removeStackInterferenceForLocalStore(Opcode, MappedValue);
      storeLocal(MappedValue, Arg1, AlignmentPrefix, HasVolatilePrefix);
      break;

    case ReaderBaseNS::CEE_STIND_I1:
    case ReaderBaseNS::CEE_STIND_I2:
    case ReaderBaseNS::CEE_STIND_I4:
    case ReaderBaseNS::CEE_STIND_I8:
    case ReaderBaseNS::CEE_STIND_I:
    case ReaderBaseNS::CEE_STIND_R4:
    case ReaderBaseNS::CEE_STIND_R8:
    case ReaderBaseNS::CEE_STIND_REF:
      verifyStoreIndir(TheVerificationState,
                       (ReaderBaseNS::StIndirOpcode)MappedValue);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      Arg2 = ReaderOperandStack->pop();
      removeStackInterference();
      storeIndir((ReaderBaseNS::StIndirOpcode)MappedValue, Arg1, Arg2,
                 AlignmentPrefix, HasVolatilePrefix);
      break;

    case ReaderBaseNS::CEE_STOBJ:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyStoreObj(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the source object
      Arg2 = ReaderOperandStack->pop(); // Pop the destination object
      removeStackInterference();
      storeObj(&ResolvedToken, Arg1, Arg2, AlignmentPrefix, HasVolatilePrefix,
               false);
      break;

    case ReaderBaseNS::CEE_STSFLD:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Field, &ResolvedToken);
      verifyFieldAccess(TheVerificationState, Opcode, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the value to store from the stack
      removeStackInterference();
      storeStaticField(&ResolvedToken, Arg1, HasVolatilePrefix);
      break;

    case ReaderBaseNS::CEE_SWITCH: {
      uint32_t NumCases;

      verifySwitch(TheVerificationState);

      // 1. Parse switch operands from msil.

      // Each label is a 4 byte offset.
      NumCases = readNumberOfSwitchCases(&Operand);

      Param->VerifiedEndBlock = true;
      verifyFinishBlock(TheVerificationState,
                        Fg); // before MaintainOperandStack
      BREAK_ON_VERIFY_ONLY;

      // 2. Pop the top operand off the stack
      Arg1 = ReaderOperandStack->pop();

      // 3. If switch had cases then the flow graph builder has
      // rigged up successor edges from the switch.
      if (NumCases != 0) {
        switchOpcode(Arg1);

        // 4. Maintain operand stack for all successors.
        // If the operand stack is non-empty then it must be ushered
        // across the block boundaries.
        if (!ReaderOperandStack->empty()) {
          maintainOperandStack(Fg);
        }
      } else {
        // consume the operand
        pop(Arg1);
      }
      ReaderOperandStack->clearStack();
    } break;

    case ReaderBaseNS::CEE_TAILCALL:
      HasTailCallPrefix = true;
      verifyTail(TheVerificationState, CurrentRegion);

      BREAK_ON_VERIFY_ONLY;

      break;

    case ReaderBaseNS::CEE_CONSTRAINED:
      HasConstrainedPrefix = true;
      ConstraintTypeRef = readValue<mdToken>(Operand);
      verifyConstrained(TheVerificationState, ConstraintTypeRef);

      BREAK_ON_VERIFY_ONLY;

      break;

    case ReaderBaseNS::CEE_READONLY:
      HasReadOnlyPrefix = true;
      verifyReadOnly(TheVerificationState);

      BREAK_ON_VERIFY_ONLY;

      break;

    case ReaderBaseNS::CEE_THROW:
      verifyThrow(TheVerificationState);
      Param->HasFallThrough = false;
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop();
      throwOpcode(Arg1);

      // Should this be supported or does the stack need to be null at this
      // point?
      clearStack();
      break;

    case ReaderBaseNS::CEE_UNALIGNED:
      // this must be 1,2 or 4 (verified)
      AlignmentPrefix = (ReaderAlignType)readValue<uint8_t>(Operand);
      verifyUnaligned(TheVerificationState, AlignmentPrefix);
      BREAK_ON_VERIFY_ONLY;

      break;

    case ReaderBaseNS::CEE_UNBOX:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyUnbox(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the object pointer
      removeStackInterference();
      handleClassAccess(&ResolvedToken);
      ResultIR = unbox(&ResolvedToken, Arg1, false, Reader_AlignNatural, false);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_UNBOX_ANY:
      Token = readValue<mdToken>(Operand);
      resolveToken(Token, CORINFO_TOKENKIND_Class, &ResolvedToken);
      verifyUnboxAny(TheVerificationState, &ResolvedToken);
      BREAK_ON_VERIFY_ONLY;

      Arg1 = ReaderOperandStack->pop(); // Pop the object pointer
      removeStackInterference();
      handleClassAccess(&ResolvedToken);
      ResultIR =
          unboxAny(&ResolvedToken, Arg1, AlignmentPrefix, HasVolatilePrefix);
      ReaderOperandStack->push(ResultIR);
      break;

    case ReaderBaseNS::CEE_VOLATILE:
      HasVolatilePrefix = true;
      verifyVolatile(TheVerificationState);
      BREAK_ON_VERIFY_ONLY;

      break;

    default:
      // unknown opcode means we cannot continue
      BADCODE(MVER_E_UNKNOWN_OPCODE);
    } // opcode switch end

    // Reset prefixes
    if (Opcode != ReaderBaseNS::CEE_VOLATILE &&
        Opcode != ReaderBaseNS::CEE_TAILCALL &&
        Opcode != ReaderBaseNS::CEE_UNALIGNED &&
        Opcode != ReaderBaseNS::CEE_READONLY &&
        Opcode != ReaderBaseNS::CEE_CONSTRAINED) {
      HasVolatilePrefix = false;
      HasTailCallPrefix = false;
      AlignmentPrefix = Reader_AlignNatural;
      HasReadOnlyPrefix = false;
      HasConstrainedPrefix = false;
      ConstraintTypeRef = mdTokenNil;
    }

    verifyPrefixConsumed(TheVerificationState, Opcode);
    CurrentOffset = NextOffset;
  }

  // Verify any fallthrough
  if (VerificationNeeded && Param->HasFallThrough) {
    verifyFallThrough(TheVerificationState, Fg);
  }

  // Sometimes the end-block verification has to be done up in the
  // switch before we perform GenIR steps that involve passing the
  // stack along outgoing edges.
  if (!Param->VerifiedEndBlock) {
    verifyFinishBlock(TheVerificationState, Fg);
    Param->VerifiedEndBlock = true;
  }
}

// Main reader loop, called once for each reachable block.
void ReaderBase::readBytesForFlowGraphNode(FlowGraphNode *Fg,
                                           bool IsVerifyOnly) {
  ReadBytesForFlowGraphNodeHelperParam TheParam;
  TheParam.This = this;
  TheParam.Fg = Fg;
  TheParam.IsVerifyOnly = IsVerifyOnly;
  TheParam.CurrentOffset = 0;
  TheParam.LocalFault = false;
  TheParam.HasFallThrough = true;
  TheParam.VState = nullptr;
  TheParam.VerifiedEndBlock = false;

  // Quick check that the array is initialized correctly.
  ASSERTNR(OpcodeRemap[ReaderBaseNS::CEE_CLT_UN] == ReaderBaseNS::CltUn);

  // Initialize local information
  CurrentFgNode = Fg;
  CurrentRegion = fgNodeGetRegion(Fg);
  // Get copy of operand stack for this block.
  // OPTIMIZATION: Continue using the existing stack in the common case
  // where it is left empty at the end of the block.
  if (!IsVerifyOnly) {
    // Note we don't propagate empty stacks to successors, so if there is no
    // stack for this block, verify that the current stack is empty.
    ReaderStack *Temp = fgNodeGetOperandStack(Fg);
    if (Temp) {
      // If we are going to switch stacks make sure to clean up the active
      // stack -- any important state was propagated to the successors
      // already.
      if (ReaderOperandStack != Temp) {
        if (ReaderOperandStack != nullptr) {
          delete ReaderOperandStack;
        }
      }
      ReaderOperandStack = Temp->copy();
    } else {
      ReaderOperandStack->assertEmpty();
    }
  }

  // Find the offset at which to start reading the buffer
  TheParam.CurrentOffset = fgNodeGetStartMSILOffset(Fg);

  ASSERTNR(!VerificationNeeded || !IsVerifiableCode ||
           (TheParam.CurrentOffset == fgNodeGetEndMSILOffset(Fg)) ||
           isOffsetInstrStart(TheParam.CurrentOffset));

  beginFlowGraphNode(Fg, TheParam.CurrentOffset, IsVerifyOnly);

  PAL_TRY(ReadBytesForFlowGraphNodeHelperParam *, Param, &TheParam) {
    Param->This->readBytesForFlowGraphNodeHelper(Param);
  }
  PAL_EXCEPT_FILTER(objectFilter) {
    CorInfoHelpFunc ThrowHelper = CORINFO_HELP_VERIFICATION;

    // Handle verification error, remove IRNodes from block
    // and have it filled with code to throw a verification
    // error.
    clearCurrentBlock();

    switch (TheParam.Excep->Type) {
    case Reader_LocalVerificationException:
      ThrowHelper = CORINFO_HELP_VERIFICATION;
      break;
    case Reader_GlobalVerificationException:
      fatal(CORJIT_BADCODE);
      break;
    default:
      ASSERTMNR(UNREACHED, "Unknown ReaderExceptionType");
      ThrowHelper = CORINFO_HELP_VERIFICATION;
      break;
    }
    // Free the exception object
    delete TheParam.Excep;

    if ((Flags & CORJIT_FLG_IMPORT_ONLY) == 0)
      insertThrow(ThrowHelper, TheParam.CurrentOffset);

    TheParam.LocalFault = true;
    TheParam.HasFallThrough =
        false; // Blocks with errors can't have this verified

    // Delete all (non-EH reachability) flow edges that come from this block.
    fgRemoveAllActualSuccessorEdges(Fg);

    // Clear operand and verifier stack since block
    // successor edges are now cut and the operands
    // have no use.
    if (!IsVerifyOnly) {
      ReaderOperandStack->clearStack();
    }

    if (VerificationNeeded) {
      ASSERT(TheParam.VState);
      TheParam.VState->BlockIsBad = true;
    }

    // Even though it is bad, it may need to be taken off the worklist.
    if (!TheParam.VerifiedEndBlock) {
      verifyFinishBlock(TheParam.VState, Fg);
    }
  }
  PAL_ENDTRY

  if (!IsVerifyOnly) {
    // Notify client JIT that end of block has been reached.
    endFlowGraphNode(Fg, TheParam.CurrentOffset);

    // Propagate operand stack to successor blocks. Nothing to
    // do if stack is empty, or if this block caused a local
    // verification fault. Local verification error causes all
    // successor edges to be cut, so live operand stack has nowhere
    // to be propagated to.
    if (!(TheParam.LocalFault || ReaderOperandStack->empty())) {
      maintainOperandStack(Fg);
    }
  }
}

void ReaderBase::fgRemoveAllActualSuccessorEdges(FlowGraphNode *Block) {
  for (FlowGraphEdgeIterator SuccessorIterator =
           fgNodeGetSuccessorsActual(Block);
       !fgEdgeIteratorIsEnd(SuccessorIterator);
       fgEdgeIteratorMoveNextSuccessorActual(SuccessorIterator)) {
    fgDeleteEdge(SuccessorIterator);
  }
}

FlowGraphNodeWorkList *
ReaderBase::fgPrependUnvisitedSuccToWorklist(FlowGraphNodeWorkList *Worklist,
                                             FlowGraphNode *CurrBlock) {

  for (FlowGraphEdgeIterator SuccessorIterator = fgNodeGetSuccessors(CurrBlock);
       !fgEdgeIteratorIsEnd(SuccessorIterator);
       fgEdgeIteratorMoveNextSuccessor(SuccessorIterator)) {

    FlowGraphNode *Successor = fgEdgeIteratorGetSink(SuccessorIterator);

    if (!fgNodeIsVisited(Successor)) {
#ifndef NODEBUG
      // Ensure that no block is on the worklist twice.
      FlowGraphNodeWorkList *DbTemp;

      DbTemp = Worklist;
      while (DbTemp != nullptr) {
        if (DbTemp->Block == Successor) {
          ASSERTNR(UNREACHED);
        }
        DbTemp = DbTemp->Next;
      }
#endif

      FlowGraphNodeWorkList *NewBlockList =
          (FlowGraphNodeWorkList *)getTempMemory(sizeof(FlowGraphNodeWorkList));

      // Mark the block as added to the list
      fgNodeSetVisited(Successor, true);

      // Add the new blockList element to the head of the list.
      NewBlockList->Block = Successor;
      NewBlockList->Next = Worklist;
      NewBlockList->Parent = CurrBlock;
      Worklist = NewBlockList;
    }
  }

  return Worklist;
}

// MSILToIR - main reader function translates MSIL to IR using calls
// to GenIR object.
void ReaderBase::msilToIR(void) {

  FlowGraphNodeWorkList *Worklist;
  FlowGraphNode *FgHead, *FgTail;

  bool IsImportOnly = (Flags & CORJIT_FLG_IMPORT_ONLY) != 0;

  // If asked to verify
  if (IsImportOnly) {

// If verification is a necessary feature, then we can throw an NYI,
// else we will assume the code is verifiable.
#ifdef FEATURE_VERIFICATION
    throw NotYetImplementedException("verification");
#else
    return;
#endif
  }

  // Initialize the NodeOffsetListArray so it can be used even in the
  // reader pre-pass
  NodeOffsetListArraySize =
      (MethodInfo->ILCodeSize / FLOW_GRAPH_NODE_LIST_ARRAY_STRIDE) + 1;
  NodeOffsetListArray = (FlowGraphNodeOffsetList **)getTempMemory(
      sizeof(FlowGraphNodeOffsetList *) * NodeOffsetListArraySize);

  // Compiler dependent pre-pass
  readerPrePass(MethodInfo->ILCode, MethodInfo->ILCodeSize);

  // SEQUENCE POINTS: Query the runtime before we
  //  do anything to check for special debugger driven
  //  sequence points
  if (needSequencePoints()) {
    getCustomSequencePoints();
  }

  // we have block offsets now, eit can be verified
  verifyEIT();

  // Build the region graph and allocate the filter offset array
  rgnCreateRegionTree();

  // Notify GenIR of eh info.
  setEHInfo(EhRegionTree, AllRegionList);

  // Notify client if generics context must be kept alive
  //
  // This triggers the client to store the generics context on the
  // stack and report its location to the EE. The JIT must also be
  // responsible for extending its GC lifetime (if it is a gc tracked
  // pointer).
  bool KeepGenericsContextAlive =
      ((MethodInfo->options & CORINFO_GENERICS_CTXT_KEEP_ALIVE) != 0);
  methodNeedsToKeepAliveGenericsContext(KeepGenericsContextAlive);

  // Build flow graph
  FgHead = buildFlowGraph(&FgTail);

  if (VerificationNeeded) {
    GlobalVerifyData *GvData = fgNodeGetGlobalVerifyData(FgHead);
    ASSERTNR(GvData);
    GvData->TiStack = nullptr;
    GvData->StkDepth = 0;
    GvData->BlockIsBad = false;
    GvWorklistHead = nullptr;
    GvWorklistTail = nullptr;
  }

  AreInlining = true;

  // Notify GenIR we've finsihed building the flow graph
  readerMiddlePass();

  // Iterate over flow graph in depth-first preorder to discover reachable
  // blocks.
  Worklist =
      (FlowGraphNodeWorkList *)getTempMemory(sizeof(FlowGraphNodeWorkList));
  Worklist->Block = FgHead;
  Worklist->Next = nullptr;
  Worklist->Parent = nullptr;
  fgNodeSetVisited(FgHead, true);

// fake up edges to unreachable code for peverify
// (so we can report errors in unreachable code)
#ifdef CC_PEVERIFY
  for (FlowGraphNode *Block = FgHead; Block != FgTail;) {
    FlowGraphNode *NextBlock = fgNodeGetNext(Block);
    FlowGraphEdgeIterator SuccessorIterator = fgNodeGetSuccessors(Block);

    if (!fgEdgeIteratorIsDone(SuccessorIterator) && (Block != FgHead) &&
        (fgNodeGetStartMSILOffset(Block) != fgNodeGetEndMSILOffset(Block))) {
      fgAddArc(nullptr, FgHead, Block);
      FlowGraphEdgeIterator HeadSuccessorIterator = fgNodeGetSuccessors(FgHead);
      while (!fgEdgeIteratorIsDone(HeadSuccessorIterator)) {
        FlowGraphNode *succBlock = fgEdgeIteratorGetSink(HeadSuccessorIterator);
        if (succBlock == Block) {
          fgEdgeIteratorMakeFake(HeadSuccessorIterator);
        }
        fgEdgeIteratorMoveNextSuccessor(HeadSuccessorIterator);
      }
    }
    Block = nextBlock;
  }
  readerMiddlePass();
#endif

  // Set up the initial stack
  ReaderOperandStack = createStack();
  ASSERTNR(ReaderOperandStack);
  fgNodeSetOperandStack(FgHead, ReaderOperandStack);

  // Walk the graph in depth-first order to discover reachable nodes but don't
  // process them yet. All reachable nodes are marked as visited.
  while (Worklist != nullptr) {
    // Pop top block
    FlowGraphNode *Block = Worklist->Block;
    FlowGraphNodeWorkList *Next = Worklist->Next;
    delete Worklist;
    // Prepend unvisited successors to worklist
    Worklist = fgPrependUnvisitedSuccToWorklist(Next, Block);
  }

  // Process the blocks in the MSIL offset order. ECMA spec guarantees
  // that no back edge (in MSIL offset sense) will have a non-empty operand
  // stack.

  // Compute the MSIL offset order. We are using a list because processing
  // a block may result in more created blocks that need to be inserted in the
  // middle of the container.
  std::list<FlowGraphNode *> FlowGraphMSILOffsetOrder;
  uint32_t LastInsertedInOrderBlockEndOffset = 0;
  for (FlowGraphNode *Block = FgHead; Block != nullptr;
       Block = fgNodeGetNext(Block)) {
    uint32_t EndOffset = fgNodeGetEndMSILOffset(Block);
    if (fgNodeIsVisited(Block)) {
      assert(LastInsertedInOrderBlockEndOffset <=
             fgNodeGetStartMSILOffset(Block));
      LastInsertedInOrderBlockEndOffset = EndOffset;
      FlowGraphMSILOffsetOrder.push_back(Block);
    }
  }

  // Process the nodes in MSIL offset order.
  std::list<FlowGraphNode *>::iterator It = FlowGraphMSILOffsetOrder.begin();
  while (It != FlowGraphMSILOffsetOrder.end()) {
    FlowGraphNode *CurrentNode = *It;
    readBytesForFlowGraphNode(CurrentNode, IsImportOnly);

#ifndef CC_PEVERIFY
    if (IsImportOnly && !VerificationNeeded) {
      // If we were told to only verify, and we no longer need to verify,
      // presumably because we found something unverifiable, then we can
      // stop this loop early.
      ASSERTNR(!IsVerifiableCode);
      break;
    }
#endif // !CC_PEVERIFY

    // Check whether we created new FlowGraphNodes while processing CurrentNode.
    for (FlowGraphEdgeIterator SuccessorIterator =
             fgNodeGetSuccessors(CurrentNode);
         !fgEdgeIteratorIsEnd(SuccessorIterator);
         fgEdgeIteratorMoveNextSuccessor(SuccessorIterator)) {

      FlowGraphNode *Successor = fgEdgeIteratorGetSink(SuccessorIterator);
      if (!fgNodeIsVisited(Successor)) {
        // Check that CurrentNode is the only predecessor of Successor that
        // propagates stack.
        ASSERTNR(!fgNodeHasMultiplePredsPropagatingStack(Successor));
        ASSERTNR(fgNodePropagatesOperandStack(CurrentNode) ||
                 fgNodeHasNoPredsPropagatingStack(Successor));

        // The two checks above ensure that it's safe to insert Successor after
        // CurrentNode even if that breaks MSIL offset order.
        ++It;
        FlowGraphMSILOffsetOrder.insert(It, Successor);
        // Point the iterator back to CurrentNode.
        --It;
        --It;
        fgNodeSetVisited(Successor, true);
      }
    }
    ++It;
  }

  // global verification dataflow
  if (VerificationNeeded) {
// iteration portion of global verification, as needed to
// revisit node for which incoming edge has new confluence data
#if !defined(NODEBUG)
    // This paranoia counter will help us detect infinite loops
    int GlobalVerificationParanoiaCounter = 0;
#endif
    while (GvWorklistHead) {
      ASSERTNR(GvWorklistHead->Block);
      ASSERTNR(!GvWorklistHead->BlockIsBad);
      readBytesForFlowGraphNode(GvWorklistHead->Block, true);

#if !defined(NODEBUG)
      GlobalVerificationParanoiaCounter++;
      ASSERTNR(GlobalVerificationParanoiaCounter < 2000);
#endif
    }
  }

  // Give client a chance to do any bookkeeping necessary after reading MSIL
  // for all blocks but before removing unreachable ones.
  readerPostVisit();

  // Remove blocks that weren't marked as visited.
  fgRemoveUnusedBlocks(FgHead);

  // Verify that all blocks that remain were, in fact, visited.
  // Also, unset the visited bit on these blocks.
  for (FlowGraphNode *Block = FgHead; Block != nullptr;) {
    FlowGraphNode *NextBlock;
    NextBlock = fgNodeGetNext(Block);
#if !defined(NDEBUG)
    ASSERTNR(fgNodeIsVisited(Block));
#endif
    fgNodeSetVisited(Block, false);
    Block = NextBlock;
  }

  // Report result of verification to the VM
  if ((Flags & CORJIT_FLG_SKIP_VERIFICATION) == 0) {
    ASSERTNR(VerificationNeeded || !IsVerifiableCode);

    CorInfoMethodRuntimeFlags VerificationFlags;
    // Do not inline this function if we see it again.
    VerificationFlags =
        IsVerifiableCode ? (CORINFO_FLG_VERIFIABLE)
                         : CorInfoMethodRuntimeFlags(CORINFO_FLG_UNVERIFIABLE |
                                                     CORINFO_FLG_BAD_INLINEE);
    JitInfo->setMethodAttribs(getCurrentMethodHandle(), VerificationFlags);
  }

  //
  // Client post-pass
  //
  readerPostPass(IsImportOnly);

  // Cleanup memory used
  for (uint32_t Index = 0; Index < NodeOffsetListArraySize; Index++) {
    FlowGraphNodeOffsetList *Element = NodeOffsetListArray[Index];
    while (Element != nullptr) {
      FlowGraphNodeOffsetList *NextElement = Element->getNext();
      delete Element;
      Element = NextElement;
    }
  }
  delete NodeOffsetListArray;

  for (FlowGraphNode *Block = FgHead; Block != nullptr;
       Block = fgNodeGetNext(Block)) {
    ReaderStack *Stack = fgNodeGetOperandStack(Block);
    if (Stack != nullptr) {
      delete Stack;
      fgNodeSetOperandStack(Block, nullptr);
    }
  }

  if (ReaderOperandStack != nullptr) {
    delete ReaderOperandStack;
    ReaderOperandStack = nullptr;
  }

  EHRegionList *RegionList = AllRegionList;
  while (RegionList != nullptr) {
    EHRegionList *Next = rgnListGetNext(RegionList);
    EHRegion *Region = rgnListGetRgn(RegionList);
    EHRegionList *Children = rgnGetChildList(Region);
    while (Children != nullptr) {
      EHRegionList *NextChild = rgnListGetNext(Children);
      free(Children);
      Children = NextChild;
    }
    free(RegionList);
    free(Region);
    RegionList = Next;
  }
}

bool ReaderBase::fgNodeHasMultiplePredsPropagatingStack(FlowGraphNode *Node) {
  int NumberOfPredecessorsPropagatingStack = 0;
  for (FlowGraphEdgeIterator PredecessorIterator =
           fgNodeGetPredecessorsActual(Node);
       !fgEdgeIteratorIsEnd(PredecessorIterator);
       fgEdgeIteratorMoveNextPredecessorActual(PredecessorIterator)) {
    FlowGraphNode *PredecessorNode =
        fgEdgeIteratorGetSource(PredecessorIterator);
    if (fgNodePropagatesOperandStack(PredecessorNode)) {
      ++NumberOfPredecessorsPropagatingStack;
      if (NumberOfPredecessorsPropagatingStack > 1) {
        return true;
      }
    }
  }
  return false;
}

bool ReaderBase::fgNodeHasNoPredsPropagatingStack(FlowGraphNode *Node) {
  for (FlowGraphEdgeIterator PredecessorIterator =
           fgNodeGetPredecessorsActual(Node);
       !fgEdgeIteratorIsEnd(PredecessorIterator);
       fgEdgeIteratorMoveNextPredecessorActual(PredecessorIterator)) {
    FlowGraphNode *PredecessorNode =
        fgEdgeIteratorGetSource(PredecessorIterator);
    if (fgNodePropagatesOperandStack(PredecessorNode)) {
      return false;
    }
  }
  return true;
}

// Checks to see if a given offset is the start of an instruction. If
// the offset is not the start of an instruction the whole program
// must be discarded as it's global flow may not be able to be
// verified.
//
// The method should be called at the beginning of each basic block
// and for each branch target. This prevents us from ever reading the
// middle of an instr and allows us to bail ASAP if the flow graph is
// invalid.
bool ReaderBase::isOffsetInstrStart(uint32_t TargetOffset) {
  return (TargetOffset < MethodInfo->ILCodeSize) &&
         LegalTargetOffsets->getBit(TargetOffset);
}

// runtimeFilter allows the JIT to catch exceptions that may be
// thrown by the runtime using a runtime-supplied filter.
int ReaderBase::runtimeFilter(struct _EXCEPTION_POINTERS *ExceptionPointers,
                              void *Param) {
  // Copy the exception pointers so that the filter handler
  // can access them
  RuntimeFilterParams *FilterParam = (RuntimeFilterParams *)Param;
  FilterParam->ExceptionPointers = *ExceptionPointers;

  ReaderBase *TheReaderBase = (ReaderBase *)(FilterParam->This);
  return TheReaderBase->JitInfo->FilterException(ExceptionPointers);
}

// __RuntimeHandleException allows the JIT to call back into the
// runtime to clean up exceptions thrown that got handled.
void ReaderBase::runtimeHandleException(
    struct _EXCEPTION_POINTERS *ExceptionPointers) {
  return JitInfo->HandleException(ExceptionPointers);
}

// SEQUENCE POINTS
//
// JIT generated sequence points are located at stack empty locations,
// EH boundary regions, and after nops and calls.  Calls include
// call/calli/callvirt/jmp/newobj instructions.  Debugger generated
// sequece points are placed based on the values revieved from the
// runtime via getSequencePoints
void ReaderBase::sequencePoint(int Offset, ReaderBaseNS::OPCODE PrevOp) {
  ASSERTNR(needSequencePoints());

  uint32_t TypeFlags = ICorDebugInfo::SOURCE_TYPE_INVALID;
  bool GenSeqPoint = false;

  if (ReaderOperandStack->empty()) {
    TypeFlags |= ICorDebugInfo::STACK_EMPTY;
    GenSeqPoint = true;
  }

  switch (PrevOp) {
  case ReaderBaseNS::CEE_NOP:
    GenSeqPoint = true;
    break;
  case ReaderBaseNS::CEE_CALL:
  case ReaderBaseNS::CEE_CALLI:
  case ReaderBaseNS::CEE_CALLVIRT:
  case ReaderBaseNS::CEE_JMP:
  case ReaderBaseNS::CEE_NEWOBJ:
    TypeFlags |= ICorDebugInfo::CALL_SITE;
    GenSeqPoint = true;
    break;
  default:
    break;
  }

  // Check for debugger (pdb) generated sequence points
  if (CustomSequencePoints->getBit(Offset)) {
    TypeFlags |= ICorDebugInfo::SEQUENCE_POINT;
    GenSeqPoint = true;
  }

  if (GenSeqPoint) {
    setSequencePoint(Offset, (ICorDebugInfo::SourceTypes)TypeFlags);
  }
}

// getCustomSequencePoints: Query runtime for debugger specific
// sequence points. We store these sequence points in a bitvector for
// easy access. The implicitBoundaries argument is not used.
//
// TODO: Workingset-wise it might be better to keep these sequence
// points in a sorted array instead of a bit vector since the bv is
// sparse (in most cases).
void ReaderBase::getCustomSequencePoints() {
  uint32_t NumILOffsets;
  uint32_t *ILOffsets;
  ICorDebugInfo::BoundaryTypes ImplicitBoundaries;
  CustomSequencePoints =
      (ReaderBitVector *)getTempMemory(sizeof(ReaderBitVector));
  CORINFO_METHOD_HANDLE Method = getCurrentMethodHandle();

  ASSERTNR(needSequencePoints());

  JitInfo->getBoundaries(Method, &NumILOffsets, (DWORD **)&ILOffsets,
                         &ImplicitBoundaries);
  const uint32_t Size = MethodInfo->ILCodeSize;
  CustomSequencePoints->allocateBitVector(Size, this);
  for (uint32_t I = 0; I < NumILOffsets; I++) {
    // The debugger sometimes passes us bogus sequence points.
    // Act like the classic JIT and just ignore the out-of-range values
    if (ILOffsets[I] < Size) {
      CustomSequencePoints->setBit(ILOffsets[I]);
    }
  }
  JitInfo->freeArray(ILOffsets); // free the array
}

bool ReaderBase::rdrIsMethodVirtual(uint32_t MethodAttribs) {
  // final methods aren't virtual
  if ((MethodAttribs & CORINFO_FLG_FINAL) != 0)
    return false;

  // static methods aren't virtual
  if ((MethodAttribs & CORINFO_FLG_STATIC) != 0)
    return false;

  // methods not explicitly marked as virtual are not virtual
  if ((MethodAttribs & CORINFO_FLG_VIRTUAL) == 0)
    return false;

  // assume all other methods are virtual
  return true;
}

uint32_t ReaderCallTargetData::getClassAttribs() {
  if (!AreClassAttribsValid) {
    TargetClassAttribs = Reader->getClassAttribs(getClassHandle());
    AreClassAttribsValid = true;
  }
  return TargetClassAttribs;
}

CORINFO_CLASS_HANDLE
ReaderCallTargetData::getClassHandle() {
  if (TargetClassHandle == nullptr) {
    CORINFO_METHOD_HANDLE Method = getMethodHandle();
    assert(Method != nullptr);
    TargetClassHandle = Reader->getMethodClass(Method);
    assert(TargetClassHandle != nullptr);
  }
  return TargetClassHandle;
}

CORINFO_CONTEXT_HANDLE
ReaderCallTargetData::getExactContext() {
  return CallInfo.exactContextNeedsRuntimeLookup
             ? MAKE_METHODCONTEXT(getMethodHandle())
             : CallInfo.contextHandle;
}

IRNode *ReaderCallTargetData::getMethodHandleNode() {
  if (!TargetMethodHandleNode) {
    ASSERTNR(!isIndirect()); // This makes no sense for indirect calls
    TargetMethodHandleNode =
        Reader->genericTokenToNode(&ResolvedToken, false, true);
  }
  return TargetMethodHandleNode;
}

IRNode *ReaderCallTargetData::getClassHandleNode() {
  if (!TargetClassHandleNode) {
    TargetClassHandleNode =
        Reader->genericTokenToNode(&ResolvedToken, true, true);
  }
  return TargetClassHandleNode;
}

IRNode *ReaderCallTargetData::applyThisTransform(IRNode *ThisNode) {
  ASSERTNR(hasThis());
  CORINFO_CALL_INFO *CallInfo = getCallInfo();

  // No CallInfo for CALLI, which also can't have any this transform
  if (CallInfo == nullptr ||
      CallInfo->thisTransform == CORINFO_NO_THIS_TRANSFORM) {
    return ThisNode;
  }

  if (CallInfo->thisTransform == CORINFO_DEREF_THIS) {
    // constraint calls on reference types dereference the byref used
    // to specify the object
    // Conservatively process the load as though the byref may be null.
    // No front-end is likely to generate a null byref here, but it
    // would be legal to do so.
    return Reader->loadIndir(ReaderBaseNS::LdindRef, ThisNode,
                             Reader_AlignNatural, false, false);
  } else {
    ASSERTNR(CallInfo->thisTransform == CORINFO_BOX_THIS);
    // Constraint calls on value types where there is no unboxed entry
    // point require us to box the value.  These only occur when a
    // value type has inherited an implementation of an interface
    // method from System.Object or System.ValueType.  The EE should
    // really provide the JITs with with "boxing" stubs for these
    // methods.
    //
    // looks like the steps are (from legacy JIT):
    //   1. load obj from pointer
    //   2. box obj that was loaded
    //   3. set this pointer to address returned from box
    return Reader->loadAndBox(&ResolvedConstraintToken, ThisNode,
                              Reader_AlignNatural);
  }
}

bool ReaderCallTargetData::getCallTargetNodeRequiresRuntimeLookup() {
  CORINFO_CALL_INFO *CallInfo = getCallInfo();

  ASSERTNR(CallInfo);
  if (CallInfo == nullptr) {
    return true; // Shouldn't have gotten here, but assume the worst
  }

  switch (CallInfo->kind) {
  case CORINFO_CALL:
    // Direct Call
    return false;

  case CORINFO_CALL_CODE_POINTER:
    // Runtime lookup required (code sharing w/o using inst param)
    ASSERTNR(CallInfo->codePointerLookup.lookupKind.needsRuntimeLookup);
    return true;

  case CORINFO_VIRTUALCALL_STUB:
    // Virtual Call via virtual dispatch stub
    return CallInfo->stubLookup.lookupKind.needsRuntimeLookup;

  case CORINFO_VIRTUALCALL_LDVIRTFTN:
    // Virtual Call via indirect virtual call
    return getClassHandleNodeRequiresRuntimeLookup() ||
           getMethodHandleNodeRequiresRuntimeLookup();

  case CORINFO_VIRTUALCALL_VTABLE:
    // Virtual call via table lookup (vtable)
    return false;

  default:
    ASSERTMNR(UNREACHED, "Unexpected call kind");
    return true;
  }
}

bool ReaderCallTargetData::getMethodHandleNodeRequiresRuntimeLookup() {
  CORINFO_GENERICHANDLE_RESULT Result;
  Reader->embedGenericHandle(&ResolvedToken, false, &Result);
  return Result.lookup.lookupKind.needsRuntimeLookup;
}

bool ReaderCallTargetData::getClassHandleNodeRequiresRuntimeLookup() {
  CORINFO_GENERICHANDLE_RESULT Result;
  Reader->embedGenericHandle(&ResolvedToken, true, &Result);
  return Result.lookup.lookupKind.needsRuntimeLookup;
}

bool ReaderCallTargetData::getTypeContextNodeRequiresRuntimeLookup() {
  if (SigInfo.hasTypeArg()) {
    if (((SIZE_T)CallInfo.contextHandle & CORINFO_CONTEXTFLAGS_MASK) ==
        CORINFO_CONTEXTFLAGS_METHOD) {
      // Instantiated generic method
      if (CallInfo.exactContextNeedsRuntimeLookup) {
        return true;
      } else {
        return false;
      }
    } else if ((getClassAttribs() & CORINFO_FLG_ARRAY) && IsReadonlyCall) {
      return false;
    } else {
      // otherwise must be an instance method in a generic struct, a
      // static method in a generic type, or a runtime-generated array
      // method which all use the class handle
      if (CallInfo.exactContextNeedsRuntimeLookup) {
        return true;
      } else {
        return false;
      }
    }
  }
  return false;
}

IRNode *ReaderCallTargetData::getTypeContextNode() {
  if (SigInfo.hasTypeArg()) {
    if (((SIZE_T)CallInfo.contextHandle & CORINFO_CONTEXTFLAGS_MASK) ==
        CORINFO_CONTEXTFLAGS_METHOD) {
      CORINFO_METHOD_HANDLE Method = (CORINFO_METHOD_HANDLE)(
          (SIZE_T)CallInfo.contextHandle & ~CORINFO_CONTEXTFLAGS_MASK);
      // Instantiated generic method
      if (Reader->Flags & CORJIT_FLG_READYTORUN) {
        return getReadyToRunTypeContextNode(mdtMethodHandle, (void *)Method);
      } else if (CallInfo.exactContextNeedsRuntimeLookup) {
        if (Reader->getCurrentMethodHandle() != Reader->MethodBeingCompiled) {
          // The runtime passes back bogus values for runtime lookups
          // of inlinees so the inliner only allows it if the handle
          // is never used
          return Reader->loadConstantI(0);
        } else {
          // runtime lookup based on the method token
          return getMethodHandleNode();
        }
      } else {
        // embed the handle passed back from getCallInfo
        Reader->methodMustBeLoadedBeforeCodeIsRun(Method);
        bool IsIndirect = false;
        void *MethodHandle = Reader->embedMethodHandle(Method, &IsIndirect);
        return Reader->handleToIRNode(mdtMethodHandle, MethodHandle, Method,
                                      IsIndirect, true, true, false);
      }
    } else {
      ASSERTNR(((SIZE_T)CallInfo.contextHandle & CORINFO_CONTEXTFLAGS_MASK) ==
               CORINFO_CONTEXTFLAGS_CLASS);
      CORINFO_CLASS_HANDLE Class = (CORINFO_CLASS_HANDLE)(
          (SIZE_T)CallInfo.contextHandle & ~CORINFO_CONTEXTFLAGS_MASK);

      if ((getClassAttribs() & CORINFO_FLG_ARRAY) && IsReadonlyCall) {
        // We indicate "readonly" to the Array::Address operation by
        // using a null instParam. This effectively tells it to not do
        // type validation.
        return Reader->loadConstantI(0);
      } else if (Reader->Flags & CORJIT_FLG_READYTORUN) {
        return getReadyToRunTypeContextNode(mdtClassHandle, (void *)Class);
      } else {
        // otherwise must be an instance method in a generic struct, a
        // static method in a generic type, or a runtime-generated array
        // method which all use the class handle
        if (CallInfo.exactContextNeedsRuntimeLookup) {
          if (Reader->getCurrentMethodHandle() != Reader->MethodBeingCompiled) {
            // The runtime passes back invalid values for runtime lookups
            // of inlinees so the inliner only allows it if the handle
            // is never used
            return Reader->loadConstantI(0);
          } else {
            // runtime lookup based on the class token
            return getClassHandleNode();
          }
        } else {
          Reader->classMustBeLoadedBeforeCodeIsRun(Class);
          bool IsIndirect = false;
          void *ClassHandle = Reader->embedClassHandle(Class, &IsIndirect);
          return Reader->handleToIRNode(mdtClassHandle, ClassHandle, Class,
                                        IsIndirect, true, true, false);
        }
      }
    }
  }
  return nullptr;
}

IRNode *
ReaderCallTargetData::getReadyToRunTypeContextNode(mdToken Token,
                                                   void *CompileHandle) {
  InfoAccessType AccessType = CallInfo.instParamLookup.accessType;
  void *EmbedHandle = nullptr;
  assert(AccessType != IAT_PPVALUE);
  bool IsIndirect;
  if (AccessType == IAT_VALUE) {
    EmbedHandle = CallInfo.instParamLookup.handle;
    IsIndirect = false;
  } else {
    assert(AccessType == IAT_PVALUE);
    EmbedHandle = CallInfo.instParamLookup.addr;
    IsIndirect = true;
  }
  const bool IsReadOnly = true;
  const bool IsRelocatable = true;
  const bool IsCallTarget = false;
  IRNode *InstParam =
      Reader->handleToIRNode(Token, EmbedHandle, CompileHandle, IsIndirect,
                             IsReadOnly, IsRelocatable, IsCallTarget);
  assert(InstParam != nullptr);
  return InstParam;
}

void ReaderCallTargetData::setOptimizedDelegateCtor(
    CORINFO_METHOD_HANDLE NewTargetMethodHandle) {
  // Leave the original class handle info around for canonNewObjCall

  this->TargetMethodHandle = NewTargetMethodHandle;
  this->TargetMethodHandleNode = nullptr;
  this->IsOptimizedDelegateCtor = true;

  this->TargetMethodAttribs = Reader->getMethodAttribs(TargetMethodHandle);

  Reader->getCallSiteSignature(NewTargetMethodHandle,
                               mdTokenNil, /* We don't have the token */
                               &this->SigInfo, &this->HasThisPtr);
}

// Private constructor: only called by friend class ReaderBase
void ReaderCallTargetData::init(
    ReaderBase *Reader, mdToken TargetToken, mdToken ConstraintToken,
    mdToken LoadFtnToken, bool IsTailCall, bool IsUnmarkedTailCall,
    bool IsReadonlyCall, ReaderBaseNS::CallOpcode Opcode, uint32_t MsilOffset,
    CORINFO_CONTEXT_HANDLE Context, CORINFO_MODULE_HANDLE Scope,
    CORINFO_METHOD_HANDLE Caller) {
  this->Reader = Reader;

  this->LoadFtnToken = LoadFtnToken;

  this->IsCallVirt = (Opcode == ReaderBaseNS::CallVirt);
  this->IsCallI = this->IsIndirect = (Opcode == ReaderBaseNS::Calli);
  this->IsNewObj = (Opcode == ReaderBaseNS::NewObj);
  this->IsJmp = (Opcode == ReaderBaseNS::Jmp);
  this->IsTailCall = IsTailCall;

  this->IsUnmarkedTailCall = IsUnmarkedTailCall;
  this->IsReadonlyCall = IsReadonlyCall;

  // nullptr out attribs, class handle, and nodes
  this->AreClassAttribsValid = false;
  this->IsCallInfoValid = false;
  this->NeedsNullCheck = false;
  this->CorIntrinsicId = CORINFO_INTRINSIC_Count;
  this->IsOptimizedDelegateCtor = false;
  this->CtorArgs = nullptr;
  this->TargetClassHandle = nullptr;
  this->TargetClassHandleNode = nullptr;
  this->TargetMethodHandleNode = nullptr;
  this->IndirectionCellNode = nullptr;
  this->CallTargetNode = nullptr;

  // fill CALL_INFO, SIG_INFO, METHOD_HANDLE, METHOD_ATTRIBS
  fillTargetInfo(TargetToken, ConstraintToken, Context, Scope, Caller,
                 MsilOffset);
}

// fill CALL_INFO, SIG_INFO, METHOD_HANDLE, and METHOD_ATTRIBS
void ReaderCallTargetData::fillTargetInfo(mdToken TargetToken,
                                          mdToken ConstraintToken,
                                          CORINFO_CONTEXT_HANDLE Context,
                                          CORINFO_MODULE_HANDLE Scope,
                                          CORINFO_METHOD_HANDLE Caller,
                                          uint32_t MsilOffset) {
  TargetMethodHandle = nullptr;
  TargetMethodAttribs = 0;

  ResolvedToken.token = TargetToken;
  ResolvedConstraintToken.token = ConstraintToken;
  if (!IsIndirect) {

    Reader->resolveToken(TargetToken, Context, Scope, CORINFO_TOKENKIND_Method,
                         &ResolvedToken);
    if (ConstraintToken != mdTokenNil)
      Reader->resolveToken(ConstraintToken, Context, Scope,
                           CORINFO_TOKENKIND_Constrained,
                           &ResolvedConstraintToken);
    Reader->getCallInfo(
        &ResolvedToken,
        (ConstraintToken != mdTokenNil) ? &ResolvedConstraintToken : nullptr,
        (CORINFO_CALLINFO_FLAGS)((IsCallVirt ? CORINFO_CALLINFO_CALLVIRT : 0) |
                                 CORINFO_CALLINFO_ALLOWINSTPARAM),
        &CallInfo, Caller);
    IsCallInfoValid = true;
    TargetMethodHandle = CallInfo.hMethod;
    TargetMethodAttribs = CallInfo.methodFlags;
  }

  Reader->getCallSiteSignature(TargetMethodHandle, ResolvedToken.token,
                               &SigInfo, &HasThisPtr, Context, Scope);

  if (IsTailCall && !IsUnmarkedTailCall && !IsJmp) {
// We have to validate that they didn't just put a invalid "tail."
// prefix on a call in the middle of nowhere.  We've already done
// this for unmarked tailcalls.
#if defined(TAMD64)
    bool AllowPop = ((Reader->MethodInfo->args.retType == CORINFO_TYPE_VOID) &&
                     (SigInfo.retType != CORINFO_TYPE_VOID));
#else  // TAMD64
    // Only AMD64 is smart enough to handle the POP
    bool AllowPop = false;
#endif // TAMD64

    IsTailCall = Reader->checkExplicitTailCall(MsilOffset, AllowPop);
  }

  // We no longer try and detect recursive tall calls here.
  this->IsRecursiveTailCall = false;

#if !defined(NODEBUG)
  // DEBUG: Attach the name of the target to the CallTargetData struct
  // NOTE: This hits a "Token out of range" assert in some ilstubs
  Reader->findNameOfToken(Scope, ResolvedToken.token, TargetName,
                          COUNTOF(TargetName));
#endif
}

void ReaderBase::resolveToken(mdToken Token, CORINFO_CONTEXT_HANDLE Context,
                              CORINFO_MODULE_HANDLE Scope,
                              CorInfoTokenKind TokenType,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  ResolvedToken->tokenContext = Context;
  ResolvedToken->tokenScope = Scope;
  ResolvedToken->token = Token;
  ResolvedToken->tokenType = TokenType;

#ifdef CC_PEVERIFY
  struct Param : JITFilterParam {
    CORINFO_RESOLVED_TOKEN *ResolvedToken;
    ReaderBase *pReader;
  } TheParam;
  TheParam.JitInfo = JitInfo;
  TheParam.ResolvedToken = ResolvedToken;
  TheParam.pReader = this;

  PAL_TRY(Param *, Param, &TheParam) {
    Param->pReader->JitInfo->resolveToken(Param->ResolvedToken);
  }
  PAL_EXCEPT_FILTER(eeJITFilter) {
    TheParam.JitInfo->HandleException(&TheParam.ExceptionPointers);
    GetErrorMessage(this, JitInfo);
    verifyOrReturn(false, MVER_E_TOKEN_RESOLVE);
  }
  PAL_ENDTRY
#else
  return JitInfo->resolveToken(ResolvedToken);
#endif
}

void ReaderBase::resolveToken(mdToken Token, CorInfoTokenKind TokenType,
                              CORINFO_RESOLVED_TOKEN *ResolvedToken) {
  return resolveToken(Token, getCurrentContext(), getCurrentModuleHandle(),
                      TokenType, ResolvedToken);
}

#pragma region SIMD_INTRINSICS

//===----------------------------------------------------------------------===//
//
// SIMD Intrinsics
//
//===----------------------------------------------------------------------===//

IRNode *ReaderBase::generateSIMDBinOp(ReaderSIMDIntrinsic OperationCode,
                                      CORINFO_CLASS_HANDLE Class) {
  IRNode *Arg2 = ReaderOperandStack->pop();
  IRNode *Arg1 = ReaderOperandStack->pop();
  if (isVectorType(Arg1) && isVectorType(Arg2)) {

    IRNode *Vector1 = Arg1;
    IRNode *Vector2 = Arg2;

    IRNode *ReturnNode = 0;
    bool IsSigned = getIsSigned(Class);
    unsigned VectorByteSize = getMaxIntrinsicSIMDVectorLength(Class);
    switch (OperationCode) {
    case ADD:
      ReturnNode = vectorAdd(Vector1, Vector2);
      break;
    case SUB:
      ReturnNode = vectorSub(Vector1, Vector2);
      break;
    case MUL:
      ReturnNode = vectorMul(Vector1, Vector2);
      break;
    case DIV:
      ReturnNode = vectorDiv(Vector1, Vector2, IsSigned);
      break;
    case MIN:
      ReturnNode = vectorMin(Vector1, Vector2, IsSigned);
      break;
    case MAX:
      ReturnNode = vectorMax(Vector1, Vector2, IsSigned);
      break;
    case BITOR:
      ReturnNode = vectorBitOr(Vector1, Vector2, VectorByteSize);
      break;
    case BITAND:
      ReturnNode = vectorBitAnd(Vector1, Vector2, VectorByteSize);
      break;
    case BITEXOR:
      ReturnNode = vectorBitExOr(Vector1, Vector2, VectorByteSize);
      break;
    default:
      break;
    }
    if (ReturnNode) {
      return ReturnNode;
    }
  }

  ReaderOperandStack->push(Arg1);
  ReaderOperandStack->push(Arg2);
  return 0;
}

IRNode *ReaderBase::generateSIMDUnOp(ReaderSIMDIntrinsic OperationCode) {
  IRNode *Arg = ReaderOperandStack->pop();
  if (isVectorType(Arg)) {
    IRNode *Vector = Arg;
    IRNode *ReturnNode = 0;
    switch (OperationCode) {
    case ABS:
      ReturnNode = vectorAbs(Vector);
      break;
    case SQRT:
      ReturnNode = vectorSqrt(Vector);
      break;
    default:
      break;
    }
    if (ReturnNode) {
      return ReturnNode;
    }
  }
  ReaderOperandStack->push(Arg);

  return 0;
}

IRNode *ReaderBase::generateSIMDIntrinsicCall(CORINFO_CLASS_HANDLE Class,
                                              CORINFO_METHOD_HANDLE Method,
                                              CORINFO_SIG_INFO *SigInfo,
                                              ReaderBaseNS::CallOpcode Opcode) {
  const char *ModuleName;
  const char *MethodName = getMethodName(Method, &ModuleName, JitInfo);
  if (!strcmp(MethodName, "get_IsHardwareAccelerated")) { // Special case.
    return generateIsHardwareAccelerated(Class);
  }

  IRNode *ReturnNode = 0;

  ReaderSIMDIntrinsic OperationType = UNDEF;
  if (!strcmp(MethodName, ".ctor")) {
    OperationType = CTOR;
  } else if (!strcmp(MethodName, "op_Addition")) {
    OperationType = ADD;
  } else if (!strcmp(MethodName, "op_Subtraction")) {
    OperationType = SUB;
  } else if (!strcmp(MethodName, "op_Multiply")) {
    OperationType = MUL;
  } else if (!strcmp(MethodName, "op_Division")) {
    OperationType = DIV;
  } else if (!strcmp(MethodName, "Min")) {
    OperationType = MIN;
  } else if (!strcmp(MethodName, "Max")) {
    OperationType = MAX;
  } else if (!strcmp(MethodName, "op_Equality")) {
    OperationType = EQ;
  } else if (!strcmp(MethodName, "op_Inequality")) {
    OperationType = NEQ;
  } else if (!strcmp(MethodName, "op_BitwiseOr")) {
    OperationType = BITOR;
  } else if (!strcmp(MethodName, "op_BitwiseAnd")) {
    OperationType = BITAND;
  } else if (!strcmp(MethodName, "op_ExclusiveOr")) {
    OperationType = BITEXOR;
  } else if (!strcmp(MethodName, "Abs")) {
    OperationType = ABS;
  } else if (!strcmp(MethodName, "SquareRoot")) {
    OperationType = SQRT;
  } else if (!strcmp(MethodName, "get_Count")) {
    OperationType = GETCOUNTOP;
  } else if (!strcmp(MethodName, "get_Item")) {
    OperationType = GETITEM;
  }
  CorInfoType ResType = SigInfo->retType;

  switch (OperationType) {
  case ADD:
  case SUB:
  case MUL:
  case DIV:
  case MIN:
  case MAX:
  case BITOR:
  case BITAND:
  case BITEXOR:
    ReturnNode = generateSIMDBinOp(OperationType, Class);
    break;
  case ABS:
  case SQRT:
    ReturnNode = generateSIMDUnOp(OperationType);
    break;
  case CTOR:
    assert(SigInfo->numArgs <= ReaderOperandStack->size());
    assert(SigInfo->hasThis());
    ReturnNode = generateSIMDCtor(Class, SigInfo->numArgs, Opcode);
    break;
  case GETCOUNTOP:
    ReturnNode = vectorGetCount(Class);
    break;
  case GETITEM:
    ReturnNode = generateSIMDGetItem(ResType);
    break;
  default:
    break;
  }
  return ReturnNode;
}

IRNode *ReaderBase::generateSIMDCtor(CORINFO_CLASS_HANDLE Class, int ArgsCount,
                                     ReaderBaseNS::CallOpcode Opcode) {
  std::vector<IRNode *> Args(ArgsCount);

  for (int Counter = ArgsCount - 1; Counter >= 0; --Counter) {
    Args[Counter] = ReaderOperandStack->pop();
  }
  IRNode *This = 0;
  if (Opcode != ReaderBaseNS::CallOpcode::NewObj) {
    This = ReaderOperandStack->pop();
  }
  IRNode *Result = vectorCtor(Class, This, Args);
  if (Result) {
    return Result;
  }
  if (Opcode != ReaderBaseNS::CallOpcode::NewObj) {
    ReaderOperandStack->push(This);
  }
  for (int Counter = 0; Counter < ArgsCount; ++Counter) {
    ReaderOperandStack->push(Args[Counter]);
  }
  return 0;
}

IRNode *ReaderBase::generateSIMDGetItem(CorInfoType ResType) {
  IRNode *Index = ReaderOperandStack->pop();
  IRNode *VectorPointer = ReaderOperandStack->pop();
  IRNode *ReturnNode = vectorGetItem(VectorPointer, Index, ResType);
  if (ReturnNode) {
    return ReturnNode;
  }
  ReaderOperandStack->push(VectorPointer);
  ReaderOperandStack->push(Index);
  return 0;
}

#pragma endregion
