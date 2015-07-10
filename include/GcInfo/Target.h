//===---- include/gcinfo/target.h -------------------------------*- C++ -*-===//
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
/// \brief Target specific definitions for GCInfo generation
///
//===----------------------------------------------------------------------===//

#ifndef GCINFO_TARGET_H
#define GCINFO_TARGET_H

#include "global.h"

#if (defined(_TARGET_X86_) || defined(_TARGET_X64_) || defined(_TARGET_AMD64_))

// Define DWARF encodings for registers
// Size variants (ex: AL,AH,AX,EAX,RAX) all get the same Dwarf register number

#define DW_RAX 0
#define DW_RBX 3
#define DW_RCX 2
#define DW_RDX 1
#define DW_RSI 4
#define DW_RDI 5
#define DW_RBP 6
#define DW_RSP 7
#define DW_RIP 16
#define DW_R8 8
#define DW_R9 9
#define DW_R10 10
#define DW_R11 11
#define DW_R12 12
#define DW_R13 13
#define DW_R14 14
#define DW_R15 15

#define DW_FRAME_POINTER DW_RBP
#define DW_STACK_POINTER DW_RSP

#else
#error GCTables not implemented for this target
#endif // defined(_TARGET_X86_ || _TARGET_X64_ || _TARGET_AMD64_)

#endif // GCINFO_TARGET_H
