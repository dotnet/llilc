//===------------------- include/Reader/vtypeinfo.h -------------*- C++ -*-===//
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
/// \brief Type information for an MSIL verifier.
///
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_TYPEINFO_H
#define MSIL_READER_TYPEINFO_H

#include <string>

class TypeInfo;
typedef TypeInfo VerType;

typedef uint8_t VarTypes;
void gVerifyOrReturn(int32_t Cond, char *Message);
void gVerifyOrReturn(int32_t Cond, HRESULT Message);
#define gVerify(cond, Message) gVerifyOrReturn(cond, Message)

enum TITypes {
  TI_Error,

  TI_Ref,
  TI_Struct,
  TI_Method,

  TI_OnlyEnum = TI_Method, // Enum values above this are completely described by
                           // the enumeration

  TI_Byte,
  TI_Short,
  TI_Int,
  TI_Long,
  TI_I,
  TI_Float,
  TI_Double,
  TI_Null,
  TI_Ptr,

  TI_Count
};

enum VerErrTypeEnum {
  LocalError = 1,
  GlobalError = 2,
  TokenValid = 4,
  OffsetValid = 8
};

// going to OR these together
typedef uint32_t VerErrType;

// Convert the type returned from the VM to a TIType.

#define assertx(x)

#ifdef DEBUG
#define WHENDEBUG(x) x
#define PREFIX_ASSERT(x) assert(x)
#else
#define WHENDEBUG(x)
#define PREFIX_ASSERT(x)
#endif

SELECTANY const TITypes MapTIType[CORINFO_TYPE_COUNT] = {
    // see the definition of enum CorInfoType in file inc/corinfo.h
    TI_Error,  // CORINFO_TYPE_UNDEF
    TI_Error,  // CORINFO_TYPE_VOID
    TI_Byte,   // CORINFO_TYPE_BOOL
    TI_Short,  // CORINFO_TYPE_CHAR
    TI_Byte,   // CORINFO_TYPE_BYTE
    TI_Byte,   // CORINFO_TYPE_UBYTE
    TI_Short,  // CORINFO_TYPE_SHORT
    TI_Short,  // CORINFO_TYPE_USHORT
    TI_Int,    // CORINFO_TYPE_INT
    TI_Int,    // CORINFO_TYPE_UINT
    TI_Long,   // CORINFO_TYPE_LONG
    TI_Long,   // CORINFO_TYPE_ULONG
    TI_I,      // CORINFO_TYPE_NATIVEINT
    TI_I,      // CORINFO_TYPE_NATIVEUINT
    TI_Float,  // CORINFO_TYPE_FLOAT
    TI_Double, // CORINFO_TYPE_DOUBLE
    TI_Ref,    // CORINFO_TYPE_STRING
    TI_Ptr,    // CORINFO_TYPE_PTR
    TI_Error,  // CORINFO_TYPE_BYREF
    TI_Struct, // CORINFO_TYPE_VALUECLASS
    TI_Ref,    // CORINFO_TYPE_CLASS
    TI_Struct, // CORINFO_TYPE_REFANY
    TI_Ref,    // CORINFO_TYPE_VAR
};

inline TITypes jitType2TIType(CorInfoType Type) {
  // spot check to make certain enumerations have not changed
  assertx(MapTIType[CORINFO_TYPE_CLASS] == TI_Ref);
  assertx(MapTIType[CORINFO_TYPE_BYREF] == TI_Error);
  assertx(MapTIType[CORINFO_TYPE_DOUBLE] == TI_Double);
  assertx(MapTIType[CORINFO_TYPE_VALUECLASS] == TI_Struct);
  assertx(MapTIType[CORINFO_TYPE_STRING] == TI_Ref);

  Type = CorInfoType(Type & CORINFO_TYPE_MASK); // strip off modifiers

  assertx(Type < CORINFO_TYPE_COUNT);

  assertx(MapTIType[Type] != TI_Error || Type == CORINFO_TYPE_VOID);
  return MapTIType[Type];
}

/*****************************************************************************
 * Declares the TypeInfo class, which represents the type of an entity on the
 * stack, in a local variable or an argument.
 *
 * Flags: LLLLLLLLLLLLLLLLffffffffffTTTTTT
 *
 * L = local var # or instance field #
 * x = unused
 * f = flags
 * T = type
 *
 * The lower bits are used to store the type component, and may be one of:
 *
 * TI_* (primitive)   - see tyelist.h for enumeration (BYTE, SHORT, INT..)
 * TI_Ref             - OBJREF / ARRAY use m_cls for the type
 *                       (including arrays and null objref)
 * TI_Struct          - VALUE type, use m_cls for the actual type
 *
 * NOTE carefully that BYREF info is not stored here.  You will never see a
 * TI_BYREF in this component.  For example, the type component
 * of a "byref TI_Int" is TI_FLAG_BYREF | TI_Int.
 *
 * NOTE carefully that Generic Type Variable info is
 * only stored here in part.  Values of type "T" (e.g "!0" in ILASM syntax),
 * i.e. some generic variable type, appear only when verifying generic
 * code.  They come in two flavours: unboxed and boxed.  Unboxed
 * is the norm, e.g. a local, field or argument of type T.  Boxed
 * values arise from an IL instruction such as "box !0".
 * The EE provides type handles for each different type
 * variable and the EE's "canCast" operation decides casting
 * for boxed type variable. Thus:
 *
 *    (TI_Ref, <type-variable-type-handle>) == boxed type variable
 *
 *    (TI_Ref, <type-variable-type-handle>)
 *          + TI_FLAG_GENERIC_TYPE_VAR      == unboxed type variable
 *
 * Using TI_Ref for these may seem odd but using TI_Struct means the
 * code-generation parts of the importer get confused when they
 * can't work out the size, GC-ness etc. of the "struct".  So using TI_Ref
 * just tricks these backend parts into generating pseudo-trees for
 * the generic code we're verifying.  These trees then get thrown away
 * anyway as we do verification of genreic code in import-only mode.
 *
*/

// TI_Count is less than or equal to TI_FLAG_DATA_MASK

#define TI_FLAG_DATA_BITS 6
#define TI_FLAG_DATA_MASK ((1 << TI_FLAG_DATA_BITS) - 1)

// Flag indicating this item is uninitialised
// Note that if UNINIT and BYREF are both set,
// it means byref (uninit x) - i.e. we are pointing to an uninit <something>

#define TI_FLAG_UNINIT_OBJREF 0x00000040

// Flag indicating this item is a byref <something>

#define TI_FLAG_BYREF 0x00000080

// This item is a byref generated using the readonly. prefix
// to a ldelema or Address function on an array type.  The
// runtime type check is ignored in these cases, but the
// resulting byref can only be used in order to perform a
// constraint call.

#define TI_FLAG_BYREF_READONLY 0x00000100

// This item contains the 'this' pointer (used for tracking)

#define TI_FLAG_THIS_PTR 0x00001000

// This item is a byref to something which has a permanent home
// (e.g. a static field, or instance field of an object in GC heap, as
// opposed to the stack or a local variable).  TI_FLAG_BYREF must also be
// set. This information is useful for tail calls and return byrefs.
//
// Instructions that generate a permanent home byref:
//
//  ldelema
//  ldflda of a ref object or another permanent home byref
//  array element address Get() helper
//  call or calli to a method that returns a byref and is verifiable or
//  SkipVerify
//  dup
//  unbox

#define TI_FLAG_BYREF_PERMANENT_HOME 0x00002000

// This is for use when verifying generic code.
// This indicates that the type handle is really an unboxed
// generic type variable (e.g. the result of loading an argument
// of type T in a class List<T>).  Without this flag
// the same type handle indicates a boxed generic value,
// e.g. the result of a "box T" instruction.
#define TI_FLAG_GENERIC_TYPE_VAR 0x00004000

// Number of bits local var # is shifted

#define TI_FLAG_LOCAL_VAR_SHIFT 16
#define TI_FLAG_LOCAL_VAR_MASK 0xFFFF0000

// Field info uses the same space as the local info

#define TI_FLAG_FIELD_SHIFT TI_FLAG_LOCAL_VAR_SHIFT
#define TI_FLAG_FIELD_MASK TI_FLAG_LOCAL_VAR_MASK

#define TI_ALL_BYREF_FLAGS                                                     \
  (TI_FLAG_BYREF | TI_FLAG_BYREF_READONLY | TI_FLAG_BYREF_PERMANENT_HOME)

const CORINFO_CLASS_HANDLE BadClassHandle = (CORINFO_CLASS_HANDLE)-1;

/*****************************************************************************
 * A TypeInfo can be one of several types:
 * - A primitive type (I4,I8,R4,R8,I)
 * - A type (ref, array, value type) (m_cls describes the type)
 * - An array (m_cls describes the array type)
 * - A byref (byref flag set, otherwise the same as the above),
 * - A Function Pointer (m_method)
 * - A byref local variable (byref and byref local flags set), can be
 *   uninitialised
 *
 * The reason that there can be 2 types of byrefs (general byrefs, and byref
 * locals) is that byref locals initially point to uninitialised items.
 * Therefore these byrefs must be tracked specialy.
 */

class TypeInfo {
  friend class Compiler;

public:
  // private:
  union {
    // Right now m_bits is for debugging,
    // clang-format off
    struct {
      TITypes Type : 6;
      uint32_t UninitObj : 1; // used
      uint32_t ByRef : 1;     // used
      uint32_t ByRefReadOnly : 1;
      uint32_t: 3;            // unused?
      uint32_t ThisPtr : 1;   // used
      uint32_t: 1;            // unused?
      uint32_t GenericTypeVar : 1; // used
    } Bits;
    // clang-format on
    uint32_t Flags;
  };

  union {
    // Valid only for TI_Struct or TI_Ref
    CORINFO_CLASS_HANDLE Class;
    // Valid only for type TI_Method
    CORINFO_METHOD_HANDLE Method;
  };

public:
  TypeInfo() : Flags(TI_Error) {
    Bits.Type = TI_Error;
    WHENDEBUG(Class = BadClassHandle);
  }

  TypeInfo(TITypes TiType) {
    assertx((TiType >= TI_Byte) && (TiType <= TI_Null));
    assertx(TiType <= TI_FLAG_DATA_MASK);

    Flags = (uint32_t)TiType;
    WHENDEBUG(Class = BadClassHandle);
  }

  TypeInfo(CorInfoType VarType) {
    Flags = (uint32_t)jitType2TIType(VarType);
    WHENDEBUG(Class = BadClassHandle);
  }

  TypeInfo(TITypes TiType, CORINFO_CLASS_HANDLE Class, bool TypeVar = false) {
    assertx(TiType == TI_Struct || TiType == TI_Ref);
    assertx(Class != 0 && Class != CORINFO_CLASS_HANDLE(0xcccccccc));
    Flags = TiType;
    if (TypeVar)
      Flags |= TI_FLAG_GENERIC_TYPE_VAR;
    this->Class = Class;
  }

  TypeInfo(CORINFO_METHOD_HANDLE Method) {
    assertx(Method != 0 && Method != CORINFO_METHOD_HANDLE(0xcccccccc));
    Flags = TI_Method;
    this->Method = Method;
  }

  bool operator==(const TypeInfo &Ti) const {
    TITypes LType, RType;
    LType = getRawType();
    RType = Ti.getRawType();

    // I interchanges with INT
    // except when we are talking about addresses
    if (LType != RType) {
      if (!isByRef()) {
        if (!((LType == TI_I && RType == TI_Int) ||
              (LType == TI_Int && RType == TI_I)))
          return false;
      } else {
        return false;
      }
    }

    if (LType == TI_Ptr || RType == TI_Ptr)
      return false;

    if ((Flags & (TI_FLAG_BYREF | TI_FLAG_BYREF_READONLY |
                  TI_FLAG_GENERIC_TYPE_VAR | TI_FLAG_UNINIT_OBJREF)) !=
        (Ti.Flags & (TI_FLAG_BYREF | TI_FLAG_BYREF_READONLY |
                     TI_FLAG_GENERIC_TYPE_VAR | TI_FLAG_UNINIT_OBJREF)))
      return false;

    assertx(TI_Error < TI_OnlyEnum); // TI_Error looks like it needs more than
                                     // enum.  This optimises the success case a
                                     // bit
    if (LType > TI_OnlyEnum)
      return true;
    if (LType == TI_Error)
      return false; // TI_Error != TI_Error
    assertx(Class != BadClassHandle && Ti.Class != BadClassHandle);
    return Class == Ti.Class;
  }

  static bool tiMergeToCommonParent(ICorJitInfo *JitInfo, TypeInfo *PDest,
                                    const TypeInfo *PSrc);
  static bool tiCompatibleWith(ICorJitInfo *JitInfo, const TypeInfo &Child,
                               const TypeInfo &Parent);

  static bool tiMergeCompatibleWith(ICorJitInfo *JitInfo, const TypeInfo &Child,
                                    const TypeInfo &Parent);
  /////////////////////////////////////////////////////////////////////////
  // Operations
  /////////////////////////////////////////////////////////////////////////

  void setIsThisPtr() {
    Flags |= TI_FLAG_THIS_PTR;
    assertx(Bits.ThisPtr);
  }

  void clearThisPtr() { Flags &= ~(TI_FLAG_THIS_PTR); }

  void setIsPermanentHomeByRef() {
    assertx(isByRef());
    Flags |= TI_FLAG_BYREF_PERMANENT_HOME;
  }

  void setIsReadonlyByRef() {
    assertx(isByRef());
    Flags |= TI_FLAG_BYREF_READONLY;
  }

  // Set that this item is uninitialised.
  void setUninitialisedObjRef() {
    assertx((isObjRef() && isThisPtr()));
    // For now, this is used only  to track uninit this ptrs in ctors

    Flags |= TI_FLAG_UNINIT_OBJREF;
    assertx(Bits.UninitObj);
  }

  // Set that this item is initialised.
  void setInitialisedObjRef() {
    assertx((isObjRef() && isThisPtr()));
    // For now, this is used only  to track uninit this ptrs in ctors

    Flags &= ~TI_FLAG_UNINIT_OBJREF;
  }

  TypeInfo &dereferenceByRef() {
    if (!isByRef()) {
      Flags = TI_Error;
      WHENDEBUG(Class = BadClassHandle);
    }
    Flags &= ~(TI_FLAG_THIS_PTR | TI_ALL_BYREF_FLAGS);
    return *this;
  }

  TypeInfo &makeByRef() {
    assertx(!isByRef());
    Flags &= ~(TI_FLAG_THIS_PTR);
    Flags |= TI_FLAG_BYREF;
    return *this;
  }

  // I1,I2 --> I4
  // FLOAT --> DOUBLE
  // objref, arrays, byrefs, value classes are unchanged
  //
  TypeInfo &normaliseForStack() {
    switch (getType()) {
    case TI_Byte:
    case TI_Short:
      Flags = TI_Int;
      break;

    case TI_Float:
      Flags = TI_Double;
      break;
    default:
      break;
    }
    return (*this);
  }

  /////////////////////////////////////////////////////////////////////////
  // Getters
  /////////////////////////////////////////////////////////////////////////

  CORINFO_CLASS_HANDLE getClassHandle() const {
    // crusso: not sure about this conditional
    if (!isType(TI_Ref) && !isType(TI_Struct))
      return 0;

    return Class;
  }

  CORINFO_CLASS_HANDLE getClassHandleForValueClass() const {
    assertx(isType(TI_Struct));
    assertx(Class && Class != BadClassHandle);
    return Class;
  }

  CORINFO_CLASS_HANDLE getClassHandleForObjRef() const {
    assertx(isType(TI_Ref));
    assertx(Class && Class != BadClassHandle);
    return Class;
  }

  CORINFO_METHOD_HANDLE getMethod() const {
    assertx(getType() == TI_Method);
    return Method;
  }

  // Get this item's type
  // If primitive, returns the primitive type (TI_*)
  // If not primitive, returns:
  //  - TI_Error if a byref anything
  //  - TI_Ref if a class or array or null or a generic type variable
  //  - TI_Struct if a value class
  TITypes getType() const {
    if (Flags & TI_FLAG_BYREF)
      return TI_Error;

    // objref/array/null (objref), value class, ptr, primitive
    return getRawType();
  }

  TITypes getRawType() const { return (TITypes)(Flags & TI_FLAG_DATA_MASK); }

  bool isType(TITypes Type) const {
    assertx(Type != TI_Error);
    return (Flags & (TI_FLAG_DATA_MASK | TI_ALL_BYREF_FLAGS |
                     TI_FLAG_GENERIC_TYPE_VAR)) == uint32_t(Type);
  }

  // Returns whether this is an objref
  bool isObjRef() const { return isType(TI_Ref) || isType(TI_Null); }

  // Returns whether this is a by-ref
  bool isByRef() const { return (Flags & TI_FLAG_BYREF); }

  // Returns whether this is the this pointer
  bool isThisPtr() const { return (Flags & TI_FLAG_THIS_PTR); }

  bool isUnboxedGenericTypeVar() const {
    return !isByRef() && (Flags & TI_FLAG_GENERIC_TYPE_VAR);
  }

  bool isReadonlyByRef() const {
    return isByRef() && (Flags & TI_FLAG_BYREF_READONLY);
  }

  bool isPermanentHomeByRef() const {
    return isByRef() && (Flags & TI_FLAG_BYREF_PERMANENT_HOME);
  }

  // Returns whether this is a method desc
  bool isMethod() const { return (getType() == TI_Method); }

  // A byref value class is NOT a value class
  bool isValueClass() const {
    // TODO: make a table lookup for efficiency
    return (isType(TI_Struct) || isPrimitiveType());
  }

  // Does not return true for primitives. Will return true for value types that
  // behave as primitives
  bool isValueClassWithClassHnd() const {
    if ((getType() == TI_Struct) ||
        (Class && getType() != TI_Ref && getType() != TI_Method &&
         getType() != TI_Error)) // necessary because if byref bit is set, we
                                 // return TI_Error)
    {
      return TRUE;
    } else {
      return FALSE;
    }
  }

  // Returns whether this is an integer or real number
  // NOTE: Use NormaliseToPrimitiveType() if you think you may have a
  // System.Int32 etc., because those types are not considered number
  // types by this function.
  bool isNumberType() const {
    TITypes Type = getType();

    // I1, I2, Boolean, character etc. cannot exist nakedly -
    // everything is at least an I4

    return (Type == TI_Int || Type == TI_Long || Type == TI_I ||
            Type == TI_Double);
  }

  // Returns whether this is an integer
  // NOTE: Use NormaliseToPrimitiveType() if you think you may have a
  // System.Int32 etc., because those types are not considered number
  // types by this function.
  bool isIntegerType() const {
    TITypes Type = getType();

    // I1, I2, Boolean, character etc. cannot exist nakedly -
    // everything is at least an I4

    return (Type == TI_Int || Type == TI_Long || Type == TI_I);
  }

  // Returns whether this is a primitive type (not a byref, objref,
  // array, null, value class, invalid value)
  // May Need to normalise first (m/r/I4 --> I4)
  bool isPrimitiveType() const {
    uint32_t Type = getType();

    // boolean, char, u1,u2 never appear on the operand stack
    return (Type == TI_Byte || Type == TI_Short || Type == TI_Int ||
            Type == TI_I || Type == TI_Long || Type == TI_Float ||
            Type == TI_Double);
  }

  // Returns whether this is the null objref
  bool isNullObjRef() const { return (isType(TI_Null)); }

  // must be for a local which is an object type (i.e. has a slot >= 0)
  // for primitive locals, use the liveness bitmap instead
  // Note that this works if the error is 'Byref'
  bool isDead() const {
    return getRawType() == TI_Error || getRawType() == TI_Ptr;
  }

  bool isUninitialisedObjRef() const { return (Flags & TI_FLAG_UNINIT_OBJREF); }

  // In the watch window of the debugger, type tiVarName.ToStaticString()
  // to view a string representation of this instance.

  void dump() const;

  void toString(WCHAR *Buffer, int BufLen, ICorJitInfo *JitInfo,
                CORINFO_METHOD_HANDLE Ctxt) const;

  std::string toStaticString() const;

private:
  // used to make functions that return typeinfo efficient.
  TypeInfo(uint32_t Flags, CORINFO_CLASS_HANDLE Class) {
    this->Class = Class;
    this->Flags = Flags;
  }

  friend TypeInfo byRef(const TypeInfo &Ti);
  friend TypeInfo dereferenceByRef(const TypeInfo &Ti);
  friend TypeInfo normaliseForStack(const TypeInfo &Ti);
};

inline TypeInfo normaliseForStack(const TypeInfo &Ti) {
  return TypeInfo(Ti).normaliseForStack();
}

// given ti make a byref to that type.
inline TypeInfo byRef(const TypeInfo &Ti) { return TypeInfo(Ti).makeByRef(); }

// given ti which is a byref, return the type it points at
inline TypeInfo dereferenceByRef(const TypeInfo &Ti) {
  return TypeInfo(Ti).dereferenceByRef();
}

bool tiCompatibleWith(ICorJitInfo *JitInfo, const TypeInfo &Child,
                      const TypeInfo &Parent);

bool tiMergeToCommonParent(ICorJitInfo *JitInfo, TypeInfo *PDest,
                           const TypeInfo *PSrc);

bool tiEquivalent(ICorJitInfo *JitInfo, const VerType &ChildTarget,
                  const VerType &ParentTarget);

#ifndef CC_PEVERIFY // peverify OFF - just use the strings for debugging
                    // purposes
#include "vtypeinfodbg.def"

#else // CC_PEVERIFY on - use hresults and lookup

#include "vtypeinfores.def"

#endif

#endif // MSIL_READER_TYPEINFO_H
