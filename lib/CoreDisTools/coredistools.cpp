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

  void printHeader() const;
  void dump(const CorDisasm *Disasm) const;
  // Determine if the current Block is equivalent to the input `Block`
  // according to the settings in DiffControl.
  bool nearDiffCodeBlock(const BlockInfo &Block,
                         const DiffControl &Control) const;
  bool isEmpty() const { return BlockSize == 0; }

  // A pointer to the code block to diassemble.
  const uint8_t *Ptr;
  // The size of the code block to compare.
  uint64_t BlockSize;
  // The original base address of the code block.
  uintptr_t Addr;
  // An identifying string, debug output only
  const char *Name;

private:
  bool nearDiff(const BlockInfo &Block, const DiffControl &Control) const;
  static bool diffError(const DiffControl &Control, const char *Mesg,
                        const BlockIterator &Left, const BlockIterator &Right);
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
      : BlockInfo(Block), Inst(), InstrSize(0) {}
  BlockIterator(const uint8_t *Pointer, uint64_t Size, uintptr_t Address = 0,
                const char *BlockName = "")
      : BlockInfo(Pointer, Size, Address, BlockName), Inst(), InstrSize(0) {}

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

  // Offset of this iterator (Instruction) wrt a Code Block
  size_t Offset(const BlockInfo &Block) const { return this->Ptr - Block.Ptr; }

  // The machine instruction at this code point, after decode.
  MCInst Inst;

  // Why store the InstrSize separately, rather than obtaining
  // it from Inst.Size()? This is because of a limitation in
  // MCInst representation on certain architectures.
  // Prefix instructions on X64 such as Lock prefix are
  // encoded as an MCInst of zero size! When such a prefix is
  // decoded, the actual decode InstrSize=1, but Inst.Size()=0
  uint64_t InstrSize;
};

// Instruction-wise disassembler helper.
// This utility is used to implement GcStress in CoreCLr
// Adapted from LLVM-objdump

struct CorDisasm {
public:
  CorDisasm(enum TargetArch Target) { TheTargetArch = Target; }
  bool init();
  void decodeInstruction(BlockIterator &BIter) const;
  uint64_t disasmInstruction(BlockIterator &BIter, bool PrintAsm = false) const;
  void printInstruction(const BlockIterator &BIter) const;

private:
  bool setTarget();
  bool verifyPrefixDecoding();

  enum TargetArch TheTargetArch;
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
    bool IsLlvmInstruction; // Does LLVM treat this opcode
                            // as a separate instruction?
  };

  static const int X86NumPrefixes = 19;
  static const OpcodeMap X86Prefix[X86NumPrefixes];
};

// clang-format off
CorDisasm::OpcodeMap const CorDisasm::X86Prefix[CorDisasm::X86NumPrefixes] = {
  { "LOCK",           0xF0, true },
  { "REPNE/XACQUIRE", 0xF2, true }, // Both the (TSX/normal) instrs 
  { "REP/XRELEASE",   0xF3, true }, // have the same byte encoding 
  { "OP_OVR",         0x66, true },
  { "CS_OVR",         0x2E, true },
  { "DS_OVR",         0x3E, true },
  { "ES_OVR",         0x26, true },
  { "FS_OVR",         0x64, true },
  { "GS_OVR",         0x65, true },
  { "SS_OVR",         0x36, true },
  { "ADDR_OVR",       0x67, false },
  { "REX64W",         0x48, false },
  { "REX64WB",        0x49, false },
  { "REX64WX",        0x4A, false },
  { "REX64WXB",       0x4B, false },
  { "REX64WR",        0x4C, false },
  { "REX64WRB",       0x4D, false },
  { "REX64WRX",       0x4E, false },
  { "REX64WRXB",      0x4F, false }
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
      errs() << "Unsupported Architecture"
             << Triple::getArchTypeName(TheTriple.getArch());
      return false;
    }
    break;

  case Target_Thumb:
    TheTriple.setArch(Triple::thumb);
    break;
  case Target_Arm64:
    TheTriple.setArch(Triple::aarch64);
  case Target_X86:
    TheTriple.setArch(Triple::x86);
  case Target_X64:
    TheTriple.setArch(Triple::x86_64);
  }

  assert(TheTargetArch != Target_Host && "Target Expected to be specific");

  // Get the target specific parser.
  string Error;
  string ArchName; // Target architecture is picked up from TargetTriple.
  TheTarget = TargetRegistry::lookupTarget(ArchName, TheTriple, Error);
  if (TheTarget == nullptr) {
    errs() << Error;
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
    errs() << "error: no register info for target " << TargetTriple << "\n";
    return false;
  }

  // Set up disassembler.
  AsmInfo.reset(TheTarget->createMCAsmInfo(*MRI, TargetTriple));
  if (!AsmInfo) {
    errs() << "error: no assembly info for target " << TargetTriple << "\n";
    return false;
  }

  string Mcpu;        // Not specifying any particular CPU type.
  string FeaturesStr; // No additional target specific attributes.
  STI.reset(TheTarget->createMCSubtargetInfo(TargetTriple, Mcpu, FeaturesStr));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TargetTriple << "\n";
    return false;
  }

  MII.reset(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TargetTriple << "\n";
    return false;
  }

  MOFI.reset(new MCObjectFileInfo);
  Ctx.reset(new MCContext(AsmInfo.get(), MRI.get(), MOFI.get()));

  Disassembler.reset(TheTarget->createMCDisassembler(*STI, *Ctx));

  if (!Disassembler) {
    errs() << "error: no disassembler for target " << TargetTriple << "\n";
    return false;
  }

  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  IP.reset(TheTarget->createMCInstPrinter(
      Triple(TargetTriple), AsmPrinterVariant, *AsmInfo, *MII, *MRI));

  if (!IP) {
    errs() << "error: No Instruction Printer for target " << TargetTriple
           << "\n";
    return false;
  }

  if (!verifyPrefixDecoding()) {
    // verifyOpcodes() prints error message if necessary
    return false;
  }

  return true;
}

// This function simply verifies our understanding of the way
// X86 prefix bytes are decoded by LLVM -- and learn about
// any change in behavior.
bool CorDisasm::verifyPrefixDecoding() {
  if ((TheTargetArch != Target_X86) && (TheTargetArch != Target_X64)) {
    return true;
  }

  bool Verified = true;

  for (uint8_t Pfx = 0; Pfx < X86NumPrefixes; Pfx++) {
    OpcodeMap Prefix = X86Prefix[Pfx];
    BlockIterator BIter(&Prefix.MachineOpcode, 1);

    decodeInstruction(BIter);
    if (!((BIter.isDecoded() && Prefix.IsLlvmInstruction) ||
          (!BIter.isDecoded() && !Prefix.IsLlvmInstruction))) {
      errs() << "Prefix Decode Verification failed for " << Prefix.Name << "\n";
      Verified = false;
    }
  }

  return Verified;
}

void CorDisasm::decodeInstruction(BlockIterator &BIter) const {
  raw_ostream &CommentStream = nulls();
  raw_ostream &DebugOut = nulls();
  ArrayRef<uint8_t> ByteArray(BIter.Ptr, BIter.BlockSize);
  bool IsDecoded =
      Disassembler->getInstruction(BIter.Inst, BIter.InstrSize, ByteArray,
                                   BIter.Addr, DebugOut, CommentStream);

  if (!IsDecoded) {
    BIter.InstrSize = 0;
  } else {
    assert((BIter.InstrSize <= BIter.BlockSize) && "Invalid Decode");
    assert(BIter.InstrSize > 0 && "Zero Length Decode");
  }
}

uint64_t CorDisasm::disasmInstruction(BlockIterator &BIter,
                                      bool PrintAsm) const {
  uint64_t TotalSize = 0;
  bool ContinueDisasm;

  // On X86, LLVM disassembler does not handle instruction prefixes
  // correctly -- please see LLVM bug 7709.
  // The disassembler reports instruction prefixes separate from the
  // actual instruction. In order to work-around this problem, we
  // continue decoding  past the prefix bytes.

  do {

    decodeInstruction(BIter);
    if (!BIter.isDecoded()) {
      errs() << "Decode Failure @ offset " << TotalSize;
      return 0;
    }

    uint64_t Size = BIter.InstrSize;
    TotalSize += Size;

    if (PrintAsm) {
      printInstruction(BIter);
    }

    ContinueDisasm = false;
    if ((TheTargetArch == Target_X86) || (TheTargetArch == Target_X64)) {

      // Check if the decoded instruction is a prefix byte, and if so,
      // continue decoding.
      if (Size == 1) {
        for (uint8_t Pfx = 0; Pfx < X86NumPrefixes; Pfx++) {
          if (BIter.Ptr[0] == X86Prefix[Pfx].MachineOpcode) {
            assert(X86Prefix[Pfx].IsLlvmInstruction && "Unexpected Decode");
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

void CorDisasm::printInstruction(const BlockIterator &BIter) const {
  assert(BIter.isDecoded() && "Cannot print before Decode");

  outs() << format("%8llx:", BIter.Addr);
  outs() << " ";

  uint64_t InstSize = BIter.InstrSize;
  dumpBytes(ArrayRef<uint8_t>(BIter.Ptr, InstSize), outs());
  if ((TheTargetArch == Target_X86) || (TheTargetArch == Target_X64)) {
    // For architectures with a variable size instruction, pad the
    // byte dump with space up to 7 bytes.
    // Some instructions might be longer, but ...
    while (InstSize++ < 7) {
      outs() << "   ";
    }
  }

  IP->printInst(&BIter.Inst, outs(), "", *STI);
  outs() << "\n";
}

void BlockInfo::printHeader() const {
  outs() << "--------------------------------------------------------------\n";
  outs() << "Block: " << Name << "\n";
  outs() << "Address: " << format("%8llx", Addr) << "\n";
  outs() << "Size: " << BlockSize << "\n";
  outs() << "Code Ptr: " << format("%8llx", Ptr) << "\n";
  outs() << "--------------------------------------------------------------\n";
}

void BlockInfo::dump(const CorDisasm *Disasm) const {
  BlockIterator Iter(*this);

  printHeader();

  while (!Iter.isEmpty()) {
    Disasm->disasmInstruction(Iter, true);
    if (!Iter.isDecoded()) {
      break;
    }
    Iter.advance();
  }
  outs()
      << "--------------------------------------------------------------\n\n";
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
// Therefore, this utility provides a facility via the
// DiffControl::areEquivalent() API  where the users can provide custom
// heuristics on mismatching operand values to try to normalize the false
// alarms.
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
bool BlockInfo::nearDiff(const BlockInfo &Block,
                         const DiffControl &Control) const {
  BlockIterator Left(*this);
  BlockIterator Right(Block);

  if (Left.BlockSize != Right.BlockSize) {
    errs() << "Code Size mismatch";
    return false;
  }

  const CorDisasm *Disasm = Control.Disasm;

  while (!Left.isEmpty() && !Right.isEmpty()) {

    Disasm->decodeInstruction(Left);
    Disasm->decodeInstruction(Right);

    if (!Left.isDecoded() || !Right.isDecoded()) {
      errs() << "Decode Failure @" << Left.Addr << "/" << Right.Addr << "\n";
      return false;
    }

    if (Left.InstrSize != Right.InstrSize) {
      return diffError(Control, "Instruction Size Mismatch", Left, Right);
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
        return diffError(Control, "OpCode Mismatch", Left, Right);
      }

      size_t numOperands = InstL.getNumOperands();

      if (numOperands != InstL.getNumOperands()) {
        return diffError(Control, "Operand Count Mismatch", Left, Right);
      }

      for (size_t i = 0; i < numOperands; i++) {
        const MCOperand &OperandL = InstL.getOperand(i);
        const MCOperand &OperandR = InstR.getOperand(i);

        if (OperandL.isExpr() || OperandR.isExpr() || OperandL.isInst() ||
            OperandR.isInst()) {
          return diffError(Control, "Unexpected Operand Kind", Left, Right);
        } else if (OperandL.isReg()) {
          if (!OperandR.isReg()) {
            return diffError(Control, "Operand Kind Mismatch", Left, Right);
          }

          if (OperandL.getReg() != OperandR.getReg()) {
            return diffError(Control, "Operand Register Mismatch", Left, Right);
          }
        } else if (OperandL.isFPImm()) {
          if (!OperandR.isFPImm()) {
            return diffError(Control, "Operand Kind Mismatch", Left, Right);
          }

          if (OperandL.getFPImm() != OperandR.getFPImm()) {
            return diffError(Control, "Operand FP value Mismatch", Left, Right);
          }
        } else if (OperandL.isImm()) {
          if (!OperandR.isImm()) {
            return diffError(Control, "Operand Kind Mismatch", Left, Right);
          }

          int64_t ImmL = OperandL.getImm();
          int64_t ImmR = OperandR.getImm();

          if (ImmL == ImmR) {
            continue;
          }

          if (Control.areEquivalent(Control.userData, Left.Offset(*this), ImmL,
                                    ImmR)) {
            // The client somehow thinks that these offsets are equivalent
            continue;
          }

          return diffError(Control, "Immediate Operand Value Mismatch", Left,
                           Right);
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

bool BlockInfo::nearDiffCodeBlock(const BlockInfo &Block,
                                  const DiffControl &Control) const {

  bool Success = nearDiff(Block, Control);

  if (Control.VerboseDump || (!Success && Control.DumpBlocksOnMisCompare)) {
    outs() << (Success ? "NO DIFF" : "DIFF") << "\n";
    this->dump(Control.Disasm);
    Block.dump(Control.Disasm);
  }

  return Success;
}

bool BlockInfo::diffError(const DiffControl &Control, const char *Mesg,
                          const BlockIterator &Left,
                          const BlockIterator &Right) {
  errs() << Mesg << "\n";
  Control.Disasm->printInstruction(Left);
  Control.Disasm->printInstruction(Right);
  return false;
}

// Implementation for CoreDisTools Interface

CorDisasm *InitDisasm(enum TargetArch Target) {
  CorDisasm *Disassembler = new CorDisasm(Target);
  if (Disassembler->init()) {
    return Disassembler;
  }

  delete Disassembler;
  return nullptr;
}

void FinishDisasm(const CorDisasm *Disasm) { delete Disasm; }

size_t DisasmInstruction(const CorDisasm *Disasm, size_t Address,
                         const uint8_t *Bytes, size_t Maxlength,
                         bool PrintAssembly) {
  assert((Disasm != nullptr) && "Disassembler object Expected ");
  BlockIterator BIter(Bytes, Maxlength, Address);
  size_t DecodeLength = (size_t)Disasm->disasmInstruction(BIter, PrintAssembly);
  return DecodeLength;
}

DllIface bool NearDiffCodeBlocks(const DiffControl *Control, size_t Address1,
                                 const uint8_t *Bytes1, size_t Size1,
                                 size_t Address2, const uint8_t *Bytes2,
                                 size_t Size2) {

  BlockIterator Left(Bytes1, Size1, Address1, "Left");
  BlockIterator Right(Bytes2, Size2, Address2, "Right");

  return Left.nearDiffCodeBlock(Right, *Control);
}

DllIface void PrintCodeBlock(const CorDisasm *Disasm, size_t Address,
                             const uint8_t *Bytes, size_t Size) {
  BlockInfo Block(Bytes, Size, Address);
  Block.dump(Disasm);
}
