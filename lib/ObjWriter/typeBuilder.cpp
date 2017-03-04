#include "typeBuilder.h"
#include "llvm/Support/COFF.h"

TypeBuilder::TypeBuilder() : Allocator(), TypeTable(Allocator) {}

void TypeBuilder::SetStreamer(MCObjectStreamer *Streamer) {
  assert(this->Streamer == 0);
  assert(Streamer != 0);
  this->Streamer = Streamer;
}

void TypeBuilder::EmitCodeViewMagicVersion() {
  Streamer->EmitValueToAlignment(4);
  Streamer->AddComment("Debug section magic");
  Streamer->EmitIntValue(COFF::DEBUG_SECTION_MAGIC, 4);
}

ClassOptions TypeBuilder::GetCommonClassOptions() { return ClassOptions(); }

void TypeBuilder::EmitTypeInformation(MCSection *COFFDebugTypesSection) {

  if (TypeTable.empty())
    return;

  Streamer->SwitchSection(COFFDebugTypesSection);
  EmitCodeViewMagicVersion();

  TypeTable.ForEachRecord([&](TypeIndex Index, ArrayRef<uint8_t> Record) {
    StringRef S(reinterpret_cast<const char *>(Record.data()), Record.size());
    Streamer->EmitBinaryData(S);
  });
}