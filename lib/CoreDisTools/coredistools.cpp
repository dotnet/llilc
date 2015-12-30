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
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/DataTypes.h"

#define DllInterfaceExporter
#include "coredistools.h"

using namespace llvm;
using namespace std;

// Instruction-wise disassembler helper.
// This utility is used to implement GcStress in CoreCLr
// Adapted from LLVM-objdump

struct CorDisasm {
public:
  bool init();
  size_t disasmInstruction(size_t Address, const uint8_t *Bytes,
                           size_t Maxlength, bool PrintAsm = false) const;
  void printInstruction(const MCInst *MI, size_t Address, size_t InstSize,
                        ArrayRef<uint8_t> Bytes) const;

  CorDisasm(TargetArch Target) { TargetArch = Target; }

private:
  bool setTarget();

  TargetArch TargetArch;
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
};

bool CorDisasm::setTarget() {
  // Figure out the target triple.

  TargetTriple = sys::getDefaultTargetTriple();
  TargetTriple = Triple::normalize(TargetTriple);
  Triple TheTriple(TargetTriple);

  switch (TargetArch) {
  case Target_Host:
    switch (TheTriple.getArch()) {
    case Triple::x86:
      TargetArch = Target_X86;
      break;
    case Triple::x86_64:
      TargetArch = Target_X64;
      break;
    case Triple::thumb:
      TargetArch = Target_Thumb;
      break;
    case Triple::aarch64:
      TargetArch = Target_Arm64;
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

  assert(TargetArch != Target_Host && "Target Expected to be specific");

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
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
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

  return true;
}

size_t CorDisasm::disasmInstruction(size_t Address, const uint8_t *Bytes,
                                    size_t Maxlength, bool PrintAsm) const {
  uint64_t Size;
  uint64_t TotalSize = 0;
  MCInst Inst;
  raw_ostream &CommentStream = nulls();
  raw_ostream &DebugOut = nulls();
  ArrayRef<uint8_t> ByteArray(Bytes, Maxlength);
  bool ContinueDisasm;

  // On X86, LLVM disassembler does not handle instruction prefixes
  // correctly -- please see LLVM bug 7709.
  // The disassembler reports instruction prefixes separate from the
  // actual instruction. In order to work-around this problem, we
  // continue decoding  past the prefix bytes.
  //
  // LLVM's MCInst does not expose Opcode enumerations by design.
  // The following enumeration is a hack to use X86 opcode numbers,
  // until bug 7709 is fixed.

  enum X86Prefix {
    CS_PREFIX = 645,
    DS_PREFIX = 768,
    ES_PREFIX = 780,
    FS_PREFIX = 843,
    GS_PREFIX = 893,
    SS_PREFIX = 2775,
    DATA16_PREFIX = 694,
    LOCK_PREFIX = 1333,
    REPNE_PREFIX = 2433,
    REP_PREFIX = 2441,
    REX64_PREFIX = 2455,
    XACQUIRE_PREFIX = 14626,
    XRELEASE_PREFIX = 14699
  };

  do {

    bool success = Disassembler->getInstruction(Inst, Size, ByteArray, Address,
                                                DebugOut, CommentStream);
    TotalSize += Size;

    if (!success) {
      errs() << "Invalid instruction encoding\n";
      return 0;
    }

    if (PrintAsm) {
      printInstruction(&Inst, Address, Size, ByteArray.slice(0, Size));
    }

    ContinueDisasm = false;
    if ((TargetArch == Target_X86) || (TargetArch == Target_X64)) {
      switch (Inst.getOpcode()) {
      case CS_PREFIX:
      case DS_PREFIX:
      case ES_PREFIX:
      case FS_PREFIX:
      case GS_PREFIX:
      case SS_PREFIX:
      case DATA16_PREFIX:
      case LOCK_PREFIX:
      case REPNE_PREFIX:
      case REP_PREFIX:
      case REX64_PREFIX:
      case XACQUIRE_PREFIX:
      case XRELEASE_PREFIX:
        ContinueDisasm = true;
        Address += Size;
        ByteArray = ByteArray.slice(Size);
        break;
      default:
        break;
      }
    }
  } while (ContinueDisasm);

  return TotalSize;
}

void CorDisasm::printInstruction(const MCInst *MI, size_t Address,
                                 size_t InstSize,
                                 ArrayRef<uint8_t> Bytes) const {
  outs() << format("%8" PRIx64 ":", Address);
  outs() << "\t";
  dumpBytes(Bytes.slice(0, InstSize), outs());

  IP->printInst(MI, outs(), "", *STI);
  outs() << "\n";
}

// Implementation for CoreDisTools Interface

CorDisasm *InitDisasm(TargetArch Target) {
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
  return Disasm->disasmInstruction(Address, Bytes, Maxlength, PrintAssembly);
}
