//===------------------- include/Reader/gverify.h ---------------*- C++ -*-===//
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
/// \brief Declares data structures useful for MSIL bytecode verification.
///
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_GVERIFY_H
#define MSIL_READER_GVERIFY_H

#include "vtypeinfo.h"

class FlowGraphNode;

// global verification structures
typedef struct {
  FlowGraphNode *Block;
  int32_t TosIndex;
  int32_t SsaIndex;
} TOSTemp;

struct TagGlobalVerifyData;
typedef struct TagGlobalVerifyData GlobalVerifyData;

enum InitState {
  ThisUnreached = -1,
  ThisUnInit = 0,
  ThisInited = 1,
  ThisInconsistent = 2,
  ThisEHReached = 3
};

struct TagGlobalVerifyData {
  int32_t MinStack;
  int32_t MaxStack;
  int32_t NetStack;
  int32_t TOSTempsCount;
  TOSTemp *TOSTemps;
  int32_t SsaBase;

  int32_t StkDepth;
  VerType *TiStack;
  bool IsOnWorklist, BlockIsBad;
  GlobalVerifyData *WorklistPrev, *WorklistNext;
  FlowGraphNode *Block;
  InitState ThisInitialized;
  bool ContainsCtorCall;
};

#endif // MSIL_READER_GVERIFY_H
