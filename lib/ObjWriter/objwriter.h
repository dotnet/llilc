//===---- objwriter.h --------------------------------*- C++ -*-===//
//
// object writer
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/Target/TargetOptions.h"

#include "cfi.h"
#include "jitDebugInfo.h"
#include <string>

using namespace llvm;
using namespace llvm::codeview;

enum CustomSectionAttributes : int32_t {
  CustomSectionAttributes_ReadOnly = 0x0000,
  CustomSectionAttributes_Writeable = 0x0001,
  CustomSectionAttributes_Executable = 0x0002,
  CustomSectionAttributes_MachO_Init_Func_Pointers = 0x0100,
};

enum class RelocType {
  IMAGE_REL_BASED_ABSOLUTE = 0x00,
  IMAGE_REL_BASED_HIGHLOW = 0x03,
  IMAGE_REL_BASED_DIR64 = 0x0A,
  IMAGE_REL_BASED_REL32 = 0x10,
};

class ObjectWriter {
public:
  bool Init(StringRef FunctionName);
  void Finish();

  void SwitchSection(const char *SectionName,
                     CustomSectionAttributes attributes,
                     const char *ComdatName);

  void EmitAlignment(int ByteAlignment);
  void EmitBlob(int BlobSize, const char *Blob);
  void EmitIntValue(uint64_t Value, unsigned Size);
  void EmitSymbolDef(const char *SymbolName);
  void EmitWinFrameInfo(const char *FunctionName, int StartOffset,
                        int EndOffset, const char *BlobSymbolName);
  int EmitSymbolRef(const char *SymbolName, RelocType RelocType, int Delta);

  void EmitDebugFileInfo(int FileId, const char *FileName);
  void EmitDebugFunctionInfo(const char *FunctionName, int FunctionSize);
  void EmitDebugVar(char *Name, int TypeIndex, bool IsParm, int RangeCount,
                    ICorDebugInfo::NativeVarInfo *Ranges);
  void EmitDebugLoc(int NativeOffset, int FileId, int LineNumber,
                    int ColNumber);
  void EmitDebugModuleInfo();

  void EmitCFIStart(int Offset);
  void EmitCFIEnd(int Offset);
  void EmitCFILsda(const char *LsdaBlobSymbolName);
  void EmitCFICode(int Offset, const char *Blob);

private:
  void EmitLabelDiff(MCStreamer &Streamer, const MCSymbol *From,
                     const MCSymbol *To, unsigned int Size = 4);
  void EmitSymRecord(MCObjectStreamer &OST, int Size,
                     SymbolRecordKind SymbolKind);
  void EmitCOFFSecRel32Value(MCObjectStreamer &OST, MCExpr const *Value);

  void EmitVarDefRange(MCObjectStreamer &OST, const MCSymbol *Fn,
                       LocalVariableAddrRange &Range);
  void EmitCVDebugVarInfo(MCObjectStreamer &OST, const MCSymbol *Fn,
                          DebugVarInfo LocInfos[], int NumVarInfos);
  void EmitCVDebugFunctionInfo(const char *FunctionName, int FunctionSize);

  const MCSymbolRefExpr *GetSymbolRefExpr(
      const char *SymbolName,
      MCSymbolRefExpr::VariantKind Kind = MCSymbolRefExpr::VK_None);

  void InitTripleName();
  Triple GetTriple();

private:
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

  std::unique_ptr<raw_fd_ostream> OS;
  MCTargetOptions MCOptions;
  bool FrameOpened;
  std::vector<DebugVarInfo> DebugVarInfos;

  std::list<MCSection *> Sections;
  int FuncId;

  std::string TripleName;

  MCStreamer *MS; // Owned by AsmPrinter
};

// When object writer is created/initialized successfully, it is returned.
// Or null object is returned. Client should check this.
extern "C" ObjectWriter *InitObjWriter(const char *ObjectFilePath) {
  ObjectWriter *OW = new ObjectWriter();
  if (OW->Init(ObjectFilePath)) {
    return OW;
  }
  delete OW;
  return nullptr;
}

extern "C" void FinishObjWriter(ObjectWriter *OW) {
  assert(OW && "ObjWriter is null");
  OW->Finish();
  delete OW;
}

extern "C" void SwitchSection(ObjectWriter *OW, const char *SectionName,
                              CustomSectionAttributes attributes,
                              const char *ComdatName) {
  assert(OW && "ObjWriter is null");
  OW->SwitchSection(SectionName, attributes, ComdatName);
}

extern "C" void EmitAlignment(ObjectWriter *OW, int ByteAlignment) {
  assert(OW && "ObjWriter is null");
  OW->EmitAlignment(ByteAlignment);
}

extern "C" void EmitBlob(ObjectWriter *OW, int BlobSize, const char *Blob) {
  assert(OW && "ObjWriter null");
  OW->EmitBlob(BlobSize, Blob);
}

extern "C" void EmitIntValue(ObjectWriter *OW, uint64_t Value, unsigned Size) {
  assert(OW && "ObjWriter is null");
  OW->EmitIntValue(Value, Size);
}

extern "C" void EmitSymbolDef(ObjectWriter *OW, const char *SymbolName) {
  assert(OW && "ObjWriter is null");
  OW->EmitSymbolDef(SymbolName);
}

extern "C" int EmitSymbolRef(ObjectWriter *OW, const char *SymbolName,
                             RelocType RelocType, int Delta) {
  assert(OW && "ObjWriter is null");
  return OW->EmitSymbolRef(SymbolName, RelocType, Delta);
}

extern "C" void EmitWinFrameInfo(ObjectWriter *OW, const char *FunctionName,
                                 int StartOffset, int EndOffset,
                                 const char *BlobSymbolName) {
  assert(OW && "ObjWriter is null");
  OW->EmitWinFrameInfo(FunctionName, StartOffset, EndOffset, BlobSymbolName);
}

extern "C" void EmitCFIStart(ObjectWriter *OW, int Offset) {
  assert(OW && "ObjWriter is null");
  OW->EmitCFIStart(Offset);
}

extern "C" void EmitCFIEnd(ObjectWriter *OW, int Offset) {
  assert(OW && "ObjWriter is null");
  OW->EmitCFIEnd(Offset);
}

extern "C" void EmitCFILsda(ObjectWriter *OW, const char *LsdaBlobSymbolName) {
  assert(OW && "ObjWriter is null");
  OW->EmitCFILsda(LsdaBlobSymbolName);
}

extern "C" void EmitCFICode(ObjectWriter *OW, int Offset, const char *Blob) {
  assert(OW && "ObjWriter is null");
  OW->EmitCFICode(Offset, Blob);
}

extern "C" void EmitDebugFileInfo(ObjectWriter *OW, int FileId,
                                  const char *FileName) {
  assert(OW && "ObjWriter is null");
  OW->EmitDebugFileInfo(FileId, FileName);
}

extern "C" void EmitDebugFunctionInfo(ObjectWriter *OW,
                                      const char *FunctionName,
                                      int FunctionSize) {
  assert(OW && "ObjWriter is null");
  OW->EmitDebugFunctionInfo(FunctionName, FunctionSize);
}

extern "C" void EmitDebugVar(ObjectWriter *OW, char *Name, int TypeIndex,
                             bool IsParam, int RangeCount,
                             ICorDebugInfo::NativeVarInfo *Ranges) {
  assert(OW && "ObjWriter is null");
  OW->EmitDebugVar(Name, TypeIndex, IsParam, RangeCount, Ranges);
}

extern "C" void EmitDebugLoc(ObjectWriter *OW, int NativeOffset, int FileId,
                             int LineNumber, int ColNumber) {
  assert(OW && "ObjWriter is null");
  OW->EmitDebugLoc(NativeOffset, FileId, LineNumber, ColNumber);
}

// This should be invoked at the end of module emission to finalize
// debug module info.
extern "C" void EmitDebugModuleInfo(ObjectWriter *OW) {
  assert(OW && "ObjWriter is null");
  OW->EmitDebugModuleInfo();
}
