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
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetAsmParser.h"
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
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetSubtargetInfo.h"

using namespace llvm;

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

typedef struct _DebugLocInfo {
  int NativeOffset;
  int FileId;
  int LineNumber;
  int ColNumber;
} DebugLocInfo;

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
  std::vector<DebugLocInfo> DebugLocInfos;

public:
  bool init(StringRef FunctionName);
  void finish();
  AsmPrinter &getAsmPrinter() const { return *Asm; }
  MCStreamer *MS; // Owned by AsmPrinter
  const Target *TheTarget;
};

bool ObjectWriter::init(llvm::StringRef ObjectFilePath) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
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

extern "C" void SwitchSection(ObjectWriter *OW, const char *SectionName) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = *AsmPrinter->OutStreamer;
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  MCSection *Section = nullptr;
  if (strcmp(SectionName, "text") == 0) {
    Section = MOFI->getTextSection();
  } else if (strcmp(SectionName, "data") == 0) {
    Section = MOFI->getDataSection();
  } else if (strcmp(SectionName, "rdata") == 0) {
    Section = MOFI->getReadOnlySection();
  } else {
    // Add more general cases
    assert(!"Unsupported section");
  }

  OST.SwitchSection(Section);
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

extern "C" void EmitSymbolRef(ObjectWriter *OW, const char *SymbolName,
                              int Size, bool IsPCRelative, int Delta = 0) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();

  // Get symbol reference expression
  const MCExpr *TargetExpr = GetSymbolRefExpr(OW, SymbolName);

  switch (Size) {
  case 8:
    assert(!IsPCRelative && "NYI no support for 8 byte pc-relative");
    break;
  case 4:
    // If the fixup is pc-relative, we need to bias the value to be relative to
    // the start of the field, not the end of the field
    if (IsPCRelative) {
      TargetExpr = MCBinaryExpr::createSub(
          TargetExpr, MCConstantExpr::create(Size, OutContext), OutContext);
    }
    break;
  default:
    assert(false && "NYI symbol reference size!");
  }

  if (Delta != 0) {
    TargetExpr = MCBinaryExpr::createAdd(
        TargetExpr, MCConstantExpr::create(Delta, OutContext), OutContext);
  }

  OST.EmitValue(TargetExpr, Size, SMLoc(), IsPCRelative);
}

extern "C" void EmitFrameInfo(ObjectWriter *OW, const char *FunctionName,
                              int StartOffset, int EndOffset, int BlobSize,
                              const char *BlobData) {

  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // Windows specific frame info for pdata/xdata.
  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  // .xdata emission
  MCSection *Section = MOFI->getXDataSection();
  OST.SwitchSection(Section);
  OST.EmitValueToAlignment(4);

  MCSymbol *FrameSymbol = OutContext.createTempSymbol();
  OST.EmitLabel(FrameSymbol);

  EmitBlob(OW, BlobSize, BlobData);

  // Emit personality function symbol
  // TODO: We now fake runtime as if it were C++ code.
  // Need to clean up when EH plan is concrete.
  OST.EmitValueToAlignment(4);
  const MCExpr *PersonalityFn = GetSymbolRefExpr(
      OW, "__CxxFrameHandler3", MCSymbolRefExpr::VK_COFF_IMGREL32);
  OST.EmitValue(PersonalityFn, 4);

  // .pdata emission
  Section = MOFI->getPDataSection();
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
  OST.EmitValue(MCSymbolRefExpr::create(
                    FrameSymbol, MCSymbolRefExpr::VK_COFF_IMGREL32, OutContext),
                4);
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

extern "C" void EmitDebugFileInfo(ObjectWriter *OW, int FileInfoSize,
                                  const char *FileInfos[]) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
  OST.SwitchSection(Section);
  OST.EmitIntValue(COFF::DEBUG_SECTION_MAGIC, 4);

  // This subsection holds a file index to offset in string table table.
  OST.EmitIntValue(COFF::DEBUG_INDEX_SUBSECTION, 4);
  OST.EmitIntValue(8 * FileInfoSize, 4);
  int CurrentOffset = 1;
  for (int I = 0; I < FileInfoSize; I++) {
    StringRef Filename = FileInfos[I];
    // For each unique filename, just write its offset in the string table.
    OST.EmitIntValue(CurrentOffset, 4);
    // The function name offset is not followed by any additional data.
    OST.EmitIntValue(0, 4);

    CurrentOffset += Filename.size() + 1;
  }

  // This subsection holds the string table.
  OST.EmitIntValue(COFF::DEBUG_STRING_TABLE_SUBSECTION, 4);
  OST.EmitIntValue(CurrentOffset, 4);
  // The payload starts with a null character.
  OST.EmitIntValue(0, 1);

  for (int I = 0; I < FileInfoSize; I++) {
    // Just emit unique filenames one by one, separated by a null character.
    OST.EmitBytes(FileInfos[I]);
    OST.EmitIntValue(0, 1);
  }

  // No more subsections. Fill with zeros to align the end of the section by 4.
  OST.EmitValueToAlignment(4);
}

static void EmitDebugLocInfo(ObjectWriter *OW, const char *FunctionName,
                             int FunctionSize, int NumLocInfos,
                             DebugLocInfo LocInfos[]) {

  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  MCSection *Section = MOFI->getCOFFDebugSymbolsSection();
  OST.SwitchSection(Section);

  MCSymbol *Fn = OutContext.getOrCreateSymbol(Twine(FunctionName));

  // Emit a symbol subsection, required by VS2012+ to find function boundaries.
  MCSymbol *SymbolsBegin = OutContext.createTempSymbol(),
           *SymbolsEnd = OutContext.createTempSymbol();
  OST.EmitIntValue(COFF::DEBUG_SYMBOL_SUBSECTION, 4);
  EmitLabelDiff(OST, SymbolsBegin, SymbolsEnd);
  OST.EmitLabel(SymbolsBegin);
  {
    MCSymbol *ProcSegmentBegin = OutContext.createTempSymbol(),
             *ProcSegmentEnd = OutContext.createTempSymbol();
    EmitLabelDiff(OST, ProcSegmentBegin, ProcSegmentEnd, 2);
    OST.EmitLabel(ProcSegmentBegin);

    OST.EmitIntValue(COFF::DEBUG_SYMBOL_TYPE_PROC_START, 2);
    // Some bytes of this segment don't seem to be required for basic debugging,
    // so just fill them with zeroes.
    OST.EmitFill(12, 0);
    // This is the important bit that tells the debugger where the function
    // code is located and what's its size:
    OST.EmitIntValue(FunctionSize, 4);
    OST.EmitFill(12, 0);
    OST.EmitCOFFSecRel32(Fn);
    OST.EmitCOFFSectionIndex(Fn);
    OST.EmitIntValue(0, 1);
    // Emit the function display name as a null-terminated string.
    OST.EmitBytes(FunctionName);
    OST.EmitIntValue(0, 1);
    OST.EmitLabel(ProcSegmentEnd);

    // We're done with this function.
    OST.EmitIntValue(0x0002, 2);
    OST.EmitIntValue(COFF::DEBUG_SYMBOL_TYPE_PROC_END, 2);
  }
  OST.EmitLabel(SymbolsEnd);

  // Every subsection must be aligned to a 4-byte boundary.
  OST.EmitValueToAlignment(4);

  // PCs/Instructions are grouped into segments sharing the same filename.
  // Pre-calculate the lengths (in instructions) of these segments and store
  // them in a map for convenience.  Each index in the map is the sequential
  // number of the respective instruction that starts a new segment.
  DenseMap<size_t, size_t> FilenameSegmentLengths;
  size_t LastSegmentEnd = 0;
  int PrevFileId = LocInfos[0].FileId;
  for (int J = 1; J < NumLocInfos; ++J) {
    if (PrevFileId == LocInfos[J].FileId)
      continue;
    FilenameSegmentLengths[LastSegmentEnd] = J - LastSegmentEnd;
    LastSegmentEnd = J;
    PrevFileId = LocInfos[J].FileId;
  }
  FilenameSegmentLengths[LastSegmentEnd] = NumLocInfos - LastSegmentEnd;

  // Emit a line table subsection, required to do PC-to-file:line lookup.
  OST.EmitIntValue(COFF::DEBUG_LINE_TABLE_SUBSECTION, 4);
  MCSymbol *LineTableBegin = OutContext.createTempSymbol(),
           *LineTableEnd = OutContext.createTempSymbol();
  EmitLabelDiff(OST, LineTableBegin, LineTableEnd);
  OST.EmitLabel(LineTableBegin);

  // Identify the function this subsection is for.
  OST.EmitCOFFSecRel32(Fn);
  OST.EmitCOFFSectionIndex(Fn);
  // Insert flags after a 16-bit section index.
  OST.EmitIntValue(COFF::DEBUG_LINE_TABLES_HAVE_COLUMN_RECORDS, 2);

  // Length of the function's code, in bytes.
  OST.EmitIntValue(FunctionSize, 4);

  // PC-to-linenumber lookup table:
  MCSymbol *FileSegmentEnd = nullptr;

  // The start of the last segment:
  size_t LastSegmentStart = 0;

  auto FinishPreviousChunk = [&] {
    if (!FileSegmentEnd)
      return;
    for (size_t ColSegI = LastSegmentStart,
                ColSegEnd = ColSegI + FilenameSegmentLengths[LastSegmentStart];
         ColSegI != ColSegEnd; ++ColSegI) {
      unsigned ColumnNumber = LocInfos[ColSegI].ColNumber;
      OST.EmitIntValue(ColumnNumber, 2); // Start column
      OST.EmitIntValue(ColumnNumber, 2); // End column
    }
    OST.EmitLabel(FileSegmentEnd);
  };

  for (int J = 0; J < NumLocInfos; ++J) {
    DebugLocInfo Loc = LocInfos[J];

    if (FilenameSegmentLengths.count(J)) {
      // We came to a beginning of a new filename segment.
      FinishPreviousChunk();

      MCSymbol *FileSegmentBegin = OutContext.createTempSymbol();
      OST.EmitLabel(FileSegmentBegin);
      // Each entry in string index table is 8 byte.
      OST.EmitIntValue(8 * Loc.FileId, 4);

      // Number of PC records in the lookup table.
      size_t SegmentLength = FilenameSegmentLengths[J];
      OST.EmitIntValue(SegmentLength, 4);

      // Full size of the segment for this filename, including the prev two
      // records.
      FileSegmentEnd = OutContext.createTempSymbol();
      EmitLabelDiff(OST, FileSegmentBegin, FileSegmentEnd);
      LastSegmentStart = J;
    }

    // The first PC with the given linenumber and the linenumber itself.
    OST.EmitIntValue(Loc.NativeOffset, 4);
    OST.EmitIntValue(Loc.LineNumber, 4);
  }

  FinishPreviousChunk();
  OST.EmitLabel(LineTableEnd);
}

extern "C" void EmitDebugLoc(ObjectWriter *OW, int NativeOffset, int FileId,
                             int LineNumber, int ColNumber) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  // We just aggregate locs for CodeView emission.
  DebugLocInfo Loc = {NativeOffset, FileId, LineNumber, ColNumber};
  OW->DebugLocInfos.push_back(Loc);
}

extern "C" void FlushDebugLocs(ObjectWriter *OW, const char *FunctionName,
                               int FunctionSize) {
  assert(OW && "ObjWriter is null");
  auto *AsmPrinter = &OW->getAsmPrinter();
  auto &OST = static_cast<MCObjectStreamer &>(*AsmPrinter->OutStreamer);
  MCContext &OutContext = OST.getContext();
  const MCObjectFileInfo *MOFI = OutContext.getObjectFileInfo();

  // TODO: Should convert this for non-Windows.
  if (MOFI->getObjectFileType() != MOFI->IsCOFF) {
    return;
  }

  int NumLocInfos = OW->DebugLocInfos.size();
  if (NumLocInfos > 0) {
    std::unique_ptr<DebugLocInfo[]> LocInfos(new DebugLocInfo[NumLocInfos]);
    std::copy(OW->DebugLocInfos.begin(), OW->DebugLocInfos.end(),
              LocInfos.get());

    EmitDebugLocInfo(OW, FunctionName, FunctionSize, NumLocInfos,
                     LocInfos.get());

    OW->DebugLocInfos.clear();
  }
}