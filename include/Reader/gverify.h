//===------------------- include/Reader/gverify.h ---------------*- C++ -*-===//
//
// LLVM-MSILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
//
// Declares data structures useful for MSIL bytecode verification.
//
//===----------------------------------------------------------------------===//

#ifndef MSIL_READER_GVERIFY_H
#define MSIL_READER_GVERIFY_H

#include "vtypeinfo.h"

class FlowGraphNode;

// global verification structures
typedef struct {
  FlowGraphNode *Block;
  int TosIndex;
  int SsaIndex;
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
  int MinStack;
  int MaxStack;
  int NetStack;
  int TOSTempsCount;
  TOSTemp *TOSTemps;
  int SsaBase;

  int StkDepth;
  VerType *TiStack;
  bool IsOnWorklist, BlockIsBad;
  GlobalVerifyData *WorklistPrev, *WorklistNext;
  FlowGraphNode *Block;
  InitState ThisInitialized;
  bool ContainsCtorCall;
};

#endif // MSIL_READER_GVERIFY_H
