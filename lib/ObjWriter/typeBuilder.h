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
#include <vector>

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

class ArrayDimensionsDescriptor {
public:
  const char *GetLengthName(unsigned index);
  const char *GetBoundsName(unsigned index);

private:
  void Resize(unsigned NewSize);

  std::vector<std::string> Lengths;
  std::vector<std::string> Bounds;
};

#pragma pack(pop)
class UserDefinedTypesBuilder {
public:
  UserDefinedTypesBuilder();
  void SetStreamer(MCObjectStreamer *Streamer);
  void SetTargetPointerSize(unsigned TargetPointerSize);
  void EmitTypeInformation(MCSection *COFFDebugTypesSection);

  unsigned GetEnumTypeIndex(const EnumTypeDescriptor &TypeDescriptor,
                            const EnumRecordTypeDescriptor *TypeRecords);
  unsigned GetClassTypeIndex(const ClassTypeDescriptor &ClassDescriptor);
  unsigned GetCompleteClassTypeIndex(
      const ClassTypeDescriptor &ClassDescriptor,
      const ClassFieldsTypeDescriptior &ClassFieldsDescriptor,
      const DataFieldDescriptor *FieldsDescriptors);

  unsigned GetArrayTypeIndex(const ClassTypeDescriptor &ClassDescriptor,
                             const ArrayTypeDescriptor &ArrayDescriptor);

private:
  void EmitCodeViewMagicVersion();
  ClassOptions GetCommonClassOptions();

  unsigned GetEnumFieldListType(uint64 Count,
                                const EnumRecordTypeDescriptor *TypeRecords);
  unsigned GetPointerType(const TypeIndex &ClassIndex);

  void AddBaseClass(FieldListRecordBuilder &FLBR, unsigned BaseClassId);

  BumpPtrAllocator Allocator;
  TypeTableBuilder TypeTable;

  MCObjectStreamer *Streamer;
  unsigned TargetPointerSize;

  ArrayDimensionsDescriptor ArrayDimentions;
};
