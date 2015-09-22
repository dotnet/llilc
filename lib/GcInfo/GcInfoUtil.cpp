//===---- include/gcinfo/gcinfoutil.cpp -------------------------*- C++ -*-===//
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
/// \brief Implementation of utility classes used by GCInfoEncoder library
///
//===----------------------------------------------------------------------===//

#include <stdint.h>
#include "GcInfoUtil.h"

//*****************************************************************************
//  GcInfoAllocator
//*****************************************************************************

int GcInfoAllocator::ZeroLengthAlloc = 0;

//*****************************************************************************
//  Utility Functions
//*****************************************************************************

//------------------------------------------------------------------------
// BitPosition: Return the position of the single bit that is set in 'value'.
//
// Return Value:
//    The position (0 is LSB) of bit that is set in 'value'
//
// Notes:
//    'value' must have exactly one bit set.
//    The algorithm is as follows:
//    - PRIME is a prime bigger than sizeof(unsigned int), which is not of the
//      form 2^n-1.
//    - Taking the modulo of 'value' with this will produce a unique hash for
//      all powers of 2 (which is what "value" is).
//    - Entries in hashTable[] which are -1 should never be used. There
//      should be PRIME-8*sizeof(value) entries which are -1 .
//------------------------------------------------------------------------

unsigned BitPosition(unsigned value) {
  _ASSERTE((value != 0) && ((value & (value - 1)) == 0));
  const unsigned PRIME = 37;

  static const int8_t hashTable[PRIME] = {
      -1, 0,  1,  26, 2,  23, 27, -1, 3, 16, 24, 30, 28, 11, -1, 13, 4,  7, 17,
      -1, 25, 22, 31, 15, 29, 10, 12, 6, -1, 21, 14, 9,  5,  20, 8,  19, 18};

  _ASSERTE(PRIME >= 8 * sizeof(value));
  _ASSERTE(sizeof(hashTable) == PRIME);

  unsigned hash = value % PRIME;
  int8_t index = hashTable[hash];
  _ASSERTE(index != -1);

  return (unsigned)index;
}

//*****************************************************************************
//  ArrayList
//*****************************************************************************

void StructArrayListBase::CreateNewChunk(SIZE_T InitialChunkLength,
                                         SIZE_T ChunkLengthGrowthFactor,
                                         SIZE_T cbElement, AllocProc *pfnAlloc,
                                         SIZE_T alignment) {
  _ASSERTE(InitialChunkLength > 0);
  _ASSERTE(ChunkLengthGrowthFactor > 0);
  _ASSERTE(cbElement > 0);

  SIZE_T cbBaseSize =
      SIZE_T(roundUp(sizeof(StructArrayListEntryBase), alignment));
  SIZE_T maxChunkCapacity = (MAXSIZE_T - cbBaseSize) / cbElement;

  _ASSERTE(maxChunkCapacity > 0);

  SIZE_T nChunkCapacity;
  if (!m_pChunkListHead)
    nChunkCapacity = InitialChunkLength;
  else
    nChunkCapacity = m_nLastChunkCapacity * ChunkLengthGrowthFactor;

  if (nChunkCapacity > maxChunkCapacity) {
    // Limit nChunkCapacity such that cbChunk computation does not overflow.
    nChunkCapacity = maxChunkCapacity;
  }

  SIZE_T cbChunk = cbBaseSize + SIZE_T(cbElement) * SIZE_T(nChunkCapacity);

  StructArrayListEntryBase *pNewChunk =
      (StructArrayListEntryBase *)pfnAlloc(this, cbChunk);

  if (m_pChunkListTail) {
    _ASSERTE(m_pChunkListHead);
    m_pChunkListTail->pNext = pNewChunk;
  } else {
    _ASSERTE(!m_pChunkListHead);
    m_pChunkListHead = pNewChunk;
  }

  pNewChunk->pNext = NULL;
  m_pChunkListTail = pNewChunk;

  m_nItemsInLastChunk = 0;
  m_nLastChunkCapacity = nChunkCapacity;
}
