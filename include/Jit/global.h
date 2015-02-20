//===--------------- include/Jit/global.h -----------------------*- C++ -*-===//
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
/// \brief Host and target defines.
///
//===----------------------------------------------------------------------===//

#ifndef JIT_GLOBAL_H
#define JIT_GLOBAL_H

#if defined(_M_IX86) || defined(__i386__)
#define _TARGET_X86_ 1
#define _HOST_X86_ 1
#elif defined(_M_X64) || defined(_M_AMD64) || defined(__amd64__)
#define _TARGET_X64_ 1
#define _TARGET_AMD64_ 1
#define _HOST_X64_ 1
#define _HOST_AMD64_ 1
#elif defined(_M_ARM) || defined(__arm__)
#define _TARGET_ARM_ 1
#define _HOST_ARM_ 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define _TARGET_ARM64_ 1
#define _HOST_ARM64_ 1
#endif

#endif // JIT_GLOBAL_H
