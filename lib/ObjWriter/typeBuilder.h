#pragma once

#include "llvm/DebugInfo/CodeView/TypeTableBuilder.h"
#include "llvm/MC/MCObjectStreamer.h"

#include <string>

using namespace llvm;
using namespace llvm::codeview;

class TypeBuilder {
public:
  TypeBuilder();
  void SetStreamer(MCObjectStreamer *Streamer);
  void EmitTypeInformation(MCSection *COFFDebugTypesSection);

private:
  void EmitCodeViewMagicVersion();
  ClassOptions GetCommonClassOptions();

  MCObjectStreamer *Streamer;
  BumpPtrAllocator Allocator;
  TypeTableBuilder TypeTable;
};