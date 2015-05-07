//===----------------- include/clr/eexcp.h ----------------------*- C++ -*-===//
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
/// \brief Definition of some Exception handling structure definitions
///  used in partially interruptible GC scenarios.
///
//===----------------------------------------------------------------------===//

#ifndef __EEXCP_X__
#define __EEXCP_X__

struct EE_ILEXCEPTION_CLAUSE {
  CorExceptionFlag Flags;
  DWORD TryStartPC;
  DWORD TryEndPC;
  DWORD HandlerStartPC;
  DWORD HandlerEndPC;
  union {
    void *TypeHandle;
    mdToken ClassToken;
    DWORD FilterOffset;
  };
};

#endif // __EEXCP_X__
