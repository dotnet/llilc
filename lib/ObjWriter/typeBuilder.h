//===---- typeBuilder.h --------------------------------*- C++ -*-===//
//
// type builder is used to convert .Net types into CodeView descriptors.
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/DebugInfo/CodeView/TypeTableBuilder.h"
#include "llvm/MC/MCObjectStreamer.h"

#include <string>

using namespace llvm;
using namespace llvm::codeview;

typedef unsigned long long uint64;

#pragma pack(push, 1)

extern "C" struct EnumRecordTypeDescriptor {
  uint64 Value;
  char *Name;
};

extern "C" struct EnumTypeDescriptor {
  uint64 ElementType;
  uint64 ElementCount;
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
  unsigned FieldTypeIndex;
  int Offset;
  char *Name;
};

extern "C" struct ClassFieldsTypeDescriptior {
  int Size;
  int FieldsCount;
};

#pragma pack(pop)
class UserDefinedTypesBuilder {
public:
  UserDefinedTypesBuilder();
  void SetStreamer(MCObjectStreamer *Streamer);
  void EmitTypeInformation(MCSection *COFFDebugTypesSection);

  unsigned GetEnumTypeIndex(EnumTypeDescriptor TypeDescriptor,
                            EnumRecordTypeDescriptor *TypeRecords);
  unsigned GetClassTypeIndex(ClassTypeDescriptor ClassDescriptor);
  unsigned
  GetCompleteClassTypeIndex(ClassTypeDescriptor ClassDescriptor,
                            ClassFieldsTypeDescriptior ClassFieldsDescriptor,
                            DataFieldDescriptor *FieldsDescriptors);

private:
  void EmitCodeViewMagicVersion();
  ClassOptions GetCommonClassOptions();

  unsigned GetEnumFieldListType(uint64 Count,
                                EnumRecordTypeDescriptor *TypeRecords);

  MCObjectStreamer *Streamer;
  BumpPtrAllocator Allocator;
  TypeTableBuilder TypeTable;
};
