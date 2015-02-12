//===------------------- include/Reader/jit64.h -----------------*- C++ -*-===//
//
// LLVM-MSILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
//
// Declarations and defines for things that are common to various
// users of the reader framweork.
//
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_JIT64_H
#define MSIL_READER_JIT64_H

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

#define FACILITY_JIT64 0x64 // This is a made up facility code

// Some fatal error occurred
#define JIT64_FATAL_ERROR CORJIT_INTERNALERROR
// An out of memory error occurred in the JIT64
#define JIT64_NOMEM_ERROR CORJIT_OUTOFMEM

#define JIT64_FATALEXCEPTION_CODE (0xE0000000 | FACILITY_JIT64 << 16 | 1)
#define JIT64_READEREXCEPTION_CODE (0xE0000000 | FACILITY_JIT64 << 16 | 2)

//===========================================================================

// Function: jitFilter
//
//  Filter to detect/handle internal JIT exceptions.
//  Returns EXCEPTION_EXECUTE_HANDLER for JIT64 exceptions,
//  and EXCEPTION_CONTINUE_SEARCH for all others.
//
#ifdef __cplusplus
extern "C"
#endif
    int
    jitFilter(PEXCEPTION_POINTERS ExceptionPointersPtr, LPVOID Param);
extern void _cdecl fatal(int Errnum, ...);

// Global environment config variables (set by GetConfigString).
// These are defined/set in jit.cpp.

#ifdef __cplusplus
extern "C" {
#endif

extern UINT EnvConfigCseOn;
#ifndef NDEBUG
extern UINT EnvConfigCseBinarySearch;
extern UINT EnvConfigCseMax;
extern UINT EnvConfigCopyPropMax;
extern UINT EnvConfigDeadCodeMax;
extern UINT EnvConfigCseStats;
#endif // !NDEBUG
#if !defined(CC_PEVERIFY)
extern UINT EnvConfigTailCallOpt;
#if !defined(NODEBUG)
extern UINT EnvConfigDebugVerify;
extern UINT EnvConfigTailCallMax;
#endif // !NODEBUG
#endif // !CC_PEVERIFY
extern UINT EnvConfigPInvokeInline;
extern UINT EnvConfigPInvokeCalliOpt;
extern UINT EnvConfigNewGCCalc;
extern UINT EnvConfigTurnOffDebugInfo;
extern WCHAR *EnvConfigJitName;

extern BOOL HaveEnvConfigCseOn;
extern BOOL HaveEnvConfigCseStats;
#ifndef NDEBUG
extern BOOL HaveEnvConfigCseBinarySearch;
extern BOOL HaveEnvConfigCseMax;
extern BOOL HaveEnvConfigCopyPropMax;
extern BOOL HaveEnvConfigDeadCodeMax;
#endif // !NDEBUG
#if !defined(CC_PEVERIFY)
extern BOOL HaveEnvConfigTailCallOpt;
#if !defined(NODEBUG)
extern BOOL HaveEnvConfigDebugVerify;
extern BOOL HaveEnvConfigTailCallMax;
#endif // !NODEBUG
#endif // !CC_PEVERIFY
extern BOOL HaveEnvConfigPInvokeInline;
extern BOOL HaveEnvConfigPInvokeCalliOpt;
extern BOOL HaveEnvConfigNewGCCalc;
extern BOOL HaveEnvConfigTurnOffDebugInfo;
extern BOOL HaveEnvConfigJitName;

} // extern "C"

struct JITFilterParams {
  CorJitResult *ErrorCode;
};

struct JITFilterCommonParams {
  EXCEPTION_POINTERS ExceptionPointers;
};
#endif // MSIL_READER_JIT64_H
