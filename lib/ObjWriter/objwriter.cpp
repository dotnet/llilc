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

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/Line.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/MC/MCWinCOFFStreamer.h"
#include "llvm/Support/COFF.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compression.h"
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
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "cfi.h"
#include <string>
#include "jitDebugInfo.h"

using namespace llvm;
using namespace llvm::codeview;

static cl::opt<std::string>
    ArchName("arch", cl::desc("Target arch to assemble for, "
                              "see -version for available targets"));

static cl::opt<std::string>
    TripleName("triple", cl::desc("Target triple to assemble for, "
                                  "see -version for available targets"));

static cl::opt<std::string>
    MCPU("mcpu",
         cl::desc("Target a specific cpu type (-mcpu=help for details)"),
         cl::value_desc("cpu-name"), cl::init(""));

static cl::opt<Reloc::Model> RelocModel(
    "relocation-model", cl::desc("Choose relocation model"),
    cl::init(Reloc::Default),
    cl::values(
        clEnumValN(Reloc::Default, "default",
                   "Target default relocation model"),
        clEnumValN(Reloc::Static, "static", "Non-relocatable code"),
        clEnumValN(Reloc::PIC_, "pic",
                   "Fully relocatable, position independent code"),
        clEnumValN(Reloc::DynamicNoPIC, "dynamic-no-pic",
                   "Relocatable external references, non-relocatable code"),
        clEnumValEnd));

static cl::opt<llvm::CodeModel::Model> CMModel(
    "code-model", cl::desc("Choose code model"), cl::init(CodeModel::Default),
    cl::values(clEnumValN(CodeModel::Default, "default",
                          "Target default code model"),
               clEnumValN(CodeModel::Small, "small", "Small code model"),
               clEnumValN(CodeModel::Kernel, "kernel", "Kernel code model"),
               clEnumValN(CodeModel::Medium, "medium", "Medium code model"),
               clEnumValN(CodeModel::Large, "large", "Large code model"),
               clEnumValEnd));

static cl::opt<bool> SaveTempLabels("save-temp-labels",
                                    cl::desc("Don't discard temporary labels"));

static cl::opt<bool> NoExecStack("no-exec-stack",
                                 cl::desc("File doesn't need an exec stack"));

static const Target *GetTarget() {
  // Figure out the target triple.
  if (TripleName.empty())
    TripleName = sys::getDefaultTargetTriple();
  Triple TheTriple(Triple::normalize(TripleName));

  // Get the target specific parser.
  std::string Error;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(ArchName, TheTriple, Error);
  if (!TheTarget) {
    errs() << "Error: " << Error;
    return nullptr;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

bool error(const Twine &Error) {
  errs() << Twine("error: ") + Error + "\n";
  return false;
}

class ObjectWriter {
public:
  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCAsmInfo> MAI;
  std::unique_ptr<MCObjectFileInfo> MOFI;
  std::unique_ptr<MCContext> MC;
  MCAsmBackend *MAB; // Owned by MCStreamer
  std::unique_ptr<MCInstrInfo> MII;
  std::unique_ptr<MCSubtargetInfo> MSTI;
  MCCodeEmitter *MCE; // Owned by MCStreamer
  std::unique_ptr<TargetMachine> TM;
  std::unique_ptr<AsmPrinter> Asm;

  std::unique_ptr<MCAsmParser> Parser;
  std::unique_ptr<MCTargetAsmParser> TAP;

  std::unique_ptr<raw_fd_ostream> OS;
  MCTargetOptions MCOptions;
  bool FrameOpened;
  std::vector<DebugVarInfo> DebugVarInfos;

  std::map<std::string, MCSection *> CustomSections;
  std::set<MCSection *> Sections;
  int FuncId;

public:
  bool init(StringRef FunctionName);
  void finish();
  AsmPrinter &getAsmPrinter() const { return *Asm; }
  MCStreamer *MS; // Owned by AsmPrinter
  const Target *TheTarget;
};

bool ObjectWriter::init(llvm::StringRef ObjectFilePath) {
  llvm_shutdown_obj Y; // Call llvm_shutdown() on exit.

  // Initialize targets
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  MCOptions = InitMCTargetOptionsFromFlags();
  TripleName = Triple::normalize(TripleName);

  TheTarget = GetTarget();
  if (!TheTarget)
    return error("Unable to get Target");
  // Now that GetTarget() has (potentially) replaced TripleName, it's safe to
  // construct the Triple object.
  Triple TheTriple(TripleName);

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
  MOFI->InitMCObjectFileInfo(TheTriple, RelocModel, CMModel, *MC);

  std::string FeaturesStr;

  MII.reset(TheTarget->createMCInstrInfo());
  if (!MII)
    return error("no instr info info for target " + TripleName);

  MSTI.reset(TheTarget->createMCSubtargetInfo(TripleName, MCPU, FeaturesStr));
  if (!MSTI)
    return error("no subtarget info for target " + TripleName);

  MCE = TheTarget->createMCCodeEmitter(*MII, *MRI, *MC);
  if (!MCE)
    return error("no code emitter for target " + TripleName);

  MAB = TheTarget->createMCAsmBackend(*MRI, TripleName, MCPU);
  if (!MAB)
    return error("no asm backend for target " + TripleName);

  MS = TheTarget->createMCObjectStreamer(TheTriple, *MC, *MAB, *OS, MCE, *MSTI,
                                         RelaxAll,
                                         /*IncrementalLinkerCompatible*/ true,
                                         /*DWARFMustBeAtTheEnd*/ false);
  if (!MS)
    return error("no object streamer for target " + TripleName);

  TM.reset(TheTarget->createTargetMachine(TripleName, MCPU, FeaturesStr,
                                          TargetOptions()));
  if (!TM)
    return error("no target machine for target " + TripleName);

  Asm.reset(TheTarget->createAsmPrinter(*TM, std::unique_ptr<MCStreamer>(MS)));
  if (!Asm)
    return error("no asm printer for target " + TripleName);

  FrameOpened = false;
  FuncId = 1;

  return true;
}

void ObjectWriter::finish() { MS->Finish(); }

// When object writer is created/initialized successfully, it is returned.
// Or null object is returned. Client should check this.
extern "C" ObjectWriter *InitObjWriter(const char *ObjectFilePath) {
  ObjectWriter *OW = new ObjectWriter();
  if (OW->init(ObjectFilePath)) {
    return OW;
  }

  delete OW;
  return nullptr;
}

extern "C" void FinishObjWriter(ObjectWriter *OW) {
  assert(OW && "ObjWriter is null");
  OW->finish();
  delete OW;
}

enum CustomSectionAttributes : int32_t {
  CustomSectionAttributes_ReadOnly = 0x0000,
  CustomSectionAttributes_Writeable = 0x0001,
  CustomSectionAttributes_Executable = 0x0002,
  CustomSectionAttributes_MachO_Init_Func_Pointers = 0x0100,
};

extern "C" bool CreateCustomSection(ObjectWriter *OW, const char *SectionName,
                                    CustomSectionAttributes attributes,
                                    const char *ComdatName) {
  assert(OW && "ObjWriter is null");
  Triple TheTriple(TripleName);
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();

  std::string SectionNameStr(SectionName);
  assert(OW->CustomSections.find(SectionNameStr) == OW->CustomSections.end() &&
         "Section with duplicate name already exists");
  assert(ComdatName == nullptr ||
         OW->MOFI->getObjectFileType() == OW->MOFI->IsCOFF);

  MCSection *Section = nullptr;
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
        (attributes & CustomSectionAttributes_Executable) ? "__TEXT" : "__DATA",
        SectionName, typeAndAttributes, Kind);
    break;
  }
  case Triple::COFF: {
    unsigned Characteristics = COFF::IMAGE_SCN_MEM_READ;

    if (attributes & CustomSectionAttributes_Executable) {
      Characteristics |= COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE;
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
    if (attributes & CustomSectionAttributes_Executable) {
      Flags |= ELF::SHF_EXECINSTR;
    } else if (attributes & CustomSectionAttributes_Writeable) {
      Flags |= ELF::SHF_WRITE;
    }
    Section = OutContext.getELFSection(SectionName, ELF::SHT_PROGBITS, Flags);
    break;
  }
  default:
    return error("Unknown output format for target " + TripleName);
    break;
  }

  if (attributes & CustomSectionAttributes_Executable) {
    Section->setHasInstructions(true);
    OutContext.addGenDwarfSection(Section);
  }

  OW->CustomSections[SectionNameStr] = Section;
  return true;
}

extern "C" void SwitchSection(ObjectWriter *OW, const char *SectionName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

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
    std::string SectionNameStr(SectionName);
    if (OW->CustomSections.find(SectionNameStr) != OW->CustomSections.end()) {
      Section = OW->CustomSections[SectionNameStr];
    } else {
      // Add more general cases
      assert(!"Unsupported section");
    }
  }

  OW->Sections.insert(Section);
  OST.SwitchSection(Section);

  if (!Section->getBeginSymbol()) {
    MCSymbol *SectionStartSym = OutContext.createTempSymbol();
    OST.EmitLabel(SectionStartSym);
    Section->setBeginSymbol(SectionStartSym);
  }
}

extern "C" void EmitAlignment(ObjectWriter *OW, int ByteAlignment) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitValueToAlignment(ByteAlignment, 0x90 /* Nop */);
}

extern "C" void EmitBlob(ObjectWriter *OW, int BlobSize, const char *Blob) {
  assert(OW && "ObjWriter null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitBytes(StringRef(Blob, BlobSize));
}

extern "C" void EmitIntValue(ObjectWriter *OW, uint64_t Value, unsigned Size) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitIntValue(Value, Size);
}

extern "C" void EmitSymbolDef(ObjectWriter *OW, const char *SymbolName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();

  MCSymbol *Sym = OutContext.getOrCreateSymbol(Twine(SymbolName));
  OST.EmitSymbolAttribute(Sym, MCSA_Global);
  OST.EmitLabel(Sym);
}

static const MCSymbolRefExpr *
GetSymbolRefExpr(ObjectWriter *OW, const char *SymbolName,
                 MCSymbolRefExpr::VariantKind Kind = MCSymbolRefExpr::VK_None) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Create symbol reference
  MCSymbol *T = OutContext.getOrCreateSymbol(SymbolName);
  MCAssembler &MCAsm = OST.getAssembler();
  MCAsm.registerSymbol(*T);
  return MCSymbolRefExpr::create(T, Kind, OutContext);
}

enum class RelocType {
  IMAGE_REL_BASED_ABSOLUTE = 0x00,
  IMAGE_REL_BASED_HIGHLOW = 0x03,
  IMAGE_REL_BASED_DIR64 = 0x0A,
  IMAGE_REL_BASED_REL32 = 0x10,
};

extern "C" int EmitSymbolRef(ObjectWriter *OW, const char *SymbolName,
                             RelocType RelocType, int Delta) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  bool IsPCRelative = false;
  int Size = 0;
  MCSymbolRefExpr::VariantKind Kind = MCSymbolRefExpr::VK_None;

  // Convert RelocType to MCSymbolRefExpr
  switch (RelocType) {
  case RelocType::IMAGE_REL_BASED_ABSOLUTE:
    assert(OW->MOFI->getObjectFileType() == OW->MOFI->IsCOFF);
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
    assert(false && "NYI RelocType!");
  }

  const MCExpr *TargetExpr = GetSymbolRefExpr(OW, SymbolName, Kind);

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

  OST.EmitValue(TargetExpr, Size, SMLoc(), IsPCRelative);

  return Size;
}

extern "C" void EmitWinFrameInfo(ObjectWriter *OW, const char *FunctionName,
                                 int StartOffset, int EndOffset,
                                 const char *BlobSymbolName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  // .pdata emission
  MCSection *Section = MOFI->getPDataSection();
  OST.SwitchSection(Section);
  OST.EmitValueToAlignment(4);

  const MCExpr *BaseRefRel =
      GetSymbolRefExpr(OW, FunctionName, MCSymbolRefExpr::VK_COFF_IMGREL32);

  // start offset
  const MCExpr *StartOfs = MCConstantExpr::create(StartOffset, OutContext);
  OST.EmitValue(MCBinaryExpr::createAdd(BaseRefRel, StartOfs, OutContext), 4);

  // end offset
  const MCExpr *EndOfs = MCConstantExpr::create(EndOffset, OutContext);
  OST.EmitValue(MCBinaryExpr::createAdd(BaseRefRel, EndOfs, OutContext), 4);

  // frame symbol reference
  OST.EmitValue(
      GetSymbolRefExpr(OW, BlobSymbolName, MCSymbolRefExpr::VK_COFF_IMGREL32),
      4);
}

extern "C" void EmitCFIStart(ObjectWriter *OW, int Offset) {
  assert(OW && "ObjWriter is null");
  assert(!OW->FrameOpened && "frame should be closed before CFIStart");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitCFIStartProc(false);
  OW->FrameOpened = true;
}

extern "C" void EmitCFIEnd(ObjectWriter *OW, int Offset) {
  assert(OW && "ObjWriter is null");
  assert(OW->FrameOpened && "frame should be opened before CFIEnd");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

  OST.EmitCFIEndProc();
  OW->FrameOpened = false;
}

extern "C" void EmitCFILsda(ObjectWriter *OW,  const char *LsdaBlobSymbolName) {
  assert(OW && "ObjWriter is null");
  assert(OW->FrameOpened && "frame should be opened before CFILsda");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Create symbol reference
  MCSymbol *T = OutContext.getOrCreateSymbol(LsdaBlobSymbolName);
  MCAssembler &MCAsm = OST.getAssembler();
  MCAsm.registerSymbol(*T);
  OST.EmitCFILsda(T, llvm::dwarf::Constants::DW_EH_PE_pcrel |
                         llvm::dwarf::Constants::DW_EH_PE_sdata4);
}

extern "C" void EmitCFICode(ObjectWriter *OW, int Offset, const char *Blob) {
  assert(OW && "ObjWriter is null");
  assert(OW->FrameOpened && "frame should be opened before CFICode");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;

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
    assert(!"Unrecognized CFI");
    break;
  }
}

static void EmitLabelDiff(MCStreamer &Streamer, const MCSymbol *From,
                          const MCSymbol *To, unsigned int Size = 4) {
  MCSymbolRefExpr::VariantKind Variant = MCSymbolRefExpr::VK_None;
  MCContext &Context = Streamer.getContext();
  const MCExpr *FromRef = MCSymbolRefExpr::create(From, Variant, Context),
               *ToRef = MCSymbolRefExpr::create(To, Variant, Context);
  const MCExpr *AddrDelta =
      MCBinaryExpr::create(MCBinaryExpr::Sub, ToRef, FromRef, Context);
  Streamer.EmitValue(AddrDelta, Size);
}

extern "C" void EmitDebugFileInfo(ObjectWriter *OW, int FileId,
                                  const char *FileName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(FileId > 0 && "FileId should be greater than 0.");
  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    OST.EmitCVFileDirective(FileId, FileName);
  } else {
    OST.EmitDwarfFileDirective(FileId, "", FileName);
  }
}

static void EmitSymRecord(MCObjectStreamer &OST, int Size,
                          SymbolRecordKind SymbolKind) {
  SymRecord Rec = {ulittle16_t(Size + sizeof(ulittle16_t)),
                   ulittle16_t(SymbolKind)};
  OST.EmitBytes(StringRef((char *)&Rec, sizeof(Rec)));
}

static void EmitVarDefRange(MCObjectStreamer &OST, const MCSymbol *Fn,
                            LocalVariableAddrRange &Range) {
  const MCSymbolRefExpr *BaseSym =
      MCSymbolRefExpr::create(Fn, OST.getContext());
  const MCExpr *Offset =
      MCConstantExpr::create(Range.OffsetStart, OST.getContext());
  const MCExpr *Expr =
      MCBinaryExpr::createAdd(BaseSym, Offset, OST.getContext());
  OST.EmitCOFFSecRel32Value(Expr);
  OST.EmitCOFFSectionIndex(Fn);
  OST.EmitIntValue(Range.Range, 2);
}

static void EmitCVDebugVarInfo(MCObjectStreamer &OST, const MCSymbol *Fn,
                               DebugVarInfo LocInfos[], int NumVarInfos) {
  for (int I = 0; I < NumVarInfos; I++) {
    // Emit an S_LOCAL record
    DebugVarInfo Var = LocInfos[I];
    LocalSym Sym = {};

    int SizeofSym = sizeof(LocalSym);
    EmitSymRecord(OST, SizeofSym + Var.Name.length() + 1,
                  SymbolRecordKind::S_LOCAL);

    Sym.Type = TypeIndex(Var.TypeIndex);

    if (Var.IsParam) {
      Sym.Flags |= LocalSym::IsParameter;
    }

    OST.EmitBytes(StringRef((char *)&Sym, SizeofSym));
    OST.EmitBytes(StringRef(Var.Name.c_str(), Var.Name.length() + 1));

    for (const auto &Range : Var.Ranges) {
      // Emit a range record
      switch (Range.loc.vlType) {
      case ICorDebugInfo::VLT_REG:
      case ICorDebugInfo::VLT_REG_FP: {
        DefRangeRegisterSym Rec = {};
        Rec.Range.OffsetStart = Range.startOffset;
        Rec.Range.Range = Range.endOffset - Range.startOffset;
        Rec.Range.ISectStart = 0;

        // Currently only support integer registers.
        // TODO: support xmm registers
        if (Range.loc.vlReg.vlrReg >=
            sizeof(cvRegMapAmd64) / sizeof(cvRegMapAmd64[0])) {
          break;
        }

        Rec.Register = cvRegMapAmd64[Range.loc.vlReg.vlrReg];
        EmitSymRecord(OST, sizeof(DefRangeRegisterSym),
                      SymbolRecordKind::S_DEFRANGE_REGISTER);
        OST.EmitBytes(
            StringRef((char *)&Rec, offsetof(DefRangeRegisterSym, Range)));
        EmitVarDefRange(OST, Fn, Rec.Range);
        break;
      }

      case ICorDebugInfo::VLT_STK: {
        DefRangeRegisterRelSym Rec = {};
        Rec.Range.OffsetStart = Range.startOffset;
        Rec.Range.Range = Range.endOffset - Range.startOffset;
        Rec.Range.ISectStart = 0;

        // TODO: support REGNUM_AMBIENT_SP
        if (Range.loc.vlStk.vlsBaseReg >=
            sizeof(cvRegMapAmd64) / sizeof(cvRegMapAmd64[0])) {
          break;
        }

        assert(Range.loc.vlStk.vlsBaseReg <
                   sizeof(cvRegMapAmd64) / sizeof(cvRegMapAmd64[0]) &&
               "Register number should be in the range of [REGNUM_RAX, "
               "REGNUM_R15].");
        Rec.BaseRegister = cvRegMapAmd64[Range.loc.vlStk.vlsBaseReg];
        Rec.BasePointerOffset = Range.loc.vlStk.vlsOffset;
        EmitSymRecord(OST, sizeof(DefRangeRegisterRelSym),
                      SymbolRecordKind::S_DEFRANGE_REGISTER_REL);
        OST.EmitBytes(
            StringRef((char *)&Rec, offsetof(DefRangeRegisterRelSym, Range)));
        EmitVarDefRange(OST, Fn, Rec.Range);
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
        assert(!"Unknown varloc type!");
        break;
      }
    }
  }
}

static void EmitCVDebugFunctionInfo(ObjectWriter *OW, const char *FunctionName,
                                    int FunctionSize) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();
  assert(MOFI->getObjectFileType() == MOFI->IsCOFF);

  // Mark the end of function.
  MCSymbol *FnEnd = OutContext.createTempSymbol();
  OST.EmitLabel(FnEnd);

  MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
  OST.SwitchSection(Section);
  // Emit debug section magic before the first entry.
  if (OW->FuncId == 1) {
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
    int RecSize = sizeof(ProcSym) + strlen(FunctionName) + 1;
    EmitSymRecord(OST, RecSize, SymbolRecordKind::S_GPROC32_ID);

    ProcSym ProRec = {};
    ProRec.CodeSize = FunctionSize;
    ProRec.DbgEnd = FunctionSize;

    OST.EmitBytes(StringRef((char *)&ProRec, offsetof(ProcSym, CodeOffset)));

    // Emit relocation
    OST.EmitCOFFSecRel32(Fn);
    OST.EmitCOFFSectionIndex(Fn);

    // Emit flags
    OST.EmitIntValue(ProRec.Flags, sizeof(ProRec.Flags));

    // Emit the function display name as a null-terminated string.
    OST.EmitBytes(StringRef(FunctionName, strlen(FunctionName) + 1));

    // Emit local var info
    int NumVarInfos = OW->DebugVarInfos.size();
    if (NumVarInfos > 0) {
      EmitCVDebugVarInfo(OST, Fn, &OW->DebugVarInfos[0], NumVarInfos);
      OW->DebugVarInfos.clear();
    }

    // We're done with this function.
    EmitSymRecord(OST, 0, SymbolRecordKind::S_PROC_ID_END);
  }

  OST.EmitLabel(SymbolsEnd);

  // Every subsection must be aligned to a 4-byte boundary.
  OST.EmitValueToAlignment(4);

  // We have an assembler directive that takes care of the whole line table.
  // We also increase function id for the next function.
  OST.EmitCVLinetableDirective(OW->FuncId++, Fn, FnEnd);
}

extern "C" void EmitDebugFunctionInfo(ObjectWriter *OW,
                                      const char *FunctionName,
                                      int FunctionSize) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    EmitCVDebugFunctionInfo(OW, FunctionName, FunctionSize);
  } else {
    // TODO: Should convert this for non-Windows.
  }
}

extern "C" void EmitDebugVar(ObjectWriter *OW, char *Name, int TypeIndex,
                             bool IsParm, int RangeCount,
                             ICorDebugInfo::NativeVarInfo *Ranges) {
  assert(OW && "ObjWriter is null");
  assert(RangeCount != 0);
  DebugVarInfo NewVar(Name, TypeIndex, IsParm);

  for (int I = 0; I < RangeCount; I++) {
    assert(Ranges[0].varNumber == Ranges[I].varNumber);
    NewVar.Ranges.push_back(Ranges[I]);
  }

  OW->DebugVarInfos.push_back(NewVar);
}

extern "C" void EmitDebugLoc(ObjectWriter *OW, int NativeOffset, int FileId,
                             int LineNumber, int ColNumber) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  assert(FileId > 0 && "FileId should be greater than 0.");
  if (MOFI->getObjectFileType() == MOFI->IsCOFF) {
    OST.EmitCVLocDirective(OW->FuncId, FileId, LineNumber, ColNumber, false,
                           true, "");
  } else {
    OST.EmitDwarfLocDirective(FileId, LineNumber, ColNumber, 1, 0, 0, "");
  }
}

// This should be invoked at the end of module emission to finalize
// debug module info.
extern "C" void EmitDebugModuleInfo(ObjectWriter *OW) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // Ensure ending all sections.
  for (auto Section : OW->Sections) {
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
