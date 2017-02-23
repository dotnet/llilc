//===---- objwriter.cpp --------------------------------*- C++ -*-===//
//
// object writer
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implementation of object writer API for JIT/AOT
///
//===----------------------------------------------------------------------===//

#include "objwriter.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/Line.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/ELF.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/Win64EH.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"

using namespace llvm;
using namespace llvm::codeview;

bool error(const Twine &Error) {
  errs() << Twine("error: ") + Error + "\n";
  return false;
}

void ObjectWriter::InitTripleName() {
  TripleName = sys::getDefaultTargetTriple();
}

Triple ObjectWriter::GetTriple() {
  Triple TheTriple(TripleName);

  if (TheTriple.getOS() == Triple::OSType::Darwin) {
    TheTriple = Triple(
        TheTriple.getArchName(), TheTriple.getVendorName(), "darwin",
        TheTriple
            .getEnvironmentName()); // it is workaround for llvm bug
                                    // https://bugs.llvm.org//show_bug.cgi?id=24927.
  }
  return TheTriple;
}

bool ObjectWriter::Init(llvm::StringRef ObjectFilePath) {
  llvm_shutdown_obj Y; // Call llvm_shutdown() on exit.

  // Initialize targets
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  MCOptions = InitMCTargetOptionsFromFlags();

  InitTripleName();
  Triple TheTriple = GetTriple();

  // Get the target specific parser.
  std::string TargetError;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(TripleName, TargetError);
  if (!TheTarget) {
    return error("Unable to create target for " + ObjectFilePath + ": " +
                 TargetError);
  }

  std::error_code EC;
  OS.reset(new raw_fd_ostream(ObjectFilePath, EC, sys::fs::F_None));
  if (EC)
    return error("Unable to create file for " + ObjectFilePath + ": " +
                 EC.message());

  MRI.reset(TheTarget->createMCRegInfo(TripleName));
  if (!MRI)
    return error("Unable to create target register info!");

  MAI.reset(TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!MAI)
    return error("Unable to create target asm info!");

  MOFI.reset(new MCObjectFileInfo);
  MC.reset(new MCContext(MAI.get(), MRI.get(), MOFI.get()));
  MOFI->InitMCObjectFileInfo(TheTriple, false, CodeModel::Default, *MC);

  std::string FeaturesStr;

  MII.reset(TheTarget->createMCInstrInfo());
  if (!MII)
    return error("no instr info info for target " + TripleName);

  std::string MCPU;

  MSTI.reset(TheTarget->createMCSubtargetInfo(TripleName, MCPU, FeaturesStr));
  if (!MSTI)
    return error("no subtarget info for target " + TripleName);

  MCE = TheTarget->createMCCodeEmitter(*MII, *MRI, *MC);
  if (!MCE)
    return error("no code emitter for target " + TripleName);

  MAB = TheTarget->createMCAsmBackend(*MRI, TripleName, MCPU, MCOptions);
  if (!MAB)
    return error("no asm backend for target " + TripleName);

  MS = TheTarget->createMCObjectStreamer(TheTriple, *MC, *MAB, *OS, MCE, *MSTI,
                                         RelaxAll,
                                         /*IncrementalLinkerCompatible*/ true,
                                         /*DWARFMustBeAtTheEnd*/ false);
  if (!MS)
    return error("no object streamer for target " + TripleName);

  TM.reset(TheTarget->createTargetMachine(TripleName, MCPU, FeaturesStr,
                                          TargetOptions(), None));
  if (!TM)
    return error("no target machine for target " + TripleName);

  Asm.reset(TheTarget->createAsmPrinter(*TM, std::unique_ptr<MCStreamer>(MS)));
  if (!Asm)
    return error("no asm printer for target " + TripleName);

  FrameOpened = false;
  FuncId = 1;

  return true;
}

void ObjectWriter::Finish() { MS->Finish(); }

void ObjectWriter::SwitchSection(const char *SectionName,
                                 CustomSectionAttributes attributes,
                                 const char *ComdatName) {
  auto &OST = *Asm->OutStreamer;
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();
  Triple TheTriple(TripleName);

  MCSection *Section = nullptr;
  if (strcmp(SectionName, "text") == 0) {
    Section = MOFI->getTextSection();
    if (!Section->hasInstructions()) {
      Section->setHasInstructions(true);
      OutContext.addGenDwarfSection(Section);
    }
  } else if (strcmp(SectionName, "data") == 0) {
    Section = MOFI->getDataSection();
  } else if (strcmp(SectionName, "rdata") == 0) {
    Section = MOFI->getReadOnlySection();
  } else if (strcmp(SectionName, "xdata") == 0) {
    Section = MOFI->getXDataSection();
  } else {
    SectionKind Kind = (attributes & CustomSectionAttributes_Executable)
                           ? SectionKind::getText()
                           : (attributes & CustomSectionAttributes_Writeable)
                                 ? SectionKind::getData()
                                 : SectionKind::getReadOnly();
    switch (TheTriple.getObjectFormat()) {
    case Triple::MachO: {
      unsigned typeAndAttributes = 0;
      if (attributes & CustomSectionAttributes_MachO_Init_Func_Pointers) {
        typeAndAttributes |= MachO::SectionType::S_MOD_INIT_FUNC_POINTERS;
      }
      Section = OutContext.getMachOSection(
          (attributes & CustomSectionAttributes_Executable) ? "__TEXT"
                                                            : "__DATA",
          SectionName, typeAndAttributes, Kind);
      break;
    }
    case Triple::COFF: {
      unsigned Characteristics = COFF::IMAGE_SCN_MEM_READ;

      if (attributes & CustomSectionAttributes_Executable) {
        Characteristics |=
            COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE;
      } else if (attributes & CustomSectionAttributes_Writeable) {
        Characteristics |=
            COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_WRITE;
      } else {
        Characteristics |= COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
      }

      if (ComdatName != nullptr) {
        Section = OutContext.getCOFFSection(
            SectionName, Characteristics | COFF::IMAGE_SCN_LNK_COMDAT, Kind,
            ComdatName, COFF::COMDATType::IMAGE_COMDAT_SELECT_ANY);
      } else {
        Section = OutContext.getCOFFSection(SectionName, Characteristics, Kind);
      }
      break;
    }
    case Triple::ELF: {
      unsigned Flags = ELF::SHF_ALLOC;
      if (ComdatName != nullptr) {
        MCSymbolELF *GroupSym =
            cast<MCSymbolELF>(OutContext.getOrCreateSymbol(ComdatName));
        OutContext.createELFGroupSection(GroupSym);
        Flags |= ELF::SHF_GROUP;
      }
      if (attributes & CustomSectionAttributes_Executable) {
        Flags |= ELF::SHF_EXECINSTR;
      } else if (attributes & CustomSectionAttributes_Writeable) {
        Flags |= ELF::SHF_WRITE;
      }
      Section =
          OutContext.getELFSection(SectionName, ELF::SHT_PROGBITS, Flags, 0,
                                   ComdatName != nullptr ? ComdatName : "");
      break;
    }
    default:
      error("Unknown output format for target " + TripleName);
      break;
    }
  }

  Sections.push_back(Section);
  OST.SwitchSection(Section);

  if (!Section->getBeginSymbol()) {
    MCSymbol *SectionStartSym = OutContext.createTempSymbol();
    OST.EmitLabel(SectionStartSym);
    Section->setBeginSymbol(SectionStartSym);
  }
}

void ObjectWriter::EmitAlignment(int ByteAlignment) {
  auto &OST = *Asm->OutStreamer;
  OST.EmitValueToAlignment(ByteAlignment, 0x90 /* Nop */);
}

void ObjectWriter::EmitBlob(int BlobSize, const char *Blob) {
  auto &OST = *Asm->OutStreamer;
  OST.EmitBytes(StringRef(Blob, BlobSize));
}

void ObjectWriter::EmitIntValue(uint64_t Value, unsigned Size) {
  auto &OST = *Asm->OutStreamer;
  OST.EmitIntValue(Value, Size);
}

void ObjectWriter::EmitSymbolDef(const char *SymbolName) {
  auto &OST = *Asm->OutStreamer;
  MCContext &OutContext = OST.getContext();

  MCSymbol *Sym = OutContext.getOrCreateSymbol(Twine(SymbolName));
  OST.EmitSymbolAttribute(Sym, MCSA_Global);
  OST.EmitLabel(Sym);
}

const MCSymbolRefExpr *
ObjectWriter::GetSymbolRefExpr(const char *SymbolName,
                               MCSymbolRefExpr::VariantKind Kind) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Create symbol reference
  MCSymbol *T = OutContext.getOrCreateSymbol(SymbolName);
  MCAssembler &MCAsm = OST.getAssembler();
  MCAsm.registerSymbol(*T);
  return MCSymbolRefExpr::create(T, Kind, OutContext);
}

int ObjectWriter::EmitSymbolRef(const char *SymbolName,
                                RelocType RelocationType, int Delta) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();

  bool IsPCRelative = false;
  int Size = 0;
  MCSymbolRefExpr::VariantKind Kind = MCSymbolRefExpr::VK_None;

  // Convert RelocationType to MCSymbolRefExpr
  switch (RelocationType) {
  case RelocType::IMAGE_REL_BASED_ABSOLUTE:
    assert(MOFI->getObjectFileType() == MOFI->IsCOFF);
    Kind = MCSymbolRefExpr::VK_COFF_IMGREL32;
    Size = 4;
    break;
  case RelocType::IMAGE_REL_BASED_HIGHLOW:
    Size = 4;
    break;
  case RelocType::IMAGE_REL_BASED_DIR64:
    Size = 8;
    break;
  case RelocType::IMAGE_REL_BASED_REL32:
    Size = 4;
    IsPCRelative = true;
    break;
  default:
    assert(false && "NYI RelocationType!");
  }

  const MCExpr *TargetExpr = GetSymbolRefExpr(SymbolName, Kind);

  if (IsPCRelative) {
    // If the fixup is pc-relative, we need to bias the value to be relative to
    // the start of the field, not the end of the field
    TargetExpr = MCBinaryExpr::createSub(
        TargetExpr, MCConstantExpr::create(Size, OutContext), OutContext);
  }

  if (Delta != 0) {
    TargetExpr = MCBinaryExpr::createAdd(
        TargetExpr, MCConstantExpr::create(Delta, OutContext), OutContext);
  }
  OST.EmitValueImpl(TargetExpr, Size, SMLoc(), IsPCRelative);
  return Size;
}

void ObjectWriter::EmitWinFrameInfo(const char *FunctionName, int StartOffset,
                                    int EndOffset, const char *BlobSymbolName) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  // .pdata emission
  MCSection *Section = MOFI->getPDataSection();

  // If the function was emitted to a Comdat section, create an associative
  // section to place the frame info in. This is due to the Windows linker
  // requirement that a function and its unwind info come from the same
  // object file.
  MCSymbol *Fn = OutContext.getOrCreateSymbol(Twine(FunctionName));
  const MCSectionCOFF *FunctionSection = cast<MCSectionCOFF>(&Fn->getSection());
  if (FunctionSection->getCharacteristics() & COFF::IMAGE_SCN_LNK_COMDAT) {
    Section = OutContext.getAssociativeCOFFSection(
        cast<MCSectionCOFF>(Section), FunctionSection->getCOMDATSymbol());
  }

  OST.SwitchSection(Section);
  OST.EmitValueToAlignment(4);

  const MCExpr *BaseRefRel =
      GetSymbolRefExpr(FunctionName, MCSymbolRefExpr::VK_COFF_IMGREL32);

  // start offset
  const MCExpr *StartOfs = MCConstantExpr::create(StartOffset, OutContext);
  OST.EmitValue(MCBinaryExpr::createAdd(BaseRefRel, StartOfs, OutContext), 4);

  // end offset
  const MCExpr *EndOfs = MCConstantExpr::create(EndOffset, OutContext);
  OST.EmitValue(MCBinaryExpr::createAdd(BaseRefRel, EndOfs, OutContext), 4);

  // frame symbol reference
  OST.EmitValue(
      GetSymbolRefExpr(BlobSymbolName, MCSymbolRefExpr::VK_COFF_IMGREL32), 4);
}

void ObjectWriter::EmitCFIStart(int Offset) {
  assert(!FrameOpened && "frame should be closed before CFIStart");
  auto &OST = *Asm->OutStreamer;

  OST.EmitCFIStartProc(false);
  FrameOpened = true;
}

void ObjectWriter::EmitCFIEnd(int Offset) {
  assert(FrameOpened && "frame should be opened before CFIEnd");
  auto &OST = *Asm->OutStreamer;
  OST.EmitCFIEndProc();
  FrameOpened = false;
}

void ObjectWriter::EmitCFILsda(const char *LsdaBlobSymbolName) {
  assert(FrameOpened && "frame should be opened before CFILsda");
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Create symbol reference
  MCSymbol *T = OutContext.getOrCreateSymbol(LsdaBlobSymbolName);
  MCAssembler &MCAsm = OST.getAssembler();
  MCAsm.registerSymbol(*T);
  OST.EmitCFILsda(T, llvm::dwarf::Constants::DW_EH_PE_pcrel |
                         llvm::dwarf::Constants::DW_EH_PE_sdata4);
}

void ObjectWriter::EmitCFICode(int Offset, const char *Blob) {
  assert(FrameOpened && "frame should be opened before CFICode");
  auto &OST = *Asm->OutStreamer;

  const CFI_CODE *CfiCode = (const CFI_CODE *)Blob;
  switch (CfiCode->CfiOpCode) {
  case CFI_ADJUST_CFA_OFFSET:
    assert(CfiCode->DwarfReg == DWARF_REG_ILLEGAL &&
           "Unexpected Register Value for OpAdjustCfaOffset");
    OST.EmitCFIAdjustCfaOffset(CfiCode->Offset);
    break;
  case CFI_REL_OFFSET:
    OST.EmitCFIRelOffset(CfiCode->DwarfReg, CfiCode->Offset);
    break;
  case CFI_DEF_CFA_REGISTER:
    assert(CfiCode->Offset == 0 &&
           "Unexpected Offset Value for OpDefCfaRegister");
    OST.EmitCFIDefCfaRegister(CfiCode->DwarfReg);
    break;
  default:
    assert(false && "Unrecognized CFI");
    break;
  }
}

void ObjectWriter::EmitLabelDiff(MCStreamer &Streamer, const MCSymbol *From,
                                 const MCSymbol *To, unsigned int Size) {
  MCSymbolRefExpr::VariantKind Variant = MCSymbolRefExpr::VK_None;
  MCContext &Context = Streamer.getContext();
  const MCExpr *FromRef = MCSymbolRefExpr::create(From, Variant, Context),
               *ToRef = MCSymbolRefExpr::create(To, Variant, Context);
  const MCExpr *AddrDelta =
      MCBinaryExpr::create(MCBinaryExpr::Sub, ToRef, FromRef, Context);
  Streamer.EmitValue(AddrDelta, Size);
}

void ObjectWriter::EmitSymRecord(MCObjectStreamer &OST, int Size,
                                 SymbolRecordKind SymbolKind) {
  RecordPrefix Rec;
  Rec.RecordLen = ulittle16_t(Size + sizeof(ulittle16_t));
  Rec.RecordKind = ulittle16_t((uint16_t)SymbolKind);
  OST.EmitBytes(StringRef((char *)&Rec, sizeof(Rec)));
}

void ObjectWriter::EmitCOFFSecRel32Value(MCObjectStreamer &OST,
                                         MCExpr const *Value) {
  MCDataFragment *DF = OST.getOrCreateDataFragment();
  MCFixup Fixup = MCFixup::create(DF->getContents().size(), Value, FK_SecRel_4);
  DF->getFixups().push_back(Fixup);
  DF->getContents().resize(DF->getContents().size() + 4, 0);
}

void ObjectWriter::EmitVarDefRange(MCObjectStreamer &OST, const MCSymbol *Fn,
                                   LocalVariableAddrRange &Range) {
  const MCSymbolRefExpr *BaseSym =
      MCSymbolRefExpr::create(Fn, OST.getContext());
  const MCExpr *Offset =
      MCConstantExpr::create(Range.OffsetStart, OST.getContext());
  const MCExpr *Expr =
      MCBinaryExpr::createAdd(BaseSym, Offset, OST.getContext());
  EmitCOFFSecRel32Value(OST, Expr);
  OST.EmitCOFFSectionIndex(Fn);
  OST.EmitIntValue(Range.Range, 2);
}

void ObjectWriter::EmitCVDebugVarInfo(MCObjectStreamer &OST, const MCSymbol *Fn,
                                      DebugVarInfo LocInfos[],
                                      int NumVarInfos) {
  for (int I = 0; I < NumVarInfos; I++) {
    // Emit an S_LOCAL record
    DebugVarInfo Var = LocInfos[I];
    TypeIndex Type = TypeIndex(Var.TypeIndex);
    LocalSymFlags Flags = LocalSymFlags::None;
    unsigned SizeofSym = sizeof(Type) + sizeof(Flags);
    unsigned NameLength = Var.Name.length() + 1;
    EmitSymRecord(OST, SizeofSym + NameLength, SymbolRecordKind::LocalSym);
    if (Var.IsParam) {
      Flags |= LocalSymFlags::IsParameter;
    }
    OST.EmitBytes(StringRef((char *)&Type, sizeof(Type)));
    OST.EmitIntValue(static_cast<uint16_t>(Flags), sizeof(Flags));
    OST.EmitBytes(StringRef(Var.Name.c_str(), NameLength));

    for (const auto &Range : Var.Ranges) {
      // Emit a range record
      switch (Range.loc.vlType) {
      case ICorDebugInfo::VLT_REG:
      case ICorDebugInfo::VLT_REG_FP: {

        // Currently only support integer registers.
        // TODO: support xmm registers
        if (Range.loc.vlReg.vlrReg >=
            sizeof(cvRegMapAmd64) / sizeof(cvRegMapAmd64[0])) {
          break;
        }
        SymbolRecordKind SymbolKind = SymbolRecordKind::DefRangeRegisterSym;
        unsigned SizeofDefRangeRegisterSym = sizeof(DefRangeRegisterSym::Hdr) +
                                             sizeof(DefRangeRegisterSym::Range);
        EmitSymRecord(OST, SizeofDefRangeRegisterSym, SymbolKind);

        DefRangeRegisterSym DefRangeRegisterSymbol(SymbolKind);
        DefRangeRegisterSymbol.Range.OffsetStart = Range.startOffset;
        DefRangeRegisterSymbol.Range.Range =
            Range.endOffset - Range.startOffset;
        DefRangeRegisterSymbol.Range.ISectStart = 0;
        DefRangeRegisterSymbol.Hdr.Register =
            cvRegMapAmd64[Range.loc.vlReg.vlrReg];
        unsigned Length = sizeof(DefRangeRegisterSymbol.Hdr);
        OST.EmitBytes(StringRef((char *)&DefRangeRegisterSymbol.Hdr, Length));
        EmitVarDefRange(OST, Fn, DefRangeRegisterSymbol.Range);
        break;
      }

      case ICorDebugInfo::VLT_STK: {

        // TODO: support REGNUM_AMBIENT_SP
        if (Range.loc.vlStk.vlsBaseReg >=
            sizeof(cvRegMapAmd64) / sizeof(cvRegMapAmd64[0])) {
          break;
        }

        assert(Range.loc.vlStk.vlsBaseReg <
                   sizeof(cvRegMapAmd64) / sizeof(cvRegMapAmd64[0]) &&
               "Register number should be in the range of [REGNUM_RAX, "
               "REGNUM_R15].");

        SymbolRecordKind SymbolKind = SymbolRecordKind::DefRangeRegisterRelSym;
        unsigned SizeofDefRangeRegisterRelSym =
            sizeof(DefRangeRegisterRelSym::Hdr) +
            sizeof(DefRangeRegisterRelSym::Range);
        EmitSymRecord(OST, SizeofDefRangeRegisterRelSym, SymbolKind);

        DefRangeRegisterRelSym DefRangeRegisterRelSymbol(SymbolKind);
        DefRangeRegisterRelSymbol.Range.OffsetStart = Range.startOffset;
        DefRangeRegisterRelSymbol.Range.Range =
            Range.endOffset - Range.startOffset;
        DefRangeRegisterRelSymbol.Range.ISectStart = 0;
        DefRangeRegisterRelSymbol.Hdr.Register =
            cvRegMapAmd64[Range.loc.vlStk.vlsBaseReg];
        DefRangeRegisterRelSymbol.Hdr.BasePointerOffset =
            Range.loc.vlStk.vlsOffset;

        unsigned Length = sizeof(DefRangeRegisterRelSymbol.Hdr);
        OST.EmitBytes(
            StringRef((char *)&DefRangeRegisterRelSymbol.Hdr, Length));
        EmitVarDefRange(OST, Fn, DefRangeRegisterRelSymbol.Range);
        break;
      }

      case ICorDebugInfo::VLT_REG_BYREF:
      case ICorDebugInfo::VLT_STK_BYREF:
      case ICorDebugInfo::VLT_REG_REG:
      case ICorDebugInfo::VLT_REG_STK:
      case ICorDebugInfo::VLT_STK_REG:
      case ICorDebugInfo::VLT_STK2:
      case ICorDebugInfo::VLT_FPSTK:
      case ICorDebugInfo::VLT_FIXED_VA:
        // TODO: for optimized debugging
        break;

      default:
        assert(false && "Unknown varloc type!");
        break;
      }
    }
  }
}

void ObjectWriter::EmitCVDebugFunctionInfo(const char *FunctionName,
                                           int FunctionSize) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();
  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  // Mark the end of function.
  MCSymbol *FnEnd = OutContext.createTempSymbol();
  OST.EmitLabel(FnEnd);

  MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
  OST.SwitchSection(Section);
  // Emit debug section magic before the first entry.
  if (FuncId == 1) {
    OST.EmitIntValue(COFF::DEBUG_SECTION_MAGIC, 4);
  }
  MCSymbol *Fn = OutContext.getOrCreateSymbol(Twine(FunctionName));

  // Emit a symbol subsection, required by VS2012+ to find function boundaries.
  MCSymbol *SymbolsBegin = OutContext.createTempSymbol(),
           *SymbolsEnd = OutContext.createTempSymbol();
  OST.EmitIntValue(unsigned(ModuleSubstreamKind::Symbols), 4);
  EmitLabelDiff(OST, SymbolsBegin, SymbolsEnd);
  OST.EmitLabel(SymbolsBegin);
  {
    ProcSym ProcSymbol(SymbolRecordKind::GlobalProcIdSym);
    ProcSymbol.CodeSize = FunctionSize;
    ProcSymbol.DbgEnd = FunctionSize;

    unsigned FunctionNameLength = strlen(FunctionName) + 1;
    unsigned HeaderSize =
        sizeof(ProcSymbol.Parent) + sizeof(ProcSymbol.End) +
        sizeof(ProcSymbol.Next) + sizeof(ProcSymbol.CodeSize) +
        sizeof(ProcSymbol.DbgStart) + sizeof(ProcSymbol.DbgEnd) +
        sizeof(ProcSymbol.FunctionType);
    unsigned SymbolSize = HeaderSize + 4 + 2 + 1 + FunctionNameLength;
    EmitSymRecord(OST, SymbolSize, SymbolRecordKind::GlobalProcIdSym);

    OST.EmitBytes(StringRef((char *)&ProcSymbol.Parent, HeaderSize));
    // Emit relocation
    OST.EmitCOFFSecRel32(Fn, 0);
    OST.EmitCOFFSectionIndex(Fn);

    // Emit flags
    OST.EmitIntValue(0, 1);

    // Emit the function display name as a null-terminated string.

    OST.EmitBytes(StringRef(FunctionName, FunctionNameLength));

    // Emit local var info
    int NumVarInfos = DebugVarInfos.size();
    if (NumVarInfos > 0) {
      EmitCVDebugVarInfo(OST, Fn, &DebugVarInfos[0], NumVarInfos);
      DebugVarInfos.clear();
    }

    // We're done with this function.
    EmitSymRecord(OST, 0, SymbolRecordKind::ProcEnd);
  }

  OST.EmitLabel(SymbolsEnd);

  // Every subsection must be aligned to a 4-byte boundary.
  OST.EmitValueToAlignment(4);

  // We have an assembler directive that takes care of the whole line table.
  // We also increase function id for the next function.
  OST.EmitCVLinetableDirective(FuncId++, Fn, FnEnd);
}

void ObjectWriter::EmitDebugFileInfo(int FileId, const char *FileName) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(FileId > 0 && "FileId should be greater than 0.");
  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    OST.EmitCVFileDirective(FileId, FileName);
  } else {
    OST.EmitDwarfFileDirective(FileId, "", FileName);
  }
}

void ObjectWriter::EmitDebugFunctionInfo(const char *FunctionName,
                                         int FunctionSize) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    OST.EmitCVFuncIdDirective(FuncId);
    EmitCVDebugFunctionInfo(FunctionName, FunctionSize);
  } else {
    // TODO: Should convert this for non-Windows.
  }
}

void ObjectWriter::EmitDebugVar(char *Name, int TypeIndex, bool IsParm,
                                int RangeCount,
                                ICorDebugInfo::NativeVarInfo *Ranges) {
  assert(RangeCount != 0);
  DebugVarInfo NewVar(Name, TypeIndex, IsParm);

  for (int I = 0; I < RangeCount; I++) {
    assert(Ranges[0].varNumber == Ranges[I].varNumber);
    NewVar.Ranges.push_back(Ranges[I]);
  }

  DebugVarInfos.push_back(NewVar);
}

void ObjectWriter::EmitDebugLoc(int NativeOffset, int FileId, int LineNumber,
                                int ColNumber) {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(FileId > 0 && "FileId should be greater than 0.");
  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    OST.EmitCVFuncIdDirective(FuncId);
    OST.EmitCVLocDirective(FuncId, FileId, LineNumber, ColNumber, false, true,
                           "", SMLoc());
  } else {
    OST.EmitDwarfLocDirective(FileId, LineNumber, ColNumber, 1, 0, 0, "");
  }
}

void ObjectWriter::EmitDebugModuleInfo() {
  auto &OST = static_cast<MCObjectStreamer &>(*Asm->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // Ensure ending all sections.
  for (auto Section : Sections) {
    OST.endSection(Section);
  }

  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
    OST.SwitchSection(Section);
    OST.EmitCVFileChecksumsDirective();
    OST.EmitCVStringTableDirective();
  } else {
    MCGenDwarfInfo::Emit(&OST);
  }
}
