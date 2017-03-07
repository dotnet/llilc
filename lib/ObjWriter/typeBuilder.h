#pragma once

#include "llvm/DebugInfo/CodeView/TypeTableBuilder.h"
#include "llvm/MC/MCObjectStreamer.h"

#include <string>

using namespace llvm;
using namespace llvm::codeview;

typedef unsigned long long ulong;

#pragma pack(push, 1)

extern "C" struct EnumRecordTypeDescriptor {
  ulong Value;
  char *Name;
};

extern "C" struct EnumTypeDescriptor {
  ulong ElementType;
  ulong ElementCount;
  char *Name;
  char *UniqueName;
};

extern "C" struct ClassTypeDescriptor {
  int IsStruct;
  char *Name;
  char *UniqueName;
  unsigned BaseClassId;
};

extern "C" struct DataFieldDescriptor {
  unsigned typeIndex;
  int offset;
  char *Name;
};

extern "C" struct ClassFieldsTypeDescriptior {
  int Size;
  int FieldsCount;
};

#pragma pack(pop)
class TypeBuilder {
public:
  TypeBuilder();
  void SetStreamer(MCObjectStreamer *Streamer);
  void EmitTypeInformation(MCSection *COFFDebugTypesSection);

  unsigned GetEnumTypeIndex(EnumTypeDescriptor TypeDescriptor,
                            EnumRecordTypeDescriptor *TypeRecords);
  unsigned GetClassTypeIndex(ClassTypeDescriptor ClassDescriptor);
  void
  CompleteClassDescription(ClassTypeDescriptor ClassDescriptor,
                           ClassFieldsTypeDescriptior ClassFieldsDescriptor,
                           DataFieldDescriptor *FieldsDescriptors);

private:
  void EmitCodeViewMagicVersion();
  ClassOptions GetCommonClassOptions();

  unsigned GetEnumFieldListType(ulong Count,
                                EnumRecordTypeDescriptor *TypeRecords);

  MCObjectStreamer *Streamer;
  BumpPtrAllocator Allocator;
  TypeTableBuilder TypeTable;
};