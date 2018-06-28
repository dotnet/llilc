//===-------- coredistools.cpp - Dissassembly tools for CoreClr -----------===//
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implementation of Disassembly Tools API for AOT/JIT
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Triple.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/DataTypes.h"
#include <stdarg.h>

#define DllInterfaceExporter
#include "coredistools.h"

using namespace llvm;
using namespace std;

class BlockIterator;

// Represents a Code block
class BlockInfo {
public:
  BlockInfo(const uint8_t *Pointer, uint64_t Size, uintptr_t Address,
            const char *BlockName = "")
      : Ptr(Pointer), BlockSize(Size), Addr(Address), Name(BlockName) {}

  bool isEmpty() const { return BlockSize == 0; }

  // A pointer to the code block to diassemble.
  const uint8_t *Ptr;
  // The size of the code block to compare.
  uint64_t BlockSize;
  // The original base address of the code block.
  uintptr_t Addr;
  // An identifying string, debug output only
  const char *Name;
};

// A block iterator reoresents a code-point within a code block.
// It represents an instruction within the block, and can move
// forward to subsequent instructions via the advance() method.
// The iterator can be in two modes:
//  Not-Decoded: Before DecodeInstruction() is called at this
//               code point
//  Decoded: After the current instrction is Decoded. In this state,
//           Inst is a valid MCInst and InstrSize is an actual non-zero
//           length of the current instruction.
class BlockIterator : public BlockInfo {
public:
  BlockIterator(const BlockInfo &Block)
      : BlockInfo(Block), Inst(), InstrSize(0), BlockStartAddr(Block.Addr) {}
  BlockIterator(const uint8_t *Pointer, uint64_t Size, uintptr_t Address = 0,
                const char *BlockName = "")
      : BlockInfo(Pointer, Size, Address, BlockName), Inst(), InstrSize(0),
        BlockStartAddr(Address) {}

  void advance() {
    assert(isDecoded() && "Cannot advance before Decode");
    assert(InstrSize <= BlockSize && "Overflow");
    Ptr += InstrSize;
    Addr += InstrSize;
    BlockSize -= InstrSize;

    // Next instruction is not yet decoded
    InstrSize = 0;
  }

  bool isDecoded() const { return (InstrSize != 0); }
  bool isBitwiseEqual(const BlockIterator &BIter) const;

  // Offset of this iterator (Instruction) wrt the beginning
  // of the Block (given at construction time)
  size_t BlockOffset() const { return Addr - BlockStartAddr; }

  // The machine instruction at this code point, after decode.
  MCInst Inst;

  // Why store the InstrSize separately, rather than obtaining
  // it from Inst.Size()? This is because of a limitation in
  // MCInst representation on certain architectures.
  // Prefix instructions on X64 such as Lock prefix are
  // encoded as an MCInst of zero size! When such a prefix is
  // decoded, the actual decode InstrSize=1, but Inst.Size()=0
  uint64_t InstrSize;

  // The original base address of the beginning of the code block
  // into which this Iterator has indexed.
  const uintptr_t BlockStartAddr;
};

// Default Print Controls
//
// The default controls simply print to stdout and stderr.
// Unfortunately, some of the CoreDisTools' clients expect the
// print messages without a trailing newline -- because the
// printing functions append the newline themselves.
// Therefore all messages are generated without the trailing
// newline. Consequently, the default printers add a newline
// at the end of the message.

void StdOut(const char *msg, ...) {
  va_list argList;
  va_start(argList, msg);
  string message = msg;
  message += "\n";
  vprintf(message.c_str(), argList);
  va_end(argList);
}
void StdErr(const char *msg, ...) {
  va_list argList;
  va_start(argList, msg);
  string message = msg;
  message += "\n";
  vfprintf(stderr, message.c_str(), argList);
  va_end(argList);
}
const PrintControl DefaultPrintControl = {StdErr, StdErr, StdOut, StdOut};

string outputBuffer;
raw_string_ostream outputStream (outputBuffer);
void BufferedOut(const char *msg, ...) {
  va_list argList;
  va_start(argList, msg);
  string message = msg;
  message += "\n";
  size_t size = vsnprintf( nullptr, 0, message.c_str(), argList ) + 1; // Extra space for '\0'
  unique_ptr<char[]> buf( new char[ size ] );
  vsnprintf( buf.get(), size, message.c_str(), argList );
  outputStream << buf.get();
  outputStream.flush();
  va_end(argList);
}
const PrintControl BufferedPrintControl = {StdErr, StdErr, StdOut, BufferedOut};

// Default Compare Controls

bool DefaultEqualityComparator(const void *UserData, size_t BlockOffset,
                               size_t InstructionLength, uint64_t Offset1,
                               uint64_t Offset2) {
  return Offset1 == Offset2;
}

// Instruction-wise disassembler helper.
// This utility is used to implement GcStress in CoreCLr
// Adapted from LLVM-objdump

struct CorDisasm {
public:
  CorDisasm(enum TargetArch Target,
            const PrintControl *PControl = &DefaultPrintControl)
      : TheTargetArch(Target), Print(PControl) {}

  bool init();
  bool decodeInstruction(BlockIterator &BIter, bool MayFail = false) const;
  uint64_t disasmInstruction(BlockIterator &BIter, bool DumpAsm = false) const;
  void dumpInstruction(const BlockIterator &BIter) const;
  void dumpBlock(const BlockInfo &Block) const;

protected:
  enum TargetArch TheTargetArch;
  const PrintControl *Print;

private:
  bool setTarget();

  string TargetTriple;
  const Target *TheTarget;

  unique_ptr<MCRegisterInfo> MRI;
  unique_ptr<const MCAsmInfo> AsmInfo;
  unique_ptr<const MCSubtargetInfo> STI;
  unique_ptr<const MCInstrInfo> MII;
  unique_ptr<const MCObjectFileInfo> MOFI;
  unique_ptr<MCContext> Ctx;
  unique_ptr<MCDisassembler> Disassembler;
  unique_ptr<MCInstPrinter> IP;

  // LLVM's MCInst does not expose Opcode enumerations by design.
  // The following enumeration is a hack to use X86 opcode numbers,
  // until bug 7709 is fixed.
  struct OpcodeMap {
    const char *Name;
    uint8_t MachineOpcode;
  };

  static const int X86NumPrefixes = 19;
  static const OpcodeMap X86Prefix[X86NumPrefixes];
};

struct CorAsmDiff : public CorDisasm {
public:
  CorAsmDiff(enum TargetArch Target,
             const PrintControl *PControl = &DefaultPrintControl,
             const OffsetComparator Comp = DefaultEqualityComparator)
      : CorDisasm(Target, PControl), Comparator(Comp) {}

  bool nearDiff(const BlockInfo &LeftBlock, const BlockInfo &RightBlock,
                const void *UserData) const;

private:
  bool fail(const char *Mesg, const BlockIterator &Left,
            const BlockIterator &Right) const;

  OffsetComparator Comparator;
};

// clang-format off
CorDisasm::OpcodeMap const CorDisasm::X86Prefix[CorDisasm::X86NumPrefixes] = {
  { "LOCK",           0xF0 },
  { "REPNE/XACQUIRE", 0xF2 }, // Both the (TSX/normal) instrs 
  { "REP/XRELEASE",   0xF3 }, // have the same byte encoding 
  { "OP_OVR",         0x66 },
  { "CS_OVR",         0x2E },
  { "DS_OVR",         0x3E },
  { "ES_OVR",         0x26 },
  { "FS_OVR",         0x64 },
  { "GS_OVR",         0x65 },
  { "SS_OVR",         0x36 },
  { "ADDR_OVR",       0x67 },
  { "REX64W",         0x48 },
  { "REX64WB",        0x49 },
  { "REX64WX",        0x4A },
  { "REX64WXB",       0x4B },
  { "REX64WR",        0x4C },
  { "REX64WRB",       0x4D },
  { "REX64WRX",       0x4E },
  { "REX64WRXB",      0x4F }
};
// clang-format on

bool CorDisasm::setTarget() {
  // Figure out the target triple.

  TargetTriple = sys::getDefaultTargetTriple();
  TargetTriple = Triple::normalize(TargetTriple);
  Triple TheTriple(TargetTriple);

  switch (TheTargetArch) {
  case Target_Host:
    switch (TheTriple.getArch()) {
    case Triple::x86:
      TheTargetArch = Target_X86;
      break;
    case Triple::x86_64:
      TheTargetArch = Target_X64;
      break;
    case Triple::thumb:
      TheTargetArch = Target_Thumb;
      break;
    case Triple::aarch64:
      TheTargetArch = Target_Arm64;
      break;
    default:
      Print->Error("Unsupported Architecture: %s\n",
                   Triple::getArchTypeName(TheTriple.getArch()));
      return false;
    }
    break;

  case Target_Thumb:
    TheTriple.setArch(Triple::thumb);
    break;
  case Target_Arm64:
    TheTriple.setArch(Triple::aarch64);
    break;
  case Target_X86:
    TheTriple.setArch(Triple::x86);
    break;
  case Target_X64:
    TheTriple.setArch(Triple::x86_64);
    break;
  default:
    Print->Error("Unsupported Architecture: %s\n",
                 Triple::getArchTypeName(TheTriple.getArch()));
    return false;
  }

  assert(TheTargetArch != Target_Host && "Target Expected to be specific");

  // Get the target specific parser.
  string Error;
  string ArchName; // Target architecture is picked up from TargetTriple.
  TheTarget = TargetRegistry::lookupTarget(ArchName, TheTriple, Error);
  if (TheTarget == nullptr) {
    Print->Error(Error.c_str());
    return false;
  }

  // Update the triple name and return the found target.
  TargetTriple = TheTriple.getTriple();
  return true;
}

bool CorDisasm::init() {
  // Call llvm_shutdown() on exit.
  llvm_shutdown_obj Y;

  // Initialize targets and assembly printers/parsers.
  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllDisassemblers();

  if (!setTarget()) {
    // setTarget() prints error message if necessary
    return false;
  }

  MRI.reset(TheTarget->createMCRegInfo(TargetTriple));
  if (!MRI) {
    Print->Error("Error: no register info for target %s\n",
                 TargetTriple.c_str());
    return false;
  }

  // Set up disassembler.
  AsmInfo.reset(TheTarget->createMCAsmInfo(*MRI, TargetTriple.c_str()));
  if (!AsmInfo) {
    Print->Error("error: no assembly info for target %s\n");
    return false;
  }

  string Mcpu;        // Not specifying any particular CPU type.
  string FeaturesStr; // No additional target specific attributes.
  STI.reset(TheTarget->createMCSubtargetInfo(TargetTriple, Mcpu, FeaturesStr));
  if (!STI) {
    Print->Error("error: no subtarget info for target %s\n",
                 TargetTriple.c_str());
    return false;
  }

  MII.reset(TheTarget->createMCInstrInfo());
  if (!MII) {
    Print->Error("error: no instruction info for target %s\n",
                 TargetTriple.c_str());
    return false;
  }

  MOFI.reset(new MCObjectFileInfo);
  Ctx.reset(new MCContext(AsmInfo.get(), MRI.get(), MOFI.get()));

  Disassembler.reset(TheTarget->createMCDisassembler(*STI, *Ctx));

  if (!Disassembler) {
    Print->Error("error: no disassembler for target %s\n",
                 TargetTriple.c_str());
    return false;
  }

  int AsmPrinterVariant;
  if ((TheTargetArch == Target_X86) || (TheTargetArch == Target_X64)) {
    // ASM printer variants:
    // 0 = ATT, 1 = Intel.
    // LLVM doesn't export this enumeration.
    AsmPrinterVariant = 1;
  } else {
    AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  }

  IP.reset(TheTarget->createMCInstPrinter(
      Triple(TargetTriple), AsmPrinterVariant, *AsmInfo, *MII, *MRI));

  if (!IP) {
    Print->Error("error: No Instruction Printer for target %s\n",
                 TargetTriple.c_str());
    return false;
  }

  return true;
}

bool CorDisasm::decodeInstruction(BlockIterator &BIter, bool MayFail) const {
  raw_ostream &CommentStream = nulls();
  raw_ostream &DebugOut = nulls();
  ArrayRef<uint8_t> ByteArray(BIter.Ptr, BIter.BlockSize);
  bool IsDecoded =
      Disassembler->getInstruction(BIter.Inst, BIter.InstrSize, ByteArray,
                                   BIter.Addr, DebugOut, CommentStream);

  if (!IsDecoded) {
    BIter.InstrSize = 0;
    if (!MayFail) {
      Print->Error("Decode Failure %s@ offset %8llx", BIter.Name, BIter.Addr);
    }
  } else {
    assert((BIter.InstrSize <= BIter.BlockSize) && "Invalid Decode");
    assert(BIter.InstrSize > 0 && "Zero Length Decode");
  }

  return IsDecoded;
}

uint64_t CorDisasm::disasmInstruction(BlockIterator &BIter,
                                      bool DumpAsm) const {
  uint64_t TotalSize = 0;
  bool ContinueDisasm;

  // On X86, LLVM disassembler does not handle instruction prefixes
  // correctly -- please see LLVM bug 7709.
  // The disassembler reports instruction prefixes separate from the
  // actual instruction. In order to work-around this problem, we
  // continue decoding  past the prefix bytes.

  do {

    if (!decodeInstruction(BIter)) {
      return 0;
    }

    uint64_t Size = BIter.InstrSize;
    TotalSize += Size;

    if (DumpAsm) {
      dumpInstruction(BIter);
    }

    ContinueDisasm = false;
    if ((TheTargetArch == Target_X86) || (TheTargetArch == Target_X64)) {

      // Check if the decoded instruction is a prefix byte, and if so,
      // continue decoding.
      if (Size == 1) {
        for (uint8_t Pfx = 0; Pfx < X86NumPrefixes; Pfx++) {
          if (BIter.Ptr[0] == X86Prefix[Pfx].MachineOpcode) {
            ContinueDisasm = true;
            BIter.advance();
            break;
          }
        }
      }
    }
  } while (ContinueDisasm);

  return TotalSize;
}

void CorDisasm::dumpInstruction(const BlockIterator &BIter) const {
  assert(BIter.isDecoded() && "Cannot print before Decode");

  uint64_t InstSize = BIter.InstrSize;
  string buffer;
  raw_string_ostream OS(buffer);

  OS << format("%8llx: ", BIter.Addr);
  dumpBytes(ArrayRef<uint8_t>(BIter.Ptr, InstSize), OS);

  if ((TheTargetArch == Target_X86) || (TheTargetArch == Target_X64)) {
    // For architectures with a variable size instruction, we pad the
    // byte dump with space up to 7 bytes. Some instructions might be longer,
    // but ...

    const char *Padding[] = {"",
                             "   ",
                             "      ",
                             "         ",
                             "            ",
                             "               ",
                             "                  "};
    OS << (Padding[(InstSize < 7) ? (7 - InstSize) : 0]);
  }

  IP->printInst(&BIter.Inst, OS, "", *STI);
  Print->Dump(OS.str().c_str());
}

void CorDisasm::dumpBlock(const BlockInfo &Block) const {
  BlockIterator BIter(Block);

  Print->Dump("-----------------------------------------------");
  Print->Dump("Block:   %s\nSize:    %lu\nAddress: %8llx\nCodePtr: %8llx",
              BIter.Name, BIter.BlockSize, BIter.Addr, BIter.Ptr);
  Print->Dump("-----------------------------------------------");

  while (!BIter.isEmpty()) {
    disasmInstruction(BIter, true);
    if (!BIter.isDecoded()) {
      break;
    }
    BIter.advance();
  }
  Print->Dump("-----------------------------------------------");
}

// Compares two code sections for syntactic equality. This is the core of the
// asm diffing logic.
//
// This function mostly relies on McInst representation of an instruction to
// compare for equality. That is, it goes through the code stream and compares,
// instruction by instruction, op code and operand values for equality.
//
// Obviously, just blindly comparing operand values will raise a lot of false
// alarms (ex: literal pointer addresses changing in the code stream).
// Therefore, this utility provides a facility where the users can provide
// custom heuristics via the OffsetComparator API -- to determine equivalency
// of mismatching operand values in order to normalize the false alarms.
//
// Notes:
//    - The core syntactic comparison is platform agnostic; we compare op codes
//      and operand values in an architecture independent way.
//    - The heuristics provided by the customer are not guaranteed to be
//      platform agnostic.
//
// Arguments:
//    Left: The first code block information
//    Right: The second code block information
//
// Return Value:
//    True if the code sections are syntactically identical; false otherwise.
//
bool CorAsmDiff::nearDiff(const BlockInfo &LeftBlock,
                          const BlockInfo &RightBlock,
                          const void *UserData) const {
  BlockIterator Left(LeftBlock);
  BlockIterator Right(RightBlock);

  if (Left.BlockSize != Right.BlockSize) {
    Print->Log("Code Size mismatch: %s=%lu, %s=%lu\n", Left.Name,
               Left.BlockSize, Right.Name, Right.BlockSize);
    return false;
  }

  while (!Left.isEmpty() && !Right.isEmpty()) {

    decodeInstruction(Left);
    decodeInstruction(Right);

    if (!Left.isDecoded() || !Right.isDecoded()) {
      return false;
    }

    if (Left.InstrSize != Right.InstrSize) {
      return fail("Instruction Size Mismatch", Left, Right);
    }

    // First, check to see if these instructions are actually identical.
    // This is done 1) to avoid the detailed comparison of the fields of InstL
    // and InstR if they are identical, and 2) because in the event that
    // there are bugs or limitations in the user-supplied heuristics,
    // we don't want to count two Instructions as diffs if they are bitwise
    // identical.
    if (Left.isBitwiseEqual(Right)) {
      // Bytes are identical
    } else {
      // Compare field-wise

      const MCInst &InstL = Left.Inst;
      const MCInst &InstR = Right.Inst;

      if (InstL.getOpcode() != InstL.getOpcode()) {
        return fail("OpCode Mismatch", Left, Right);
      }

      size_t numOperands = InstL.getNumOperands();

      if (numOperands != InstL.getNumOperands()) {
        return fail("Operand Count Mismatch", Left, Right);
      }

      for (size_t i = 0; i < numOperands; i++) {
        const MCOperand &OperandL = InstL.getOperand(i);
        const MCOperand &OperandR = InstR.getOperand(i);

        if (OperandL.isExpr() || OperandR.isExpr() || OperandL.isInst() ||
            OperandR.isInst()) {
          return fail("Unexpected Operand Kind", Left, Right);
        } else if (OperandL.isReg()) {
          if (!OperandR.isReg()) {
            return fail("Operand Kind Mismatch", Left, Right);
          }

          if (OperandL.getReg() != OperandR.getReg()) {
            return fail("Operand Register Mismatch", Left, Right);
          }
        } else if (OperandL.isFPImm()) {
          if (!OperandR.isFPImm()) {
            return fail("Operand Kind Mismatch", Left, Right);
          }

          if (OperandL.getFPImm() != OperandR.getFPImm()) {
            return fail("Operand FP value Mismatch", Left, Right);
          }
        } else if (OperandL.isImm()) {
          if (!OperandR.isImm()) {
            return fail("Operand Kind Mismatch", Left, Right);
          }

          int64_t ImmL = OperandL.getImm();
          int64_t ImmR = OperandR.getImm();

          if (ImmL == ImmR) {
            continue;
          }

          if (Comparator(UserData, Left.BlockOffset(), Left.InstrSize, ImmL,
                         ImmR)) {
            // The client somehow thinks that these offsets are equivalent
            continue;
          }

          return fail("Immediate Operand Value Mismatch", Left, Right);
        }
      }
    }

    Left.advance();
    Right.advance();
  }

  return true;
}

bool BlockIterator::isBitwiseEqual(const BlockIterator &BIter) const {
  return memcmp(this->Ptr, BIter.Ptr, this->InstrSize) == 0;
}

bool CorAsmDiff::fail(const char *Mesg, const BlockIterator &Left,
                      const BlockIterator &Right) const {
  Print->Log("%s @[%llx : %llx]", Mesg, Left.Addr, Right.Addr);
  return false;
}

// Implementation for CoreDisTools Interface

DllIface CorDisasm *InitDisasm(enum TargetArch Target) {
  return NewDisasm(Target, &DefaultPrintControl);
}

DllIface CorDisasm *InitBufferedDisasm(enum TargetArch Target) {
  return NewDisasm(Target, &BufferedPrintControl);
}

DllIface CorDisasm *NewDisasm(enum TargetArch Target,
                              const PrintControl *PControl) {
  CorDisasm *Disassembler = new CorDisasm(Target, PControl);
  if (Disassembler->init()) {
    return Disassembler;
  }

  delete Disassembler;
  return nullptr;
}

DllIface CorDisasm *InitBufferedDiffer(enum TargetArch Target,
                               const OffsetComparator Comparator) {
  return NewDiffer(Target, &BufferedPrintControl, Comparator);
}

DllIface CorAsmDiff *NewDiffer(enum TargetArch Target,
                               const PrintControl *PControl,
                               const OffsetComparator Comparator) {
  CorAsmDiff *AsmDiff = new CorAsmDiff(Target, PControl, Comparator);

  if (AsmDiff->init()) {
    return AsmDiff;
  }

  delete AsmDiff;
  return nullptr;
}

DllIface void FinishDisasm(const CorDisasm *Disasm) { delete Disasm; }

DllIface void FinishDiff(const CorAsmDiff *AsmDiff) { delete AsmDiff; }

DllIface size_t DisasmInstruction(const CorDisasm *Disasm,
                                  const uint8_t *Address, const uint8_t *Bytes,
                                  size_t Maxlength) {
  assert((Disasm != nullptr) && "Disassembler object Expected ");
  BlockIterator BIter(Bytes, Maxlength, (uintptr_t)Address);
  size_t DecodeLength = (size_t)Disasm->disasmInstruction(BIter);
  return DecodeLength;
}

DllIface size_t DumpInstruction(const CorDisasm *Disasm,
	const uint8_t *Address, const uint8_t *Bytes,
	size_t Maxlength) {
	assert((Disasm != nullptr) && "Disassembler object Expected ");
	BlockIterator BIter(Bytes, Maxlength, (uintptr_t)Address);
	size_t DecodeLength = (size_t)Disasm->disasmInstruction(BIter, true);
	return DecodeLength;
}

DllIface bool NearDiffCodeBlocks(const CorAsmDiff *AsmDiff,
                                 const void *UserData, const uint8_t *Address1,
                                 const uint8_t *Bytes1, size_t Size1,
                                 const uint8_t *Address2, const uint8_t *Bytes2,
                                 size_t Size2) {

  BlockIterator Left(Bytes1, Size1, (uintptr_t)Address1, "Left");
  BlockIterator Right(Bytes2, Size2, (uintptr_t)Address2, "Right");
  return AsmDiff->nearDiff(Left, Right, UserData);
}

DllIface void DumpCodeBlock(const CorDisasm *Disasm, const uint8_t *Address,
                            const uint8_t *Bytes, size_t Size) {
  BlockInfo Block(Bytes, Size, (uintptr_t)Address);
  Disasm->dumpBlock(Block);
}

// This API is only necessary because we don't expose in the DLL interface
// that CorAsmDiff inherits from CorDisAsm.

DllIface void DumpDiffBlocks(const CorAsmDiff *AsmDiff, const uint8_t *Address1,
                             const uint8_t *Bytes1, size_t Size1,
                             const uint8_t *Address2, const uint8_t *Bytes2,
                             size_t Size2) {

  BlockIterator Left(Bytes1, Size1, (uintptr_t)Address1, "Left");
  BlockIterator Right(Bytes2, Size2, (uintptr_t)Address2, "Right");

  AsmDiff->dumpBlock(Left);
  AsmDiff->dumpBlock(Right);
}

DllIface const char* GetOutputBuffer() {
  return outputStream.str().c_str();
}

DllIface void ClearOutputBuffer() {
  outputBuffer.clear();
}
