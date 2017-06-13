//===---- typeBuilder.cpp --------------------------------*- C++ -*-===//
//
// type builder implementation using codeview::TypeTableBuilder
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//

#include "typeBuilder.h"
#include "llvm/BinaryFormat/COFF.h"
#include <sstream>

UserDefinedTypesBuilder::UserDefinedTypesBuilder()
    : Allocator(), TypeTable(Allocator), Streamer(nullptr),
      TargetPointerSize(0) {}

void UserDefinedTypesBuilder::SetStreamer(MCObjectStreamer *Streamer) {
  assert(this->Streamer == nullptr);
  assert(Streamer != nullptr);
  this->Streamer = Streamer;
}

void UserDefinedTypesBuilder::SetTargetPointerSize(unsigned TargetPointerSize) {
  assert(this->TargetPointerSize == 0);
  assert(TargetPointerSize != 0);
  this->TargetPointerSize = TargetPointerSize;
}

void UserDefinedTypesBuilder::EmitCodeViewMagicVersion() {
  Streamer->EmitValueToAlignment(4);
  Streamer->AddComment("Debug section magic");
  Streamer->EmitIntValue(COFF::DEBUG_SECTION_MAGIC, 4);
}

ClassOptions UserDefinedTypesBuilder::GetCommonClassOptions() {
  return ClassOptions();
}

void UserDefinedTypesBuilder::EmitTypeInformation(
    MCSection *COFFDebugTypesSection) {

  if (TypeTable.empty())
    return;

  Streamer->SwitchSection(COFFDebugTypesSection);
  EmitCodeViewMagicVersion();

  TypeTable.ForEachRecord([&](TypeIndex FieldTypeIndex,
                              ArrayRef<uint8_t> Record) {
    StringRef S(reinterpret_cast<const char *>(Record.data()), Record.size());
    Streamer->EmitBinaryData(S);
  });
}

unsigned UserDefinedTypesBuilder::GetEnumFieldListType(
    uint64 Count, const EnumRecordTypeDescriptor *TypeRecords) {
  FieldListRecordBuilder FLRB(TypeTable);
  FLRB.begin();
#ifndef NDEBUG
  uint64 MaxInt = (unsigned int)-1;
  assert(Count <= MaxInt && "There are too many fields inside enum");
#endif
  for (int i = 0; i < (int)Count; ++i) {
    EnumRecordTypeDescriptor record = TypeRecords[i];
    EnumeratorRecord ER(MemberAccess::Public, APSInt::getUnsigned(record.Value),
                        record.Name);
    FLRB.writeMemberType(ER);
  }
  TypeIndex Type = FLRB.end(true);
  return Type.getIndex();
}

unsigned UserDefinedTypesBuilder::GetEnumTypeIndex(
    const EnumTypeDescriptor &TypeDescriptor,
    const EnumRecordTypeDescriptor *TypeRecords) {

  ClassOptions CO = GetCommonClassOptions();
  unsigned FieldListIndex =
      GetEnumFieldListType(TypeDescriptor.ElementCount, TypeRecords);
  TypeIndex FieldListIndexType = TypeIndex(FieldListIndex);
  TypeIndex ElementTypeIndex = TypeIndex(TypeDescriptor.ElementType);

  EnumRecord EnumRecord(TypeDescriptor.ElementCount, CO, FieldListIndexType,
                        TypeDescriptor.Name, TypeDescriptor.UniqueName,
                        ElementTypeIndex);

  TypeIndex Type = TypeTable.writeKnownType(EnumRecord);
  UserDefinedTypes.push_back(std::make_pair(TypeDescriptor.Name, Type));
  return Type.getIndex();
}

unsigned UserDefinedTypesBuilder::GetClassTypeIndex(
    const ClassTypeDescriptor &ClassDescriptor) {
  TypeRecordKind Kind =
      ClassDescriptor.IsStruct ? TypeRecordKind::Struct : TypeRecordKind::Class;
  ClassOptions CO = ClassOptions::ForwardReference | GetCommonClassOptions();
  ClassRecord CR(Kind, 0, CO, TypeIndex(), TypeIndex(), TypeIndex(), 0,
                 ClassDescriptor.Name, ClassDescriptor.UniqueName);
  TypeIndex FwdDeclTI = TypeTable.writeKnownType(CR);

  if (ClassDescriptor.IsStruct == false) {
    PointerRecord PointerToClass(FwdDeclTI, 0);
    TypeIndex PointerIndex = TypeTable.writeKnownType(PointerToClass);
    return PointerIndex.getIndex();
  }
  return FwdDeclTI.getIndex();
}

unsigned UserDefinedTypesBuilder::GetCompleteClassTypeIndex(
    const ClassTypeDescriptor &ClassDescriptor,
    const ClassFieldsTypeDescriptior &ClassFieldsDescriptor,
    const DataFieldDescriptor *FieldsDescriptors) {

  FieldListRecordBuilder FLBR(TypeTable);
  FLBR.begin();

  if (ClassDescriptor.BaseClassId != 0) {
    AddBaseClass(FLBR, ClassDescriptor.BaseClassId);
  }

  for (int i = 0; i < ClassFieldsDescriptor.FieldsCount; ++i) {
    DataFieldDescriptor desc = FieldsDescriptors[i];
    MemberAccess Access = MemberAccess::Public;
    TypeIndex MemberBaseType(desc.FieldTypeIndex);
    int MemberOffsetInBytes = desc.Offset;
    DataMemberRecord DMR(Access, MemberBaseType, MemberOffsetInBytes,
                         desc.Name);
    FLBR.writeMemberType(DMR);
  }
  TypeIndex FieldListIndex = FLBR.end(true);
  TypeRecordKind Kind =
      ClassDescriptor.IsStruct ? TypeRecordKind::Struct : TypeRecordKind::Class;
  ClassOptions CO = GetCommonClassOptions();
  ClassRecord CR(Kind, ClassFieldsDescriptor.FieldsCount, CO, FieldListIndex,
                 TypeIndex(), TypeIndex(), ClassFieldsDescriptor.Size,
                 ClassDescriptor.Name, ClassDescriptor.UniqueName);
  TypeIndex ClassIndex = TypeTable.writeKnownType(CR);

  UserDefinedTypes.push_back(std::make_pair(ClassDescriptor.Name, ClassIndex));

  if (ClassDescriptor.IsStruct == false) {
    return GetPointerType(ClassIndex);
  }
  return ClassIndex.getIndex();
}

unsigned UserDefinedTypesBuilder::GetArrayTypeIndex(
    const ClassTypeDescriptor &ClassDescriptor,
    const ArrayTypeDescriptor &ArrayDescriptor) {
  FieldListRecordBuilder FLBR(TypeTable);
  FLBR.begin();

  unsigned Offset = 0;
  unsigned FieldsCount = 0;

  assert(ClassDescriptor.BaseClassId != 0);
  AddBaseClass(FLBR, ClassDescriptor.BaseClassId);
  FieldsCount++;
  Offset += TargetPointerSize;

  MemberAccess Access = MemberAccess::Public;
  TypeIndex IndexType = TypeIndex(SimpleTypeKind::Int32);
  DataMemberRecord CountDMR(Access, IndexType, Offset, "count");
  FLBR.writeMemberType(CountDMR);
  FieldsCount++;
  Offset += TargetPointerSize;

  if (ArrayDescriptor.IsMultiDimensional == 1) {
    for (unsigned i = 0; i < ArrayDescriptor.Rank; ++i) {
      DataMemberRecord LengthDMR(Access, TypeIndex(SimpleTypeKind::Int32),
                                 Offset, ArrayDimentions.GetLengthName(i));
      FLBR.writeMemberType(LengthDMR);
      FieldsCount++;
      Offset += 4;
    }

    for (unsigned i = 0; i < ArrayDescriptor.Rank; ++i) {
      DataMemberRecord BoundsDMR(Access, TypeIndex(SimpleTypeKind::Int32),
                                 Offset, ArrayDimentions.GetBoundsName(i));
      FLBR.writeMemberType(BoundsDMR);
      FieldsCount++;
      Offset += 4;
    }
  }

  TypeIndex ElementTypeIndex = TypeIndex(ArrayDescriptor.ElementType);
  ArrayRecord AR(ElementTypeIndex, IndexType, ArrayDescriptor.Size, "");
  TypeIndex ArrayIndex = TypeTable.writeKnownType(AR);
  DataMemberRecord ArrayDMR(Access, ArrayIndex, Offset, "values");
  FLBR.writeMemberType(ArrayDMR);
  FieldsCount++;

  TypeIndex FieldListIndex = FLBR.end(true);

  assert(ClassDescriptor.IsStruct == false);
  TypeRecordKind Kind = TypeRecordKind::Class;
  ClassOptions CO = GetCommonClassOptions();
  ClassRecord CR(Kind, FieldsCount, CO, FieldListIndex, TypeIndex(),
                 TypeIndex(), ArrayDescriptor.Size, ClassDescriptor.Name,
                 ClassDescriptor.UniqueName);
  TypeIndex ClassIndex = TypeTable.writeKnownType(CR);

  UserDefinedTypes.push_back(std::make_pair(ClassDescriptor.Name, ClassIndex));

  return GetPointerType(ClassIndex);
}

void UserDefinedTypesBuilder::AddBaseClass(FieldListRecordBuilder &FLBR,
                                           unsigned BaseClassId) {
  MemberAttributes def;
  TypeIndex BaseTypeIndex(BaseClassId);
  BaseClassRecord BCR(def, BaseTypeIndex, 0);
  FLBR.writeMemberType(BCR);
}

unsigned UserDefinedTypesBuilder::GetPointerType(const TypeIndex &ClassIndex) {
  PointerRecord PointerToClass(ClassIndex, 0);
  TypeIndex PointerIndex = TypeTable.writeKnownType(PointerToClass);
  return PointerIndex.getIndex();
}

const char *ArrayDimensionsDescriptor::GetLengthName(unsigned index) {
  if (Lengths.size() <= index) {
    Resize(index + 1);
  }
  return Lengths[index].c_str();
}

const char *ArrayDimensionsDescriptor::GetBoundsName(unsigned index) {
  if (Bounds.size() <= index) {
    Resize(index);
  }
  return Bounds[index].c_str();
}

void ArrayDimensionsDescriptor::Resize(unsigned NewSize) {
  unsigned OldSize = Lengths.size();
  assert(OldSize == Bounds.size());
  Lengths.resize(NewSize);
  Bounds.resize(NewSize);
  for (unsigned i = OldSize; i < NewSize; ++i) {
    std::stringstream ss;
    ss << "length" << i;
    ss >> Lengths[i];
    ss.clear();
    ss << "bounds" << i;
    ss >> Bounds[i];
  }
}
