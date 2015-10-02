//===--- include/Pal/LLILCPal.h ---------------------------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
//
// Minimal PAL for non-Windows platforms.
//
//===----------------------------------------------------------------------===//

#ifndef LLILC_PAL
#define LLILC_PAL

// Compatibility definitions for Architecture-specific attributes.
//
// __stdcall is X86 specific. MSVC ignores the attribute on other architectures,
// whereas other compilers complain about the ignored attribute.
#if (!defined(_MSC_VER) || defined(__clang__)) && !defined(_HOST_X86_)
#define __stdcall
#endif // MSC_VER && _HOST_X86

// We still need the PAL EH macros on Windows, so define them here.
#if defined(_MSC_VER)

#if defined(_DEBUG)
#include <windows.h> // For UINT
#endif

// clang-format off

// Note: PAL_SEH_RESTORE_GUARD_PAGE is only ever defined in clrex.h, so we only
// restore guard pages automatically when these macros are used from within the
// VM.
#define PAL_SEH_RESTORE_GUARD_PAGE

#define PAL_TRY_NAKED                                                          \
  {                                                                            \
    bool __exHandled;                                                          \
    __exHandled = false;                                                       \
    DWORD __exCode;                                                            \
    __exCode = 0;                                                              \
    __try {

#define PAL_EXCEPT_NAKED(Disposition)                                          \
  }                                                                            \
  __except (__exCode = GetExceptionCode(), Disposition) {                      \
    __exHandled = true;                                                        \
  PAL_SEH_RESTORE_GUARD_PAGE

#define PAL_EXCEPT_FILTER_NAKED(pfnFilter, param)                              \
  }                                                                            \
  __except (__exCode = GetExceptionCode(),                                     \
            pfnFilter(GetExceptionInformation(), param)) {                     \
    __exHandled = true;                                                        \
  PAL_SEH_RESTORE_GUARD_PAGE

#define PAL_FINALLY_NAKED                                                      \
  }                                                                            \
  __finally {

#define PAL_ENDTRY_NAKED                                                       \
  }                                                                            \
  }

#if defined(_DEBUG) && !defined(DACCESS_COMPILE)
//
// In debug mode, compile the try body as a method of a local class.
// This way, the compiler will check that the body is not directly
// accessing any local variables and arguments.
//
#define PAL_TRY(__ParamType, __paramDef, __paramRef)                           \
  {                                                                            \
    __ParamType __param = __paramRef;                                          \
    __ParamType __paramToPassToFilter = __paramRef;                            \
    class __Body {                                                             \
    public:                                                                    \
      static void run(__ParamType __paramDef) {

#define PAL_EXCEPT(Disposition)                                                \
  }                                                                            \
  }                                                                            \
  ;                                                                            \
  PAL_TRY_NAKED                                                                \
  __Body::run(__param);                                                        \
  PAL_EXCEPT_NAKED(Disposition)

#define PAL_EXCEPT_FILTER(pfnFilter)                                           \
  }                                                                            \
  }                                                                            \
  ;                                                                            \
  PAL_TRY_NAKED                                                                \
  __Body::run(__param);                                                        \
  PAL_EXCEPT_FILTER_NAKED(pfnFilter, __paramToPassToFilter)

#define PAL_FINALLY                                                            \
  }                                                                            \
  }                                                                            \
  ;                                                                            \
  PAL_TRY_NAKED                                                                \
  __Body::run(__param);                                                        \
  PAL_FINALLY_NAKED

#define PAL_ENDTRY                                                             \
  PAL_ENDTRY_NAKED                                                             \
  }

#else // _DEBUG

#define PAL_TRY(__ParamType, __paramDef, __paramRef)                           \
  {                                                                            \
    __ParamType __param = __paramRef;                                          \
    __ParamType __paramDef = __param;                                          \
    PAL_TRY_NAKED

#define PAL_EXCEPT(Disposition)                                                \
  PAL_EXCEPT_NAKED(Disposition)

#define PAL_EXCEPT_FILTER(pfnFilter)                                           \
  PAL_EXCEPT_FILTER_NAKED(pfnFilter, __param)

#define PAL_FINALLY                                                            \
  PAL_FINALLY_NAKED

#define PAL_ENDTRY                                                             \
  PAL_ENDTRY_NAKED                                                             \
  }

#endif // _DEBUG

#else // defined(_MSC_VER)

// Force inline

#if !defined(__has_attribute)
#define __has_attribute(x) 0
#endif

#if !defined(__forceinline)
#if __has_attribute(always_inline)
#define __forceinline __attribute__((always_inline)) inline 
#else
#define __forceinline inline
#endif
#endif

// SAL
#define _In_
#define __in_z
#define _Out_opt_
#define _Out_writes_to_opt_(size, count)
#define __out_ecount(count)
#define __inout_ecount(count)
#define __deref_inout_ecount(count)

#if defined(__cplusplus)
extern "C" {
#endif

// Linkage
#if defined(__cplusplus)
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif // __cplusplus

#define _ASSERTE assert
#define __assume(x) (void)0

#define UNALIGNED

#if defined(__GNUC__) && !defined(BIT64)
#define __cdecl __attribute__((cdecl))
#else
#define __cdecl
#endif

#define PALIMPORT EXTERN_C
#define PALAPI __stdcall

#define _cdecl __cdecl

#define STDMETHODCALLTYPE __stdcall

#define STDAPICALLTYPE __stdcall

#define STDMETHOD(method) virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type, method) virtual type STDMETHODCALLTYPE method

#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE
#define STDAPI_(type) EXTERN_C type STDAPICALLTYPE

#define PURE = 0

#if defined(__GNUC__)
#define DECLSPEC_NOVTABLE
#define DECLSPEC_IMPORT
#define DECLSPEC_SELECTANY __attribute__((weak))
#define SELECTANY extern __attribute__((weak))
#else
#define DECLSPEC_NOVTABLE
#define DECLSPEC_IMPORT
#define DECLSPEC_SELECTANY
#define SELECTANY
#endif

#if defined(__cplusplus)
}
#endif

#define _alloca alloca

// A bunch of source files (e.g. most of the ndp tree) include pal.h
// but are written to be LLP64, not LP64.  (LP64 => long = 64 bits
// LLP64 => longs = 32 bits, long long = 64 bits)
//
// To handle this difference, we #define long to be int (and thus 32 bits) when
// compiling those files.  (See the bottom of this file or search for
// #define long to see where we do this.)
//
// But this fix is more complicated than it seems, because we also use the
// preprocessor to #define __int64 to long for LP64 architectures (__int64
// isn't a builtin in gcc).   We don't want __int64 to be an int (by cascading
// macro rules).  So we play this little trick below where we add
// __cppmungestrip before "long", which is what we're really #defining __int64
// to.  The preprocessor sees __cppmungestriplong as something different than
// long, so it doesn't replace it with int.  The during the cppmunge phase, we
// remove the __cppmungestrip part, leaving long for the compiler to see.
//
// Note that we can't just use a typedef to define __int64 as long before
// #defining long because typedefed types can't be signedness-agnostic (i.e.
// they must be either signed or unsigned) and we want to be able to use
// __int64 as though it were intrinsic

#if defined(BIT64)
#define __int64 long
#else // _WIN64
#define __int64 long long
#endif // _WIN64

#define __int32 int
#define __int16 short int
#define __int8 char // assumes char is signed

typedef void VOID, *PVOID, *LPVOID, *LPCVOID;

#if !defined(PLATFORM_UNIX)
typedef long LONG typedef unsigned long ULONG;
typedef unsigned long DWORD
#else
typedef int LONG;           // NOTE: diff from windows.h, for LP64 compat
typedef unsigned int ULONG; // NOTE: diff from windows.h, for LP64 compat
typedef unsigned int DWORD; // NOTE: diff from windows.h, for LP64 compat
#endif

#if defined(PLATFORM_UNIX)
#if defined(__cplusplus)
    typedef char16_t WCHAR;
#else
    typedef unsigned short WCHAR;
#endif
#endif

typedef int BOOL;
typedef unsigned char BYTE, *PBYTE;
typedef char CHAR, *LPCSTR;
typedef unsigned char UCHAR;
typedef unsigned short WORD;
typedef short SHORT;
typedef unsigned short USHORT;
typedef WCHAR *LPWSTR, *LPCWSTR;
typedef int INT;
typedef unsigned int UINT;

typedef float FLOAT;
typedef double DOUBLE;

typedef BYTE BOOLEAN;

typedef signed char         INT8, *PINT8;
typedef signed short        INT16, *PINT16;
typedef signed int          INT32, *PINT32;
typedef signed __int64      INT64, *PINT64;
typedef unsigned char       UINT8, *PUINT8;
typedef unsigned short      UINT16, *PUINT16;
typedef unsigned int        UINT32, *PUINT32;
typedef unsigned __int64    UINT64, *PUINT64;

typedef unsigned __int32 ULONG32;
typedef __int64 LONGLONG;
typedef unsigned __int64 ULONGLONG;
typedef unsigned __int64 ULONG64;
typedef ULONGLONG DWORD64, *PDWORD64;

#define _W64

#if _WIN64
typedef unsigned __int64 UINT_PTR;
typedef unsigned __int64 ULONG_PTR;
typedef __int64 INT_PTR;
typedef __int64 LONG_PTR;
#else
typedef _W64 unsigned __int32 UINT_PTR;
typedef _W64 unsigned __int32 ULONG_PTR;
typedef _W64 __int32 INT_PTR;
typedef _W64 __int32 LONG_PTR;
#endif

typedef ULONG_PTR SIZE_T;
typedef LONG_PTR SSIZE_T;

#define MAXSIZE_T   ((SIZE_T)~((SIZE_T)0))
#define MAXSSIZE_T  ((SSIZE_T)(MAXSIZE_T >> 1))
#define MINSSIZE_T  ((SSIZE_T)~MAXSSIZE_T)

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

// Offset Computation

#if defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 5 || __GNUC__ > 3)
#define FIELD_OFFSET(type, field) __builtin_offsetof(type, field)
#define PAL_safe_offsetof(type, field) __builtin_offsetof(type, field)
#else
#define FIELD_OFFSET(type, field) (((LONG)(LONG_PTR)&(((type *)64)->field)) - 64)
#define PAL_safe_offsetof(s,m) ((size_t)((ptrdiff_t)&(char&)(((s *)64)->m))-64)
#endif

#define CONTAINING_RECORD(address, type, field) \
  ((type *)((LONG_PTR)(address)-FIELD_OFFSET(type, field)))

// Handles
typedef VOID *HANDLE;
typedef HANDLE HINSTANCE;

// HRESULTs
typedef LONG HRESULT;

// diff from Win32
#define MAKE_HRESULT(sev, fac, code)                                           \
  ((HRESULT)(((ULONG)(sev) << 31) | ((ULONG)(fac) << 16) | ((ULONG)(code))))

#define _HRESULT_TYPEDEF_(_sc) ((HRESULT)_sc)

#define S_OK _HRESULT_TYPEDEF_(0x00000000L)

#define FACILITY_NULL 0

#define NO_ERROR 0L

#define SEVERITY_ERROR 1

#define SUCCEEDED(Status) ((HRESULT)(Status) >= 0)
#define FAILED(Status) ((HRESULT)(Status) < 0)

// EH

#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING 0x2

#define EXCEPTION_MAXIMUM_PARAMETERS 15

typedef struct _EXCEPTION_RECORD {
  DWORD ExceptionCode;
  DWORD ExceptionFlags;
  struct _EXCEPTION_RECORD *ExceptionRecord;
  PVOID ExceptionAddress;
  DWORD NumberParameters;
  ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _EXCEPTION_POINTERS {
  PEXCEPTION_RECORD ExceptionRecord;
  PVOID ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;

#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_EXECUTION -1

#define GetExceptionCode (DWORD) __exception_code
#define GetExceptionInformation (PEXCEPTION_POINTERS) __exception_info

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

//
// Exception Handling ABI Level I: Base ABI
//

typedef enum {
  _URC_NO_REASON = 0,
  _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
  _URC_FATAL_PHASE2_ERROR = 2,
  _URC_FATAL_PHASE1_ERROR = 3,
  _URC_NORMAL_STOP = 4,
  _URC_END_OF_STACK = 5,
  _URC_HANDLER_FOUND = 6,
  _URC_INSTALL_CONTEXT = 7,
  _URC_CONTINUE_UNWIND = 8,
} _Unwind_Reason_Code;

typedef enum {
  _UA_SEARCH_PHASE = 1,
  _UA_CLEANUP_PHASE = 2,
  _UA_HANDLER_FRAME = 4,
  _UA_FORCE_UNWIND = 8,
} _Unwind_Action;
#define _UA_PHASE_MASK (_UA_SEARCH_PHASE | _UA_CLEANUP_PHASE)

struct _Unwind_Context;

void *_Unwind_GetIP(struct _Unwind_Context *context);
void _Unwind_SetIP(struct _Unwind_Context *context, void *new_value);
void *_Unwind_GetCFA(struct _Unwind_Context *context);
void *_Unwind_GetGR(struct _Unwind_Context *context, int index);
void _Unwind_SetGR(struct _Unwind_Context *context, int index, void *new_value);

struct _Unwind_Exception;

typedef void (*_Unwind_Exception_Cleanup_Fn)(
    _Unwind_Reason_Code urc, struct _Unwind_Exception *exception_object);

struct _Unwind_Exception {
  ULONG64 exception_class;
  _Unwind_Exception_Cleanup_Fn exception_cleanup;
  UINT_PTR private_1;
  UINT_PTR private_2;
} __attribute__((aligned));

void _Unwind_DeleteException(struct _Unwind_Exception *exception_object);

typedef _Unwind_Reason_Code (*_Unwind_Trace_Fn)(struct _Unwind_Context *context,
                                                void *pvParam);
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn pfnTrace, void *pvParam);

_Unwind_Reason_Code
_Unwind_RaiseException(struct _Unwind_Exception *exception_object);
__attribute__((noreturn)) void _Unwind_Resume(
    struct _Unwind_Exception *exception_object);

//
// Exception Handling ABI Level II: C++ ABI
//

void *__cxa_begin_catch(void *exceptionObject) throw();
void __cxa_end_catch();

#if defined(__cplusplus)
}
#endif                               // __cplusplus

typedef LONG EXCEPTION_DISPOSITION;

enum {
  ExceptionContinueExecution,
  ExceptionContinueSearch,
  ExceptionNestedException,
  ExceptionCollidedUnwind,
};

// A pretend exception code that we use to stand for external exceptions,
// such as a C++ exception leaking across a P/Invoke boundary into
// COMPlusFrameHandler.
#define EXCEPTION_FOREIGN 0xe0455874 // 0xe0000000 | 'EXT'

// Test whether the argument exceptionObject is an SEH exception.  If it is,
// return the associated exception pointers.  If it is not, return NULL.
typedef void (*PFN_PAL_BODY)(void *pvParam);

typedef struct _PAL_DISPATCHER_CONTEXT {
  _Unwind_Action actions;
  struct _Unwind_Exception *exception_object;
  struct _Unwind_Context *context;
} PAL_DISPATCHER_CONTEXT;

typedef EXCEPTION_DISPOSITION (*PFN_PAL_EXCEPTION_FILTER)(
    EXCEPTION_POINTERS *ExceptionPointers,
    PAL_DISPATCHER_CONTEXT *DispatcherContext, void *pvParam);

PALIMPORT
VOID PALAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
                           DWORD nNumberOfArguments,
                           const ULONG_PTR *lpArguments);

#if defined(__cplusplus)

class PAL_SEHException
{
    // We never want to create these directly, since technically this is an
    // incomplete definition of the type.
    PAL_SEHException() { }

public:
    // Note that the following two are actually embedded in this heap-allocated
    // instance - in contrast to Win32, where the exception record would usually
    // be allocated on the stack.  This is needed because foreign cleanup
    // handlers partially unwind the stack on the second pass.
    EXCEPTION_POINTERS ExceptionPointers;
    EXCEPTION_RECORD ExceptionRecord;
};

#endif // __cplusplus

// Start of a try block for exceptions raised by RaiseException
#define PAL_TRY(__ParamType, __ParamDef, __ParamRef)                           \
{                                                                              \
    __ParamType __Param = __ParamRef;                                          \
    auto __TryBlock = [](__ParamType __ParamDef)                               \
    {

// Start of an exception handler. If an exception raised by the RaiseException 
// occurs in the try block and the disposition is EXCEPTION_EXECUTE_HANDLER, 
// the handler code is executed. If the disposition is EXCEPTION_CONTINUE_SEARCH,
// the exception is rethrown. The EXCEPTION_CONTINUE_EXECUTION disposition is
// not supported.
#define PAL_EXCEPT(DispositionExpression)                                      \
    };                                                                         \
    const bool __IsFinally = false;                                            \
    auto __FinallyBlock = []() {};                                             \
    try                                                                        \
    {                                                                          \
        __TryBlock(__Param);                                                   \
    }                                                                          \
    catch (PAL_SEHException *__Ex)                                             \
    {                                                                          \
        EXCEPTION_DISPOSITION __Disposition = DispositionExpression;           \
        _ASSERTE(__Disposition != EXCEPTION_CONTINUE_EXECUTION);               \
        if (__Disposition == EXCEPTION_CONTINUE_SEARCH)                        \
        {                                                                      \
            throw;                                                             \
        }

// Start of an exception handler. It works the same way as the PAL_EXCEPT except
// that the disposition is obtained by calling the specified filter.
#define PAL_EXCEPT_FILTER(Filter)                                              \
    PAL_EXCEPT(Filter(&__Ex->ExceptionPointers, __Param))

// Start of a finally block. The finally block is executed both when the try
// block finishes or when an exception is raised using the RaiseException in it.
#define PAL_FINALLY                                                            \
    };                                                                         \
    const bool __IsFinally = true;                                             \
    auto __FinallyBlock = [&]()                                                \
    {

// End of an except or a finally block.
#define PAL_ENDTRY                                                             \
    };                                                                         \
    if (__IsFinally)                                                           \
    {                                                                          \
        try                                                                    \
        {                                                                      \
            __TryBlock(__Param);                                               \
        }                                                                      \
        catch (...)                                                            \
        {                                                                      \
            __FinallyBlock();                                                  \
            throw;                                                             \
        }                                                                      \
        __FinallyBlock();                                                      \
    }                                                                          \
}

// COM
typedef struct _GUID {
  ULONG Data1; // NOTE: diff from Win32, for LP64
  USHORT Data2;
  USHORT Data3;
  UCHAR Data4[8];
} GUID;

#if defined(__cplusplus)
#define REFGUID const GUID &
#else
#define REFGUID const GUID *
#endif

typedef GUID IID;
#if defined(__cplusplus)
#define REFIID const IID &
#else
#define REFIID const IID *
#endif

typedef GUID CLSID;
#if defined(__cplusplus)
#define REFCLSID const CLSID &
#else
#define REFCLSID const CLSID *
#endif

#define MIDL_INTERFACE(x) struct DECLSPEC_NOVTABLE

#define EXTERN_GUID(itf, l1, s1, s2, c1, c2, c3, c4, c5, c6, c7, c8)           \
  EXTERN_C const IID DECLSPEC_SELECTANY                                        \
      itf = {l1, s1, s2, {c1, c2, c3, c4, c5, c6, c7, c8}}

#define interface struct

#define DECLARE_INTERFACE(iface) interface DECLSPEC_NOVTABLE iface
#define DECLARE_INTERFACE_(iface, baseiface)                                   \
  interface DECLSPEC_NOVTABLE iface : public baseiface

typedef interface IUnknown IUnknown;

typedef /* [unique] */ IUnknown *LPUNKNOWN;

// 00000000-0000-0000-C000-000000000046
EXTERN_C const IID IID_IUnknown;

MIDL_INTERFACE("00000000-0000-0000-C000-000000000046")
IUnknown {
  virtual HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) = 0;

  virtual ULONG STDMETHODCALLTYPE AddRef(void) = 0;

  virtual ULONG STDMETHODCALLTYPE Release(void) = 0;
};

interface IStream;
interface IRecordInfo;
interface ITypeInfo;

typedef SHORT VARIANT_BOOL;
typedef LONG SCODE;

typedef union tagCY {
  struct {
#if BIGENDIAN
    LONG Hi;
    ULONG Lo;
#else
    ULONG Lo;
    LONG Hi;
#endif
  } u;
  LONGLONG int64;
} CY;

typedef WCHAR *BSTR;
typedef double DATE;

typedef struct tagDEC {
#if BIGENDIAN
  union {
    struct {
      BYTE sign;
      BYTE scale;
    } u;
    USHORT signscale;
  } u;
  USHORT wReserved;
#else
  USHORT wReserved;
  union {
    struct {
      BYTE scale;
      BYTE sign;
    } u;
    USHORT signscale;
  } u;
#endif
  ULONG Hi32;
  union {
    struct {
      ULONG Lo32;
      ULONG Mid32;
    } v;
    ULONGLONG Lo64;
  } v;
} DECIMAL;

typedef unsigned short VARTYPE;

typedef struct tagVARIANT VARIANT;

struct tagVARIANT {
  union {
    struct {
#if BIGENDIAN
      WORD wReserved1;
      VARTYPE vt;
#else
      VARTYPE vt;
      WORD wReserved1;
#endif
      WORD wReserved2;
      WORD wReserved3;
      union {
        LONGLONG llVal;
        LONG lVal;
        BYTE bVal;
        SHORT iVal;
        FLOAT fltVal;
        DOUBLE dblVal;
        VARIANT_BOOL boolVal;
        SCODE scode;
        CY cyVal;
        DATE date;
        BSTR bstrVal;
        interface IUnknown *punkVal;
        BYTE *pbVal;
        SHORT *piVal;
        LONG *plVal;
        LONGLONG *pllVal;
        FLOAT *pfltVal;
        DOUBLE *pdblVal;
        VARIANT_BOOL *pboolVal;
        SCODE *pscode;
        CY *pcyVal;
        DATE *pdate;
        BSTR *pbstrVal;
        interface IUnknown **ppunkVal;
        VARIANT *pvarVal;
        PVOID byref;
        CHAR cVal;
        USHORT uiVal;
        ULONG ulVal;
        ULONGLONG ullVal;
        INT intVal;
        UINT uintVal;
        DECIMAL *pdecVal;
        CHAR *pcVal;
        USHORT *puiVal;
        ULONG *pulVal;
        ULONGLONG *pullVal;
        INT *pintVal;
        UINT *puintVal;
        struct __tagBRECORD {
          PVOID pvRecord;
          interface IRecordInfo *pRecInfo;
        } brecVal;
      } n3;
    } n2;
    DECIMAL decVal;
  } n1;
};

#endif // _MSC_VER

#endif // LLILC_PAL

// clang-format on
