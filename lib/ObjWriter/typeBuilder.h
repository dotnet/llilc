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
  unsigned ElementType;
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
  uint64 Offset;
  char *Name;
};

extern "C" struct ClassFieldsTypeDescriptior {
  uint64 Size;
  int FieldsCount;
};

extern "C" struct ArrayTypeDescriptor {
  unsigned Rank;
  unsigned ElementType;
  unsigned Size;
  int IsMultiDimensional;
};

#pragma pack(pop)
class UserDefinedTypesBuilder {
public:
  UserDefinedTypesBuilder();
  void SetStreamer(MCObjectStreamer *Streamer);
  void SetTargetPointerSize(unsigned TargetPointerSize);
  void EmitTypeInformation(MCSection *COFFDebugTypesSection);

  unsigned GetEnumTypeIndex(EnumTypeDescriptor TypeDescriptor,
                            EnumRecordTypeDescriptor *TypeRecords);
  unsigned GetClassTypeIndex(ClassTypeDescriptor ClassDescriptor);
  unsigned
  GetCompleteClassTypeIndex(ClassTypeDescriptor ClassDescriptor,
                            ClassFieldsTypeDescriptior ClassFieldsDescriptor,
                            DataFieldDescriptor *FieldsDescriptors);

  unsigned GetArrayTypeIndex(ClassTypeDescriptor ClassDescriptor,
                             ArrayTypeDescriptor ArrayDescriptor);

private:
  void EmitCodeViewMagicVersion();
  ClassOptions GetCommonClassOptions();

  unsigned GetEnumFieldListType(uint64 Count,
                                EnumRecordTypeDescriptor *TypeRecords);
  unsigned GetPointerType(TypeIndex ClassIndex);

  void AddBaseClass(FieldListRecordBuilder &FLBR, unsigned BaseClassId);

  MCObjectStreamer *Streamer;
  BumpPtrAllocator Allocator;
  TypeTableBuilder TypeTable;

  unsigned TargetPointerSize;
};
