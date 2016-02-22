//===--------- coredistools.h - Dissassembly tools for CoreClr
//-------------===//
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Core Disassembly Tools API for CoreClr AOT/JIT
///
//===----------------------------------------------------------------------===//

#if !defined(LLILC_TOOLS_COREDISTOOLS)
#define LLILC_TOOLS_COREDISTOOLS

#include <stdint.h>

#if defined(__cplusplus)
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif // defined(__cplusplus)

#if defined(_MSC_VER)
#if defined(DllInterfaceExporter)
#define DllIface EXTERN_C __declspec(dllexport)
#else
#define DllIface EXTERN_C __declspec(dllimport)
#endif // defined(DllInterfaceExporter)
#else
#define DllIface EXTERN_C
#endif // defined(_MSC_VER)

enum TargetArch {
  Target_Host, // Target is the same as host architecture
  Target_X86,
  Target_X64,
  Target_Thumb,
  Target_Arm64
};

struct CorDisasm;

DllIface CorDisasm *InitDisasm(enum TargetArch Target);
DllIface void FinishDisasm(const CorDisasm *Disasm);

// DisasmInstruction -- Disassemble one instruction
// Arguments:
// Address -- The address at which the bytes of the instruction
//            are intended to execute
// Bytes -- Pointer to the actual bytes which need to be disassembled
// MaxLength -- Number of bytes available in Bytes buffer
// PrintAssembly -- Whether to dump the decoded instriction to stdout
// Returns:
//   -- The Size of the disassembled instruction
//   -- Zero on failure
DllIface size_t DisasmInstruction(const CorDisasm *Disasm, size_t Address,
                                  const uint8_t *Bytes, size_t Maxlength,
                                  bool PrintAssembly);

struct DiffControl {
  // The disassembler to use for Diffing code blocks.
  const CorDisasm *Disasm;
  // Are the two offsets considered equivalent by the consumer
  bool (*areEquivalent)(const void *userData, size_t blockOffset,
                        uint64_t offset1, uint64_t offset2);
  // Always dump the code blocks compared
  bool VerboseDump;
  // Whether to dump the entire code blocks on MisCompare
  bool DumpBlocksOnMisCompare;
  // Any state that the user of this library wants to pass
  // through into areEquivalent() function.
  void *userData;
};

DllIface bool NearDiffCodeBlocks(const DiffControl *Control, size_t Address1,
                                 const uint8_t *Bytes1, size_t Size1,
                                 size_t Address2, const uint8_t *Bytes2,
                                 size_t Size2);

DllIface void PrintCodeBlock(const CorDisasm *Disasm, size_t Address,
                             const uint8_t *Bytes, size_t Size);

#endif // !defined(LLILC_TOOLS_COREDISTOOLS)
