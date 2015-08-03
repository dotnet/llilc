//===---- include/gcinfo/gcinfoutil.h ---------------------------*- C++ -*-===//
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
/// \brief Utility classes used by GCInfoEncoder library
///
//===----------------------------------------------------------------------===//

#ifndef GCINFOUTIL_H
#define GCINFOUTIL_H

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <unordered_map>

#include "global.h"
#include "LLILCPal.h"

#if defined(_MSC_VER)
#include <windows.h>
#else
#include <cstddef>
#include <cstdarg>
#include "ntimage.h"
#include "misc.h"
#endif

#include "corjit.h"
#include "eexcp.h"

//*****************************************************************************
//  Helper Macros
//*****************************************************************************

#if !defined(_ASSERTE)
#define _ASSERTE(expr) assert(expr)
#endif
#if !defined(_ASSERT)
#define _ASSERT(expr) assert(expr)
#endif

//*****************************************************************************
//  Logging Support
//*****************************************************************************

#define LF_GCINFO 1

// LL_INFO##N: can be expected to generate NH logs per small but not trival run
#define LL_EVERYTHING 10
#define LL_INFO1000000 9
#define LL_INFO100000 8
#define LL_INFO10000 7
#define LL_INFO1000 6
#define LL_INFO100 5
#define LL_INFO10 4
#define LL_WARNING 3
#define LL_ERROR 2
#define LL_FATALERROR 1
#define LL_ALWAYS 0

#define FMT_STK "sp%s0x%02x "
#define DBG_STK(off) (off >= 0) ? "+" : "-", (off >= 0) ? off : -off

#ifdef GCINFOEMITTER_LOGGING
#define LOG(x)                                                                 \
  do {                                                                         \
    gcinfo_log x;                                                              \
  } while (0)
#else
#define LOG(x)
#endif

inline void gcinfo_log(long facility, long level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
}

//*****************************************************************************
//  Utility Functions
//*****************************************************************************

inline BOOL IS_ALIGNED(size_t val, size_t alignment) {
  // alignment must be a power of 2 for this implementation to work
  // (need modulo otherwise)
  _ASSERTE(0 == (alignment & (alignment - 1)));
  return 0 == (val & (alignment - 1));
}

inline BOOL IS_ALIGNED(const void *val, size_t alignment) {
  return IS_ALIGNED((size_t)val, alignment);
}

//------------------------------------------------------------------------
// BitPosition: Return the position of the single bit that is set in 'value'.
//
// Return Value:
//    The position (0 is LSB) of bit that is set in 'value'
//------------------------------------------------------------------------

unsigned BitPosition(unsigned value);

//*****************************************************************************
//  Overflow Checking
//*****************************************************************************

template <class _Ty, _Ty _Val>
struct integral_constant { // convenient template for integral constant types
  static const _Ty value = _Val;

  typedef _Ty value_type;
  typedef integral_constant<_Ty, _Val> type;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template <class _Ty> struct remove_const { // remove top level const qualifier
  typedef _Ty type;
};

template <class _Ty>
struct remove_const<const _Ty> { // remove top level const qualifier
  typedef _Ty type;
};

template <class _Ty>
struct remove_const<const _Ty[]> { // remove top level const qualifier
  typedef _Ty type[];
};

template <class _Ty, unsigned int _Nx>
struct remove_const<const _Ty[_Nx]> { // remove top level const qualifier
  typedef _Ty type[_Nx];
};

template <bool _Test, class _Ty1, class _Ty2>
struct conditional { // type is _Ty2 for assumed !_Test
  typedef _Ty2 type;
};

template <class _Ty1, class _Ty2>
struct conditional<true, _Ty1, _Ty2> { // type is _Ty1 for _Test
  typedef _Ty1 type;
};

template <typename T>
struct is_signed
    : conditional<static_cast<typename remove_const<T>::type>(-1) < 0,
                  true_type, false_type>::type {};

template <typename Dst, typename Src> inline bool FitsIn(Src val) {
  if (is_signed<Src>::value ==
      is_signed<Dst>::value) {        // Src and Dst are equally signed
    if (sizeof(Src) <= sizeof(Dst)) { // No truncation is possible
      return true;
    } else { // Truncation is possible, requiring runtime check
      return val == (Src)((Dst)val);
    }
  } else if (is_signed<Src>::value) { // Src is signed, Dst is unsigned
#ifdef __GNUC__
    // Workaround for GCC warning: "comparison is always
    // false due to limited range of data type."
    if (!(val == 0 || val > 0))
#else
    if (val < 0)
#endif
    { // A negative number cannot be represented by an unsigned type
      return false;
    } else {
      if (sizeof(Src) <= sizeof(Dst)) { // No truncation is possible
        return true;
      } else { // Truncation is possible, requiring runtime check
        return val == (Src)((Dst)val);
      }
    }
  } else { // Src is unsigned, Dst is signed
    if (sizeof(Src) <
        sizeof(Dst)) { // No truncation is possible. Note that Src is strictly
      // smaller than Dst.
      return true;
    } else { // Truncation is possible, requiring runtime check
#ifdef __GNUC__
      // Workaround for GCC warning: "comparison is always
      // true due to limited range of data type." If in fact
      // Dst were unsigned we'd never execute this code
      // anyway.
      return ((Dst)val > 0 || (Dst)val == 0) &&
#else
      return ((Dst)val >= 0) &&
#endif
             (val == (Src)((Dst)val));
    }
  }
}

//*****************************************************************************
//  A Simple Allocator
//*****************************************************************************

class IAllocator {
public:
  virtual void *Alloc(size_t sz) = 0;

  virtual void Free(void *p) = 0;
};

class GcInfoAllocator : public IAllocator {
  static int ZeroLengthAlloc;

public:
  void *Alloc(size_t sz) {
    if (sz == 0) {
      return (void *)(&ZeroLengthAlloc);
    }

    return ::operator new(sz);
  }

  virtual void Free(void *p) {
    if (p != (void *)(&ZeroLengthAlloc)) {
      ::operator delete(p);
    }
  }
};

//*****************************************************************************
//  Quick Sort Implementation
//*****************************************************************************

template <class T> class CQuickSort {
protected:
  T *m_pBase; // Base of array to sort.
private:
  SSIZE_T m_iCount;    // How many items in array.
  SSIZE_T m_iElemSize; // Size of one element.
public:
  CQuickSort(T *pBase, // Address of first element.
             SSIZE_T iCount)
      : // How many there are.
        m_pBase(pBase),
        m_iCount(iCount), m_iElemSize(sizeof(T)) {}

  // Call to sort the array.
  inline void Sort() { SortRange(0, m_iCount - 1); }

protected:
  // Override this function to do the comparison.
  __forceinline virtual int Compare( // -1, 0, or 1
      T *psFirst,                    // First item to compare.
      T *psSecond)                   // Second item to compare.
  {
    return (memcmp(psFirst, psSecond, sizeof(T)));
  }

  __forceinline virtual void Swap(SSIZE_T iFirst, SSIZE_T iSecond) {
    if (iFirst == iSecond)
      return;
    T sTemp(m_pBase[iFirst]);
    m_pBase[iFirst] = m_pBase[iSecond];
    m_pBase[iSecond] = sTemp;
  }

private:
  inline void SortRange(SSIZE_T iLeft, SSIZE_T iRight) {
    SSIZE_T iLast;
    SSIZE_T i; // loop variable.

    for (;;) {
      // if less than two elements you're done.
      if (iLeft >= iRight)
        return;

      // ASSERT that we now have valid indicies.  This is statically provable
      // since this private function is only called with valid indicies,
      // and iLeft and iRight only converge towards eachother.  However,
      // PreFast can't detect this because it doesn't know about our callers.
      assert(iLeft >= 0 && iLeft < m_iCount);
      assert(iRight >= 0 && iRight < m_iCount);

      // The mid-element is the pivot, move it to the left.
      Swap(iLeft, (iLeft + iRight) / 2);
      iLast = iLeft;

      // move everything that is smaller than the pivot to the left.
      for (i = iLeft + 1; i <= iRight; i++) {
        if (Compare(&m_pBase[i], &m_pBase[iLeft]) < 0) {
          Swap(i, ++iLast);
        }
      }

      // Put the pivot to the point where it is in between smaller
      // and larger elements.
      Swap(iLeft, iLast);

      // Sort each partition.
      SSIZE_T iLeftLast = iLast - 1;
      SSIZE_T iRightFirst = iLast + 1;
      if (iLeftLast - iLeft < iRight - iRightFirst) {
        // Left partition is smaller, sort it recursively
        SortRange(iLeft, iLeftLast);
        // Tail call to sort the right (bigger) partition
        iLeft = iRightFirst;
        // iRight = iRight;
        continue;
      } else { // Right partition is smaller, sort it recursively
        SortRange(iRightFirst, iRight);
        // Tail call to sort the left (bigger) partition
        // iLeft = iLeft;
        iRight = iLeftLast;
        continue;
      }
    }
  }
};

//*****************************************************************************
//  SList Implementation
//*****************************************************************************

//------------------------------------------------------------------
// struct SLink, to use a singly linked list
// have a data member m_Link of type SLink in your class
// and instantiate the template SList class
//--------------------------------------------------------------------

struct SLink;
typedef struct SLink *PTR_SLink;

struct SLink {
  PTR_SLink m_pNext;
  SLink() { m_pNext = NULL; }

  void InsertAfter(SLink *pLinkToInsert) {
    _ASSERTE(NULL == pLinkToInsert->m_pNext);

    PTR_SLink pTemp = m_pNext;

    m_pNext = PTR_SLink(pLinkToInsert);
    pLinkToInsert->m_pNext = pTemp;
  }
};

//------------------------------------------------------------------
// class SList. Intrusive singly linked list.
//
// To use SList with the default instantiation, your class should
// define a data member of type SLink and named 'm_Link'. To use a
// different field name, you need to provide an explicit LinkPtr
// template argument. For example:
//   'SList<MyClass, false, MyClass*, &MyClass::m_FieldName>'
//--------------------------------------------------------------
template <class T, typename __PTR = T *, SLink T::*LinkPtr = &T::m_Link>
class SList {
protected:
  // used as sentinel
  SLink m_link; // slink.m_pNext == Null
  PTR_SLink m_pHead;
  PTR_SLink m_pTail;

  // get the list node within the object
  static SLink *GetLink(T *pLink) { return &(pLink->*LinkPtr); }

  // move to the beginning of the object given the pointer within the object
  static T *GetObject(SLink *pLink) {
    if (pLink == NULL) {
      return NULL;
    } else {
      // GCC defines offsetof to be __builtin_offsetof, which doesn't use the
      // old-school memory model trick to determine offset.
      const UINT_PTR offset =
          (((UINT_PTR) & (((T *)0x1000)->*LinkPtr)) - 0x1000);
      return (T *)__PTR((ULONG_PTR)(pLink)-offset);
    }
  }

public:
  SList() {
    m_pHead = &m_link;
    m_pTail = &m_link;
  }

  bool IsEmpty() { return m_pHead->m_pNext == NULL; }

  void InsertTail(T *pObj) {
    _ASSERTE(pObj != NULL);
    SLink *pLink = GetLink(pObj);

    m_pTail->m_pNext = pLink;
    m_pTail = pLink;
  }

  T *RemoveHead() {
    SLink *pLink = m_pHead->m_pNext;
    if (pLink != NULL) {
      m_pHead->m_pNext = pLink->m_pNext;
    }

    if (m_pTail == pLink) {
      m_pTail = m_pHead;
    }

    return GetObject(pLink);
  }

  T *GetHead() { return GetObject(m_pHead->m_pNext); }

  static T *GetNext(T *pObj) {
    _ASSERTE(pObj != NULL);
    return GetObject(GetLink(pObj)->m_pNext);
  }
};

//*****************************************************************************
//  ArrayList Implementation
//*****************************************************************************

//*****************************************************************************
// StructArrayList is an ArrayList, where the element type can be any
// arbitrary type.  Elements can only be accessed sequentially.  This is
// basically just a more efficient linked list - it's useful for accumulating
// lots of small fixed-size allocations into larger chunks, which would
// otherwise have an unnecessarily high ratio of heap overhead.
//
// The allocator provided must throw an exception on failure.
//
// The ArrayList has two properties:
// -> Creates an initial array of specified size, and creates subsequent
//    chunks of previous-size times the growth factor, up to a maximum
// -> The pointers into the ArrayList are not invalidated
//*****************************************************************************

struct StructArrayListEntryBase {
  StructArrayListEntryBase *pNext;
};

template <class ELEMENT_TYPE>
struct StructArrayListEntry : StructArrayListEntryBase {
  ELEMENT_TYPE rgItems[1];
};

class StructArrayListBase {
protected:
  typedef void *AllocProc(void *pvContext, SIZE_T cb);
  typedef void FreeProc(void *pvContext, void *pv);

  StructArrayListBase() {
    m_pChunkListHead = NULL;
    m_pChunkListTail = NULL;
    m_nTotalItems = 0;
  }

  void Destruct(FreeProc *pfnFree) {
    StructArrayListEntryBase *pList = m_pChunkListHead;
    while (pList) {
      StructArrayListEntryBase *pTrash = pList;
      pList = pList->pNext;
      pfnFree(this, pTrash);
    }
  }

  size_t roundUp(size_t size, size_t mult = sizeof(size_t)) {
    assert(mult && ((mult & (mult - 1)) == 0)); // power of two test

    return (size + (mult - 1)) & ~(mult - 1);
  }

  void CreateNewChunk(SIZE_T InitialChunkLength, SIZE_T ChunkLengthGrowthFactor,
                      SIZE_T cbElement, AllocProc *pfnAlloc, SIZE_T alignment);

  class ArrayIteratorBase {
  protected:
    void SetCurrentChunk(StructArrayListEntryBase *pChunk,
                         SIZE_T nChunkCapacity) {
      m_pCurrentChunk = pChunk;

      if (pChunk) {
        if (pChunk == m_pArrayList->m_pChunkListTail)
          m_nItemsInCurrentChunk = m_pArrayList->m_nItemsInLastChunk;
        else
          m_nItemsInCurrentChunk = nChunkCapacity;

        m_nCurrentChunkCapacity = nChunkCapacity;
      }
    }

    StructArrayListEntryBase *m_pCurrentChunk;
    SIZE_T m_nItemsInCurrentChunk;
    SIZE_T m_nCurrentChunkCapacity;
    StructArrayListBase *m_pArrayList;
  };
  friend class ArrayIteratorBase;

  StructArrayListEntryBase
      *m_pChunkListHead; // actually StructArrayListEntry<ELEMENT_TYPE>*
  StructArrayListEntryBase
      *m_pChunkListTail; // actually StructArrayListEntry<ELEMENT_TYPE>*
  SIZE_T m_nItemsInLastChunk;
  SIZE_T m_nTotalItems;
  SIZE_T m_nLastChunkCapacity;
};

template <class ELEMENT_TYPE, SIZE_T INITIAL_CHUNK_LENGTH,
          SIZE_T CHUNK_LENGTH_GROWTH_FACTOR, class ALLOCATOR>
class StructArrayList : public StructArrayListBase {
private:
  friend class ArrayIterator;

public:
  StructArrayList() {}

  ~StructArrayList() { Destruct(&ALLOCATOR::Free); }

  ELEMENT_TYPE *AppendThrowing() {
    if (!m_pChunkListTail || m_nItemsInLastChunk == m_nLastChunkCapacity)
      CreateNewChunk(INITIAL_CHUNK_LENGTH, CHUNK_LENGTH_GROWTH_FACTOR,
                     sizeof(ELEMENT_TYPE), &ALLOCATOR::Alloc,
                     __alignof(ELEMENT_TYPE));

    m_nTotalItems++;
    m_nItemsInLastChunk++;
    return &((StructArrayListEntry<ELEMENT_TYPE> *)m_pChunkListTail)
                ->rgItems[m_nItemsInLastChunk - 1];
  }

  SIZE_T Count() { return m_nTotalItems; }

  VOID CopyTo(ELEMENT_TYPE *pDest) {
    ArrayIterator iter(this);
    ELEMENT_TYPE *pSrc;
    SIZE_T nSrc;

    while ((pSrc = iter.GetNext(&nSrc))) {
      memcpy(pDest, pSrc, nSrc * sizeof(ELEMENT_TYPE));
      pDest += nSrc;
    }
  }

  ELEMENT_TYPE *GetIndex(SIZE_T index) {
    ArrayIterator iter(this);

    ELEMENT_TYPE *chunk;
    SIZE_T count;
    SIZE_T chunkBaseIndex = 0;

    while ((chunk = iter.GetNext(&count))) {
      SIZE_T nextBaseIndex = chunkBaseIndex + count;
      if (nextBaseIndex > index) {
        return (chunk + (index - chunkBaseIndex));
      }
      chunkBaseIndex = nextBaseIndex;
    }
    // Should never reach here
    assert(false);
    return NULL;
  }

  class ArrayIterator : public ArrayIteratorBase {
  public:
    ArrayIterator(StructArrayList *pArrayList) {
      m_pArrayList = pArrayList;
      SetCurrentChunk(pArrayList->m_pChunkListHead, INITIAL_CHUNK_LENGTH);
    }

    ELEMENT_TYPE *GetCurrent(SIZE_T *pnElements) {
      ELEMENT_TYPE *pRet = NULL;
      SIZE_T nElements = 0;

      if (m_pCurrentChunk) {
        pRet = &((StructArrayListEntry<ELEMENT_TYPE> *)m_pCurrentChunk)
                    ->rgItems[0];
        nElements = m_nItemsInCurrentChunk;
      }

      *pnElements = nElements;
      return pRet;
    }

    // Returns NULL when there are no more items.
    ELEMENT_TYPE *GetNext(SIZE_T *pnElements) {
      ELEMENT_TYPE *pRet = GetCurrent(pnElements);

      if (pRet)
        SetCurrentChunk(m_pCurrentChunk->pNext,
                        m_nCurrentChunkCapacity * CHUNK_LENGTH_GROWTH_FACTOR);

      return pRet;
    }
  };
};

//*****************************************************************************
//  SimplerHashTable Implementation
//*****************************************************************************

// This SimplerHashTable implementation simply uses std::unordered_map
// with the hashing nad equality functions provided.

// DefaultSimplerHashBehavior is the only behavior supported -- it simply
// follows std::unordered_map's default allocation behavior
class DefaultSimplerHashBehavior;

template <typename Key, typename KeyFuncs, typename Value, typename Behavior>
class SimplerHashTable {
private:
  class Hasher {
  public:
    size_t operator()(Key const &key) const {
      return KeyFuncs::GetHashCode(key);
    }
  };

  class Comparer {
  public:
    bool operator()(Key const &key1, Key const &key2) const {
      return KeyFuncs::Equals(key1, key2);
    }
  };

  typedef std::unordered_map<Key, Value, Hasher, Comparer> HashMap;
  HashMap payload;

public:
  SimplerHashTable(IAllocator *allocator) : payload() {}

  bool Lookup(Key key, Value *pVal) {
    typename HashMap::const_iterator iterator = payload.find(key);

    if (iterator != payload.end()) {
      if (pVal != NULL) {
        *pVal = iterator->second;
      }
      return true;
    }

    return false;
  }

  bool Set(Key key, Value value) {
    bool preExisting = Lookup(key, NULL);
    payload[key] = value;
    return preExisting;
  }

  class KeyIterator {
    friend class SimplerHashTable;

  private:
    typename HashMap::iterator keyIterator;

  public:
    KeyIterator(typename HashMap::iterator kIterator) {
      keyIterator = kIterator;
    }

    const Key &Get() const { return keyIterator->first; }

    const Value &GetValue() const { return keyIterator->second; }

    void SetValue(const Value &value) const { keyIterator->second = value; }

    void Next() { keyIterator++; }

    bool Equal(KeyIterator kIterator) {
      return (keyIterator == kIterator.keyIterator);
    }
  };

  KeyIterator Begin() { return KeyIterator(payload.begin()); }

  KeyIterator End() { return KeyIterator(payload.end()); }
};

#endif // GCINFOUTIL_H
