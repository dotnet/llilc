//===------------------- include/Reader/readerenum.h ------------*- C++ -*-===//
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
/// \brief Enumerations that are useful in translating from MSIL bytecode to
/// some other representation.
///
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_ENUM_H
#define MSIL_READER_ENUM_H

namespace ReaderBaseNS {

// Opcode Enumerations for communicating opcode between
// reader and genir. Do not change the order of these
// enumerations as client may use these as indices into
// private arrays.

#include "openum.h"

/// \brief Describes the set of unary opcodes.
///
/// The operations indicated by these opcodes expect a single argument and
/// produce a single result.
enum UnaryOpcode {
  Neg = 0, ///< Produces the two's complement of its argument.
  Not,     ///< Produces the one's complement of its argument.

  LastUnaryOpcode
};

/// \brief Describes the set of binary opcodes.
///
/// The operations indicated by these opcodes expect two arguments and produce a
/// single result.
enum BinaryOpcode {
  Add = 0, ///< Produces the sum of its arguments.

  AddOvf, ///< Produces the sum of its arguments and throws an exception if
          ///< the result is out of range.

  AddOvfUn, ///< Produces the sum of its arguments (treating both as unsigned
            ///< integers) and throws an exception if the result is out of
            ///< range.

  And, ///< Produces the bitwise and of its arguments.

  Div, ///< Produces the quotient of its first argument divided by its
       ///< second argument.

  DivUn, ///< Produces the quotient of its first argument divided by its
         ///< second argument, treating both arguments as unsigned integers.

  Mul, ///< Produces the product of its arguments.

  MulOvf, ///< Produces the product of its arguments and throws an exception
          ///< if the result is out of range.

  MulOvfUn, ///< Produces the product of its arguments (treating both as
            ///< unsigned integers) and throws an exception if the result is
            ///< out of range.

  Or, ///< Produces the bitwise or of its arguments.

  Rem, ///< Produces the remainder of its first argument divided by its
       ///< second argument.

  RemUn, ///< Produces the remainder of its first argument divided by its
         ///< second argument, treating both arguments as unsigned integers.

  Sub, ///< Produces the difference of its arguments.

  SubOvf, ///< Produces the difference of its arguments and throws an
          ///< exception if the result is out of range.

  SubOvfUn, ///< Produces the difference of its arguments (treating both as
            ///< unsigned integers) and throws an exception if the result is
            ///< out of range.

  Xor, ///< Produces the bitwise exclusive-or of its arguments.

  LastBinaryOpcode
};

/// \brief Describes the set of boolean branch opcodes.
///
/// The operations indicated by these opcodes transfer control to a target
/// offset depending on the value of a single boolean argument.
enum BoolBranchOpcode {
  BrFalse = 0, ///< Branches to the target offset if its argument is false. The
               ///< target offset falls within the range [-2^31, 2^31).

  BrFalseS, ///< As #BrFalse, but the target offset falls within the range
            ///< [-2^7, 2^7).

  BrTrue, ///< Branches to the target offset it its argument is true. The
          ///< target offset falls withing the range [-2^31, 2^31).

  BrTrueS, ///< As #BrTrue, but the target offset falls within the range
           ///< [-2^7, 2^7).

  LastBoolBranchOpcode
};

/// \brief Describes the set of call opcodes.
///
/// The operations indicated by these opcodes are (possibly terminal) calls to a
/// specified method, passing any arguments accepted by the method and
/// optionally producing a single result from the method.
enum CallOpcode {
  Jmp = 0, ///< Exits the current method and jumps to the method described
           ///< by its argument.

  Call, ///< Calls the method described by its argument.

  CallVirt, ///< Calls the late-bound method described by its argument.

  Calli, ///< Calls through its method pointer argument using an indicated
         ///< method signature.

  Tail, ///< Tail calls the method described by its argument.

  // Call code is used to process NewObj
  NewObj, ///< Calls the constructor indicated by its argument using the
          ///< indicated storage.

  LastCallOpcode
};

/// \brief Describes the set of opcodes that compare two values.
///
/// The operations indicated by these opcodes expect two arguments and produce a
/// single result.
enum CmpOpcode {
  Ceq = 0, ///< Produces true if its first argument is equal to its second
           ///< argument and false otherwise.

  Cgt, ///< Produces true if its first argument is greater than its second
       ///< argument and false otherwise.

  CgtUn, ///< Produces true if its first argument is greater than its second
         ///< argument (treating both arguments as unsigned integers) and
         ///< false otherwise.

  Clt, ///< Produces true if its first argument is less than its second
       ///< argument and false otherwise.

  CltUn, ///< Produces true if its first argument is less than its second
         ///< argument (treating both arguments as unsigned integers) and
         ///< false otherwise.

  LastCmpOpcode
};

/// \brief Describes the set of conditional branch opcodes.
///
/// The operations indicated by these opcodes transfer control to a target
/// offset depending on the result of comparing two arguments. If an opcode
/// carries an "S" suffix, its target offset falls within the range [-2^7, 2^7);
/// otherwise, its target offset falls within the range [-2^31, 2^31).
enum CondBranchOpcode {
  Beq = 0, ///< Branches to the target offset if its arguments are equal.

  BeqS, ///< As #Beq, but with a short target offset.

  Bge, ///< Branches to the target offset if its first argument is greater
       ///< than or equal to its second argument.

  BgeS, ///< As #Bge, but with a short target offset.

  BgeUn, ///< Branches to the target offset if its first argument is greater
         ///< than or equal to its second argment, treating both arguments as
         ///< unsigned integers.

  BgeUnS, ///< As #BgeUn, but with a short target offset.

  Bgt, ///< Branches to the target offset if its first argument is greater
       ///< than its second argument.

  BgtS, ///< As #Bgt, but with a short target offset.

  BgtUn, ///< Branches to the target offset if its first argument is greater
         ///< than its second argment, treating both arguments as unsigned
         ///< integers.

  BgtUnS, ///< As #BgtUn, but with a short target offset.

  Ble, ///< Branches to the target offset if its first argument is less than
       ///< or equal to its second argument.

  BleS, ///< As #Ble, but with a short target offset.

  BleUn, ///< Branches to the target offset if its first argument is less than
         ///< or equal to its second argment, treating both arguments as
         ///< unsigned integers.

  BleUnS, ///< As #BleUn, but with a short target offset.

  Blt, ///< Branches to the target offset if its first argument is less than
       ///< its second argument.

  BltS, ///< As #Blt, but with a short target offset.

  BltUn, ///< Branches to the target offset if its first argument is less than
         ///< its second argment, treating both arguments as unsigned
         ///< integers.

  BltUnS, ///< As #BltUn, but with a short target offset.

  BneUn, ///< Branches to the target offset if its arguments are not equal,
         ///< treating both arguments as unsigned integers.

  BneUnS, ///< As #BneUn, but with a short target offset.

  LastCondBranchOpcode
};

/// \brief Describes the set of numeric conversion opcodes.
///
/// The operations described by these opcodes expect a single numeric operand
/// and produce the result of converting the argument to a specified numeric
/// type.
enum ConvOpcode {
  ConvI1 = 0, ///< Produces its argument converted to a 1-byte signed integer.

  ConvI2, ///< Produces its argument converted to a 2-byte signed integer.

  ConvI4, ///< Produces its argument converted to a 4-byte signed integer.

  ConvI8, ///< Produces its argument converted to an 8-byte signed integer.

  ConvR4, ///< Produces its argument converted to a 4-byte IEC 60559:1989
          ///< floating-point number.

  ConvR8, ///< Produces its argument converted to an 8-byte IEC 60559:1989
          ///< floating-point number.

  ConvU1, ///< Produces its argument converted to a 1-byte unsigned integer.

  ConvU2, ///< Produces its argument converted to a 2-byte unsigned integer.

  ConvU4, ///< Produces its argument converted to a 4-byte unsigned integer.

  ConvU8, ///< Produces its argument converted to an 8-byte unsigned
          ///< integer.

  ConvI, ///< Produces its argument converted to a natively sized signed
         ///< integer.

  ConvU, ///< Produces its argument converted to a natively sized unsigned
         ///< integer.

  ConvOvfI1, ///< As #ConvI1, but throws an exception if the result is out of
             ///< range.

  ConvOvfI2, ///< As #ConvI2, but throws an exception if the result is out of
             ///< range.

  ConvOvfI4, ///< As #ConvI4, but throws an exception if the result is out of
             ///< range.

  ConvOvfI8, ///< As #ConvI8, but throws an exception if the result is out of
             ///< range.

  ConvOvfU1, ///< As #ConvU1, but throws an exception if the result is out of
             ///< range.

  ConvOvfU2, ///< As #ConvU2, but throws an exception if the result is out of
             ///< range.

  ConvOvfU4, ///< As #ConvU4, but throws an exception if the result is out of
             ///< range.

  ConvOvfU8, ///< As #ConvU8, but throws an exception if the result is out of
             ///< range.

  ConvOvfI, ///< As #ConvI, but throws an exception if the result is out of
            ///< range.

  ConvOvfU, ///< As #ConvU, but throws an exception if the result is out of
            ///< range.

  ConvOvfI1Un, ///< As #ConvOvfI1, but treats its input as an unsigned integer.

  ConvOvfI2Un, ///< As #ConvOvfI2, but treats its input as an unsigned integer.

  ConvOvfI4Un, ///< As #ConvOvfI4, but treats its input as an unsigned integer.

  ConvOvfI8Un, ///< As #ConvOvfI8, but treats its input as an unsigned integer.

  ConvOvfU1Un, ///< As #ConvOvfU1, but treats its input as an unsigned integer.

  ConvOvfU2Un, ///< As #ConvOvfU2, but treats its input as an unsigned integer.

  ConvOvfU4Un, ///< As #ConvOvfU4, but treats its input as an unsigned integer.

  ConvOvfU8Un, ///< As #ConvOvfU8, but treats its input as an unsigned integer.

  ConvOvfIUn, ///< As #ConvOvfI, but treats its input as an unsigned integer.

  ConvOvfUUn, ///< As #ConvOvfU, but treats its input as an unsigned integer.

  ConvRUn, ///< Converts its argument to a floating-point number in the
           ///< runtime's internal representation, treating its input as an
           ///< unsigned integer.

  LastConvOpcode
};

/// \brief Describes the set of opcodes related to exception handling.
enum ExceptOpcode {
  EndFilter, ///< Indicates the end of a filter clause.

  Throw, ///< Throws the exception given as an argument.

  LastExceptOpcode
};

/// \brief Describes the set of element load opcodes.
///
/// The operations indicated by these opcodes expect two arguments, an array and
/// an index, and produce the element of the given array at the given index.
enum LdElemOpcode {
  LdelemI1 = 0, ///< Produces the 1-byte signed integer at the given index of
                ///< the given array.

  LdelemU1, ///< Produces the 1-byte unsigned integer at the given index of
            ///< the given array.

  LdelemI2, ///< Produces the 2-byte signed integer at the given index of
            ///< the given array.

  LdelemU2, ///< Produces the 2-byte unsigned integer at the given index of
            ///< the given array.

  LdelemI4, ///< Produces the 4-byte signed integer at the given index of
            ///< the given array.

  LdelemU4, ///< Produces the 4-byte unsigned integer at the given index of
            ///< the given array.

  LdelemI8, ///< Produces the 8-byte signed integer at the given index of
            ///< the given array.

  LdelemI, ///< Produces the natively sized signed integer at the given index
           ///< of the given array.

  LdelemR4, ///< Produces the 4-byte IEC 60559:1989 floating-point number at
            ///< the given index of the given array.

  LdelemR8, ///< Produces the 8-byte IEC 60559:1989 floating-point number at
            ///< the given index of the given array.

  LdelemRef, ///< Produces the object reference at the given index of the
             ///< given array.

  Ldelem, ///< Produces the value of the indicated type at the given
          ///< index of the given array.

  LastLdelemOpcode
};

/// \brief Describes the set of indirect load opcodes.
///
/// The operations indicated by these opcodes expect a single address argument
/// and produces the value stored at the argument.
enum LdIndirOpcode {
  LdindI1 = 0, ///< Produces the 1-byte signed integer stored at the given
               ///< address.

  LdindU1, ///< Produces the 1-byte unsigned integer stored at the given
           ///< address.

  LdindI2, ///< Produces the 2-byte signed integer stored at the given
           ///< address.

  LdindU2, ///< Produces the 2-byte unsigned integer stored at the given
           ///< address.

  LdindI4, ///< Produces the 4-byte signed integer stored at the given
           ///< address.

  LdindU4, ///< Produces the 4-byte unsigned integer stored at the given
           ///< address.

  LdindI8, ///< Produces the 8-byte signed integer stored at the given
           ///< address.

  LdindI, ///< Produces the natively sized signed integer stored at the
          ///< given address.

  LdindR4, ///< Produces the 4-byte IEC 60559:1989 floating-point number
           ///< stored at the given address.

  LdindR8, ///< Produces the 8-byte IEC 60559:1989 floating-point number
           ///< stored at the given address.

  LdindRef, ///< Produces the object reference stored at the given address.

  LastLdindOpcode
};

/// \brief Describes the set of element store opcodes.
///
/// The operations indicated by these opcodes expect three arguments, an array,
/// an index, and a value, and store the value at the given index of the given
/// array.
enum StElemOpcode {
  StelemI = 0, ///< Stores the given 1-byte integer at the given index of the
               ///< given array.

  StelemI1, ///< Stores the given 1-byte integer at the given index of the
            ///< given array.

  StelemI2, ///< Stores the given 2-byte integer at the given index of the
            ///< given array.

  StelemI4, ///< Stores the given 4-byte integer at the given index of the
            ///< given array.

  StelemI8, ///< Stores the given 8-byte integer at the given index of the
            ///< given array.

  StelemR4, ///< Stores the given 4-byte IEC 60559:1989 floating-point number
            ///< at the given index of the given array.

  StelemR8, ///< Stores the given 8-byte IEC 60559:1989 floating-point number
            ///< at the given index of the given array.

  StelemRef, ///< Stores the given object reference at the given index of the
             ///< given array.

  Stelem, ///< Stores the given value of the indicated type at the given
          ///< index of the given array.

  LastStelemOpcode
};

/// \brief Describes the set of shift opcodes.
///
/// The operations indicated by these opcodes expect two arguments, a value and
/// a shift amount, and produce the result of shifting the value by the shift
/// amount in a particular direction.
enum ShiftOpcode {
  Shl = 0, ///< Produces the result of shifting the given value leftwards by the
           ///< given shift amount.

  Shr, ///< Produces the result of an arithmetic shift right of the given
       ///< value by the given shift amount.

  ShrUn, ///< Produces the result of a logical shift right of the given value
         ///< by the given shift amount.

  LastShiftOpcode
};

/// \brief Describes the set of indirect store opcodes.
///
/// The operations indicated by these opcodes expect two arguments, an address
/// and a value, and store the value at the given address.
enum StIndirOpcode {
  StindI1 = 0, ///< Store the given 1-byte integer at the given address.

  StindI2, ///< Store the given 2-byte integer at the given address.

  StindI4, ///< Store the given 4-byte integer at the given address.

  StindI8, ///< Store the given 8-byte integer at the given address.

  StindI, ///< Store the given natively sized integer at the given address.

  StindR4, ///< Store the given 4-byte IEC 60559:1989 floating-point number
           ///< at the given address.

  StindR8, ///< Store the given 8-byte IEC 60559:1989 floating-point number
           ///< at the given address.

  StindRef, ///< Store the given object reference at the given address.

  LastStindOpcode
};

/// \brief Describes the set of region kinds.
typedef enum {
  RGN_Root,    ///< Indicates the root of a region tree.
  RGN_Try,     ///< Indicates a try region.
  RGN_Fault,   ///< Indicates a fault region.
  RGN_Finally, ///< Indicates a finally region.
  RGN_Filter,  ///< Indicates a filter region.
  RGN_MExcept, ///< Indicates a managed except region.
  RGN_MCatch   ///< Indicates a managed catch region.
} RegionKind;
}

/// \brief Used to map MSIL opcodes to function-specific opcode enumerations.
///
/// Uses the same ordering as enum opcode_t from openum.h.
SELECTANY const int8_t
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
        ReaderBaseNS::BeqS,
        ReaderBaseNS::BgeS,
        ReaderBaseNS::BgtS,
        ReaderBaseNS::BleS,
        ReaderBaseNS::BltS,
        ReaderBaseNS::BneUnS,
        ReaderBaseNS::BgeUnS,
        ReaderBaseNS::BgtUnS,
        ReaderBaseNS::BleUnS,
        ReaderBaseNS::BltUnS,
        -1,                    // CEE_BR,
        ReaderBaseNS::BrFalse, // CEE_BRFALSE,
        ReaderBaseNS::BrTrue,  // CEE_BRTRUE,
        ReaderBaseNS::Beq,
        ReaderBaseNS::Bge,
        ReaderBaseNS::Bgt,
        ReaderBaseNS::Ble,
        ReaderBaseNS::Blt,
        ReaderBaseNS::BneUn,
        ReaderBaseNS::BgeUn,
        ReaderBaseNS::BgtUn,
        ReaderBaseNS::BleUn,
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
        ReaderBaseNS::ConvOvfI8,   // CEE_CONV_OVF_I8,
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
