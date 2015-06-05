//===----------------- include/clr/misc.h -----------------------*- C++ -*-===//
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
/// \brief Miscellaneous helper functions defined in Windows CRT
///  Explicitly defined here for use in other platforms.
///
//===----------------------------------------------------------------------===//

#ifndef _MISC_H_
#define _MISC_H_

#if !defined(_MSC_VER)

//-- Bit Manipulation --

#ifndef _rotl
inline unsigned int __cdecl _rotl(unsigned int value, int shift)
{
  unsigned int retval = 0;

  shift &= 0x1f;
  retval = (value << shift) | (value >> (sizeof(int)* 8 - shift));
  return retval;
}
#endif

#ifndef _rotr
inline unsigned int __cdecl _rotr(unsigned int value, int shift)
{
  unsigned int retval;

  shift &= 0x1f;
  retval = (value >> shift) | (value << (sizeof(int)* 8 - shift));
  return retval;
}
#endif

//-- Memory move/copy/set/cmp operations

#define RtlEqualMemory(Destination,Source,Length) (!memcmp((Destination),(Source),(Length)))
#define RtlMoveMemory(Destination,Source,Length) memmove((Destination),(Source),(Length))
#define RtlCopyMemory(Destination,Source,Length) memcpy((Destination),(Source),(Length))
#define RtlFillMemory(Destination,Length,Fill) memset((Destination),(Fill),(Length))
#define RtlZeroMemory(Destination,Length) memset((Destination),0,(Length))

#define MoveMemory RtlMoveMemory
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory


#endif // defined(MSC_VER)
#endif // _MISC_H_
