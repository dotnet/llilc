//===------------------- include/Reader/readerenum.h ------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
//
// Enumerations that are useful in translating from MSIL bytecode to some
// other representation.
//
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_ENUM_H
#define MSIL_READER_ENUM_H

namespace ReaderBaseNS {

// Opcode Enumerations for communicating opcode between
// reader and genir. Do not change the order of these
// enumerations as client may use these as indices into
// private arrays.

#include "openum.h"

enum UnaryOpcode {
  Neg = 0,
  Not,

  LastUnaryOpcode
};

enum BinaryOpcode {
  Add = 0,
  AddOvf,
  AddOvfUn,
  And,
  Div,
  DivUn,
  Mul,
  MulOvf,
  MulOvfUn,
  Or,
  Rem,
  RemUn,
  Sub,
  SubOvf,
  SubOvfUn,
  Xor,

  LastBinaryOpcode
};

enum BoolBranchOpcode {
  BrFalse = 0,
  BrFalseS,
  BrTrue,
  BrTrueS,

  LastBoolBranchOpcode
};

enum CallOpcode {
  Jmp = 0,
  Call,
  CallVirt,
  Calli,
  Tail,
  NewObj, // Call code is used to process NewObj...

  LastCallOpcode
};

enum CmpOpcode {
  Ceq = 0,
  Cgt,
  CgtUn,
  Clt,
  CltUn,

  LastCmpOpcode
};

enum CondBranchOpcode {
  Beq = 0,
  BeqS,
  Bge,
  BgeS,
  BgeUn,
  BgeUnS,
  Bgt,
  BgtS,
  BgtUn,
  BgtUnS,
  Ble,
  BleS,
  BleUn,
  BleUnS,
  Blt,
  BltS,
  BltUn,
  BltUnS,
  BneUn,
  BneUnS,

  LastCondBranchOpcode
};

enum ConvOpcode {
  ConvI1 = 0,
  ConvI2,
  ConvI4,
  ConvI8,
  ConvR4,
  ConvR8,
  ConvU1,
  ConvU2,
  ConvU4,
  ConvU8,
  ConvI,
  ConvU,

  ConvOvfI1,
  ConvOvfI2,
  ConvOvfI4,
  ConvOnvI8,
  ConvOvfU1,
  ConvOvfU2,
  ConvOvfU4,
  ConvOvfU8,
  ConvOvfI,
  ConvOvfU,

  ConvOvfI1Un,
  ConvOvfI2Un,
  ConvOvfI4Un,
  ConvOvfI8Un,
  ConvOvfU1Un,
  ConvOvfU2Un,
  ConvOvfU4Un,
  ConvOvfU8Un,
  ConvOvfIUn,
  ConvOvfUUn,
  ConvRUn,

  LastConvOpcode
};

enum ExceptOpcode {
  EndFilter,
  Throw,

  LastExceptOpcode
};

enum LdElemOpcode {
  LdelemI1 = 0,
  LdelemU1,
  LdelemI2,
  LdelemU2,
  LdelemI4,
  LdelemU4,
  LdelemI8,
  LdelemI,
  LdelemR4,
  LdelemR8,
  LdelemRef,
  Ldelem, // (M2 Generics)

  LastLdelemOpcode
};

enum LdIndirOpcode {
  LdindI1 = 0,
  LdindU1,
  LdindI2,
  LdindU2,
  LdindI4,
  LdindU4,
  LdindI8,
  LdindI,
  LdindR4,
  LdindR8,
  LdindRef,

  LastLdindOpcode
};

enum StElemOpcode {
  StelemI = 0,
  StelemI1,
  StelemI2,
  StelemI4,
  StelemI8,
  StelemR4,
  StelemR8,
  StelemRef,
  Stelem,

  LastStelemOpcode
};

enum ShiftOpcode {
  Shl = 0,
  Shr,
  ShrUn,

  LastShiftOpcode
};

enum StIndirOpcode {
  StindI1 = 0,
  StindI2,
  StindI4,
  StindI8,
  StindI,
  StindR4,
  StindR8,
  StindRef,

  LastStindOpcode
};

// Taken from rgn.h, eventually needs to go into its own file.
typedef enum {
  RGN_Unknown = 0,
  RGN_None,
  RGN_Root,
  RGN_Try,
  RGN_Except, // C++ except (SEH)
  RGN_Fault,
  RGN_Finally,
  RGN_Filter,
  RGN_Dtor,
  RGN_Catch,   // C++ catch
  RGN_MExcept, // managed (CLR) except
  RGN_MCatch,  // managed (CLR) catch

  // New region types used in common reader
  RGN_ClauseNone,
  RGN_ClauseFilter,
  RGN_ClauseFinally,
  RGN_ClauseError,
  RGN_ClauseFault,
} RegionKind;

// Taken from rgn.h, eventually needs to go into its own file.
typedef enum {
  TRY_None = 0,
  TRY_Fin,
  TRY_Fault,      // try/fault
  TRY_MCatch,     // the try has only catch handlers
  TRY_MCatchXcpt, // the try has both catch and except handlers
  TRY_MXcpt,      // the try has only except handlers
  // for native compiler code, not used in current JIT
  TRY_Xcpt,  // native SEH except
  TRY_CCatch // native C++ catch
} TryKind;
};

// Used to map read opcodes to function-specific opcode enumerations.
// Uses the same ordering as openum.h.
SELECTANY const char
    OpcodeRemap[ReaderBaseNS::CEE_MACRO_END - ReaderBaseNS::CEE_NOP] = {
        -1,                     // CEE_NOP,
        -1,                     // CEE_BREAK,
        0,                      // CEE_LDARG_0,
        1,                      // CEE_LDARG_1,
        2,                      // CEE_LDARG_2,
        3,                      // CEE_LDARG_3,
        0,                      // CEE_LDLOC_0,
        1,                      // CEE_LDLOC_1,
        2,                      // CEE_LDLOC_2,
        3,                      // CEE_LDLOC_3,
        0,                      // CEE_STLOC_0,
        1,                      // CEE_STLOC_1,
        2,                      // CEE_STLOC_2,
        3,                      // CEE_STLOC_3,
        -1,                     // CEE_LDARG_S,
        -1,                     // CEE_LDARGA_S,
        -1,                     // CEE_STARG_S,
        -1,                     // CEE_LDLOC_S,
        -1,                     // CEE_LDLOCA_S,
        -1,                     // CEE_STLOC_S,
        -1,                     // CEE_LDNULL,
        -1,                     // CEE_LDC_I4_M1,
        0,                      // CEE_LDC_I4_0,
        1,                      // CEE_LDC_I4_1,
        2,                      // CEE_LDC_I4_2,
        3,                      // CEE_LDC_I4_3,
        4,                      // CEE_LDC_I4_4,
        5,                      // CEE_LDC_I4_5,
        6,                      // CEE_LDC_I4_6,
        7,                      // CEE_LDC_I4_7,
        8,                      // CEE_LDC_I4_8,
        -1,                     // CEE_LDC_I4_S,
        -1,                     // CEE_LDC_I4,
        -1,                     // CEE_LDC_I8,
        -1,                     // CEE_LDC_R4,
        -1,                     // CEE_LDC_R8,
        -1,                     // CEE_UNUSED49,
        -1,                     // CEE_DUP,
        -1,                     // CEE_POP,
        ReaderBaseNS::Jmp,      // CEE_JMP,
        ReaderBaseNS::Call,     // CEE_CALL,
        ReaderBaseNS::Calli,    // CEE_CALLI,
        -1,                     // CEE_RET,
        -1,                     // CEE_BR_S,
        ReaderBaseNS::BrFalseS, // CEE_BRFALSE_S,
        ReaderBaseNS::BrTrueS,  // CEE_BRTRUE_S,
        ReaderBaseNS::BeqS,        ReaderBaseNS::BgeS,   ReaderBaseNS::BgtS,
        ReaderBaseNS::BleS,        ReaderBaseNS::BltS,   ReaderBaseNS::BneUnS,
        ReaderBaseNS::BgeUnS,      ReaderBaseNS::BgtUnS, ReaderBaseNS::BleUnS,
        ReaderBaseNS::BltUnS,
        -1,                    // CEE_BR,
        ReaderBaseNS::BrFalse, // CEE_BRFALSE,
        ReaderBaseNS::BrTrue,  // CEE_BRTRUE,
        ReaderBaseNS::Beq,         ReaderBaseNS::Bge,    ReaderBaseNS::Bgt,
        ReaderBaseNS::Ble,         ReaderBaseNS::Blt,    ReaderBaseNS::BneUn,
        ReaderBaseNS::BgeUn,       ReaderBaseNS::BgtUn,  ReaderBaseNS::BleUn,
        ReaderBaseNS::BltUn,
        -1,                        // CEE_SWITCH,
        ReaderBaseNS::LdindI1,     // CEE_LDIND_I1,
        ReaderBaseNS::LdindU1,     // CEE_LDIND_U1,
        ReaderBaseNS::LdindI2,     // CEE_LDIND_I2,
        ReaderBaseNS::LdindU2,     // CEE_LDIND_U2,
        ReaderBaseNS::LdindI4,     // CEE_LDIND_I4,
        ReaderBaseNS::LdindU4,     // CEE_LDIND_U4,
        ReaderBaseNS::LdindI8,     // CEE_LDIND_I8,
        ReaderBaseNS::LdindI,      // CEE_LDIND_I,
        ReaderBaseNS::LdindR4,     // CEE_LDIND_R4,
        ReaderBaseNS::LdindR8,     // CEE_LDIND_R8,
        ReaderBaseNS::LdindRef,    // CEE_LDIND_REF,
        ReaderBaseNS::StindRef,    // CEE_STIND_REF,
        ReaderBaseNS::StindI1,     // CEE_STIND_I1,
        ReaderBaseNS::StindI2,     // CEE_STIND_I2,
        ReaderBaseNS::StindI4,     // CEE_STIND_I4,
        ReaderBaseNS::StindI8,     // CEE_STIND_I8,
        ReaderBaseNS::StindR4,     // CEE_STIND_R4,
        ReaderBaseNS::StindR8,     // CEE_STIND_R8,
        ReaderBaseNS::Add,         // CEE_ADD,
        ReaderBaseNS::Sub,         // CEE_SUB,
        ReaderBaseNS::Mul,         // CEE_MUL,
        ReaderBaseNS::Div,         // CEE_DIV,
        ReaderBaseNS::DivUn,       // CEE_DIV_UN,
        ReaderBaseNS::Rem,         // CEE_REM,
        ReaderBaseNS::RemUn,       // CEE_REM_UN,
        ReaderBaseNS::And,         // CEE_AND,
        ReaderBaseNS::Or,          // CEE_OR,
        ReaderBaseNS::Xor,         // CEE_XOR,
        ReaderBaseNS::Shl,         // CEE_SHL,
        ReaderBaseNS::Shr,         // CEE_SHR,
        ReaderBaseNS::ShrUn,       // CEE_SHR_UN,
        ReaderBaseNS::Neg,         // CEE_NEG,
        ReaderBaseNS::Not,         // CEE_NOT,
        ReaderBaseNS::ConvI1,      // CEE_CONV_I1,
        ReaderBaseNS::ConvI2,      // CEE_CONV_I2,
        ReaderBaseNS::ConvI4,      // CEE_CONV_I4,
        ReaderBaseNS::ConvI8,      // CEE_CONV_I8,
        ReaderBaseNS::ConvR4,      // CEE_CONV_R4,
        ReaderBaseNS::ConvR8,      // CEE_CONV_R8,
        ReaderBaseNS::ConvU4,      // CEE_CONV_U4,
        ReaderBaseNS::ConvU8,      // CEE_CONV_U8,
        ReaderBaseNS::CallVirt,    // CEE_CALLVIRT,
        -1,                        // CEE_CPOBJ,
        -1,                        // CEE_LDOBJ,
        -1,                        // CEE_LDSTR,
        ReaderBaseNS::NewObj,      // CEE_NEWOBJ,
        -1,                        // CEE_CASTCLASS,
        -1,                        // CEE_ISINST,
        ReaderBaseNS::ConvRUn,     // CEE_CONV_R_UN,
        -1,                        // CEE_UNUSED58,
        -1,                        // CEE_UNUSED1,
        -1,                        // CEE_UNBOX,
        ReaderBaseNS::Throw,       // CEE_THROW,
        -1,                        // CEE_LDFLD,
        -1,                        // CEE_LDFLDA,
        -1,                        // CEE_STFLD,
        -1,                        // CEE_LDSFLD,
        -1,                        // CEE_LDSFLDA,
        -1,                        // CEE_STSFLD,
        -1,                        // CEE_STOBJ,
        ReaderBaseNS::ConvOvfI1Un, // CEE_CONV_OVF_I1_UN,
        ReaderBaseNS::ConvOvfI2Un, // CEE_CONV_OVF_I2_UN,
        ReaderBaseNS::ConvOvfI4Un, // CEE_CONV_OVF_I4_UN,
        ReaderBaseNS::ConvOvfI8Un, // CEE_CONV_OVF_I8_UN,
        ReaderBaseNS::ConvOvfU1Un, // CEE_CONV_OVF_U1_UN,
        ReaderBaseNS::ConvOvfU2Un, // CEE_CONV_OVF_U2_UN,
        ReaderBaseNS::ConvOvfU4Un, // CEE_CONV_OVF_U4_UN,
        ReaderBaseNS::ConvOvfU8Un, // CEE_CONV_OVF_U8_UN,
        ReaderBaseNS::ConvOvfIUn,  // CEE_CONV_OVF_I_UN,
        ReaderBaseNS::ConvOvfUUn,  // CEE_CONV_OVF_U_UN,
        -1,                        // CEE_BOX,
        -1,                        // CEE_NEWARR,
        -1,                        // CEE_LDLEN,
        -1,                        // CEE_LDELEMA,
        ReaderBaseNS::LdelemI1,    // CEE_LDELEM_I1,
        ReaderBaseNS::LdelemU1,    // CEE_LDELEM_U1,
        ReaderBaseNS::LdelemI2,    // CEE_LDELEM_I2,
        ReaderBaseNS::LdelemU2,    // CEE_LDELEM_U2,
        ReaderBaseNS::LdelemI4,    // CEE_LDELEM_I4,
        ReaderBaseNS::LdelemU4,    // CEE_LDELEM_U4,
        ReaderBaseNS::LdelemI8,    // CEE_LDELEM_I8,
        ReaderBaseNS::LdelemI,     // CEE_LDELEM_I,
        ReaderBaseNS::LdelemR4,    // CEE_LDELEM_R4,
        ReaderBaseNS::LdelemR8,    // CEE_LDELEM_R8,
        ReaderBaseNS::LdelemRef,   // CEE_LDELEM_REF,
        ReaderBaseNS::StelemI,     // CEE_STELEM_I,
        ReaderBaseNS::StelemI1,    // CEE_STELEM_I1,
        ReaderBaseNS::StelemI2,    // CEE_STELEM_I2,
        ReaderBaseNS::StelemI4,    // CEE_STELEM_I4,
        ReaderBaseNS::StelemI8,    // CEE_STELEM_I8,
        ReaderBaseNS::StelemR4,    // CEE_STELEM_R4,
        ReaderBaseNS::StelemR8,    // CEE_STELEM_R8,
        ReaderBaseNS::StelemRef,   // CEE_STELEM_REF,
        ReaderBaseNS::Ldelem,      // CEE_LDELEM (M2 Generics),
        ReaderBaseNS::Stelem,      // CEE_STELEM (M2 Generics),
        -1,                        // UNBOX_ANY (M2 Generics),
        -1,                        // CEE_UNUSED5
        -1,                        // CEE_UNUSED6
        -1,                        // CEE_UNUSED7
        -1,                        // CEE_UNUSED8
        -1,                        // CEE_UNUSED9
        -1,                        // CEE_UNUSED10
        -1,                        // CEE_UNUSED11
        -1,                        // CEE_UNUSED12
        -1,                        // CEE_UNUSED13
        -1,                        // CEE_UNUSED14
        -1,                        // CEE_UNUSED15
        -1,                        // CEE_UNUSED16
        -1,                        // CEE_UNUSED17
        ReaderBaseNS::ConvOvfI1,   // CEE_CONV_OVF_I1,
        ReaderBaseNS::ConvOvfU1,   // CEE_CONV_OVF_U1,
        ReaderBaseNS::ConvOvfI2,   // CEE_CONV_OVF_I2,
        ReaderBaseNS::ConvOvfU2,   // CEE_CONV_OVF_U2,
        ReaderBaseNS::ConvOvfI4,   // CEE_CONV_OVF_I4,
        ReaderBaseNS::ConvOvfU4,   // CEE_CONV_OVF_U4,
        ReaderBaseNS::ConvOnvI8,   // CEE_CONV_OVF_I8,
        ReaderBaseNS::ConvOvfU8,   // CEE_CONV_OVF_U8,
        -1,                        // CEE_UNUSED50,
        -1,                        // CEE_UNUSED18,
        -1,                        // CEE_UNUSED19,
        -1,                        // CEE_UNUSED20,
        -1,                        // CEE_UNUSED21,
        -1,                        // CEE_UNUSED22,
        -1,                        // CEE_UNUSED23,
        -1,                        // CEE_REFANYVAL,
        -1,                        // CEE_CKFINITE,
        -1,                        // CEE_UNUSED24,
        -1,                        // CEE_UNUSED25,
        -1,                        // CEE_MKREFANY,
        -1,                        // CEE_UNUSED59,
        -1,                        // CEE_UNUSED60,
        -1,                        // CEE_UNUSED61,
        -1,                        // CEE_UNUSED62,
        -1,                        // CEE_UNUSED63,
        -1,                        // CEE_UNUSED64,
        -1,                        // CEE_UNUSED65,
        -1,                        // CEE_UNUSED66,
        -1,                        // CEE_UNUSED67,
        -1,                        // CEE_LDTOKEN,
        ReaderBaseNS::ConvU2,      // CEE_CONV_U2,
        ReaderBaseNS::ConvU1,      // CEE_CONV_U1,
        ReaderBaseNS::ConvI,       // CEE_CONV_I,
        ReaderBaseNS::ConvOvfI,    // CEE_CONV_OVF_I,
        ReaderBaseNS::ConvOvfU,    // CEE_CONV_OVF_U,
        ReaderBaseNS::AddOvf,      // CEE_ADD_OVF,
        ReaderBaseNS::AddOvfUn,    // CEE_ADD_OVF_UN,
        ReaderBaseNS::MulOvf,      // CEE_MUL_OVF,
        ReaderBaseNS::MulOvfUn,    // CEE_MUL_OVF_UN,
        ReaderBaseNS::SubOvf,      // CEE_SUB_OVF,
        ReaderBaseNS::SubOvfUn,    // CEE_SUB_OVF_UN,
        -1,                        // CEE_ENDFINALLY,
        -1,                        // CEE_LEAVE,
        -1,                        // CEE_LEAVE_S,
        ReaderBaseNS::StindI,      // CEE_STIND_I,
        ReaderBaseNS::ConvU,       // CEE_CONV_U,
        -1,                        // CEE_UNUSED26,
        -1,                        // CEE_UNUSED27,
        -1,                        // CEE_UNUSED28,
        -1,                        // CEE_UNUSED29,
        -1,                        // CEE_UNUSED30,
        -1,                        // CEE_UNUSED31,
        -1,                        // CEE_UNUSED32,
        -1,                        // CEE_UNUSED33,
        -1,                        // CEE_UNUSED34,
        -1,                        // CEE_UNUSED35,
        -1,                        // CEE_UNUSED36,
        -1,                        // CEE_UNUSED37,
        -1,                        // CEE_UNUSED38,
        -1,                        // CEE_UNUSED39,
        -1,                        // CEE_UNUSED40,
        -1,                        // CEE_UNUSED41,
        -1,                        // CEE_UNUSED42,
        -1,                        // CEE_UNUSED43,
        -1,                        // CEE_UNUSED44,
        -1,                        // CEE_UNUSED45,
        -1,                        // CEE_UNUSED46,
        -1,                        // CEE_UNUSED47,
        -1,                        // CEE_UNUSED48,
        -1,                        // CEE_PREFIX7,
        -1,                        // CEE_PREFIX6,
        -1,                        // CEE_PREFIX5,
        -1,                        // CEE_PREFIX4,
        -1,                        // CEE_PREFIX3,
        -1,                        // CEE_PREFIX2,
        -1,                        // CEE_PREFIX1,
        -1,                        // CEE_PREFIXREF,

        -1,                      // CEE_ARGLIST,
        ReaderBaseNS::Ceq,       // CEE_CEQ,
        ReaderBaseNS::Cgt,       // CEE_CGT,
        ReaderBaseNS::CgtUn,     // CEE_CGT_UN,
        ReaderBaseNS::Clt,       // CEE_CLT,
        ReaderBaseNS::CltUn,     // CEE_CLT_UN,
        -1,                      // CEE_LDFTN,
        -1,                      // CEE_LDVIRTFTN,
        -1,                      // CEE_UNUSED56,
        -1,                      // CEE_LDARG,
        -1,                      // CEE_LDARGA,
        -1,                      // CEE_STARG,
        -1,                      // CEE_LDLOC,
        -1,                      // CEE_LDLOCA,
        -1,                      // CEE_STLOC,
        -1,                      // CEE_LOCALLOC,
        -1,                      // CEE_UNUSED57,
        ReaderBaseNS::EndFilter, // CEE_ENDFILTER,
        -1,                      // CEE_UNALIGNED,
        -1,                      // CEE_VOLATILE,
        -1,                      // CEE_TAILCALL,
        -1,                      // CEE_INITOBJ,
        -1,                      // CEE_CONSTRAINED,
        -1,                      // CEE_CPBLK,
        -1,                      // CEE_INITBLK,
        -1,                      // CEE_UNUSED69,
        -1,                      // CEE_RETHROW,
        -1,                      // CEE_UNUSED51,
        -1,                      // CEE_SIZEOF,
        -1,                      // CEE_REFANYTYPE,
        -1,                      // CEE_READONLY,
        -1,                      // CEE_UNUSED53,
        -1,                      // CEE_UNUSED54,
        -1,                      // CEE_UNUSED55,
        -1,                      // CEE_UNUSED70,
        -1,                      // CEE_ILLEGAL,
};

#endif // MSIL_READER_ENUM_H
