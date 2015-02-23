//===--------------- include/Jit/jitpch.h -----------------------*- C++ -*-===//
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
/// \brief Jit precompiled header
///
//===----------------------------------------------------------------------===//

#ifndef JIT_PCH_H
#define JIT_PCH_H

#include "global.h"
#include "LLILCPal.h"

#if defined(_MSC_VER)
#include <windows.h>
#else
#include <cstddef>
#include <cstdarg>
#include "ntimage.h"
#endif

#include "corjit.h"

#endif // JIT_PCH_H
