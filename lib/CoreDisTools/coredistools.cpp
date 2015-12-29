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
  MCInst Inst;
  raw_ostream &CommentStream = nulls();
  raw_ostream &DebugOut = nulls();
  ArrayRef<uint8_t> ByteArray(Bytes, Maxlength);

  bool success = Disassembler->getInstruction(Inst, Size, ByteArray, Address,
                                              DebugOut, CommentStream);

  if (!success) {
    errs() << "Invalid instruction encoding\n";
    return 0;
  }

  if (PrintAsm) {
    printInstruction(&Inst, Address, Size, ByteArray.slice(0, Size));
  }

  return Size;
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
