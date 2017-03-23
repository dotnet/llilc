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
#include "llvm/Support/COFF.h"

UserDefinedTypesBuilder::UserDefinedTypesBuilder()
    : Allocator(), TypeTable(Allocator) {}

void UserDefinedTypesBuilder::SetStreamer(MCObjectStreamer *Streamer) {
  assert(this->Streamer == 0);
  assert(Streamer != 0);
  this->Streamer = Streamer;
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
    uint64 Count, EnumRecordTypeDescriptor *TypeRecords) {
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
  TypeIndex Type = FLRB.end();
  return Type.getIndex();
}

unsigned UserDefinedTypesBuilder::GetEnumTypeIndex(
    EnumTypeDescriptor TypeDescriptor, EnumRecordTypeDescriptor *TypeRecords) {

  ClassOptions CO = GetCommonClassOptions();
  unsigned FieldListIndex =
      GetEnumFieldListType(TypeDescriptor.ElementCount, TypeRecords);
  TypeIndex FieldListIndexType = TypeIndex(FieldListIndex);
  TypeIndex ElementTypeIndex = TypeIndex(TypeDescriptor.ElementType);

  EnumRecord EnumRecord(TypeDescriptor.ElementCount, CO, FieldListIndexType,
                        TypeDescriptor.Name, TypeDescriptor.UniqueName,
                        ElementTypeIndex);

  TypeIndex Type = TypeTable.writeKnownType(EnumRecord);
  return Type.getIndex();
}

unsigned UserDefinedTypesBuilder::GetClassTypeIndex(
    ClassTypeDescriptor ClassDescriptor) {
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
    ClassTypeDescriptor ClassDescriptor,
    ClassFieldsTypeDescriptior ClassFieldsDescriptor,
    DataFieldDescriptor *FieldsDescriptors) {

  FieldListRecordBuilder FLBR(TypeTable);
  FLBR.begin();

  if (ClassDescriptor.BaseClassId != 0) {
    MemberAttributes def;
    TypeIndex BaseTypeIndex(ClassDescriptor.BaseClassId);
    BaseClassRecord BCR(def, BaseTypeIndex, 0);
    FLBR.writeMemberType(BCR);
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
  TypeIndex FieldListIndex = FLBR.end();
  TypeRecordKind Kind =
      ClassDescriptor.IsStruct ? TypeRecordKind::Struct : TypeRecordKind::Class;
  ClassOptions CO = GetCommonClassOptions();
  ClassRecord CR(Kind, ClassFieldsDescriptor.FieldsCount, CO, FieldListIndex,
                 TypeIndex(), TypeIndex(), ClassFieldsDescriptor.Size,
                 ClassDescriptor.Name, ClassDescriptor.UniqueName);
  TypeIndex ClassIndex = TypeTable.writeKnownType(CR);
  if (ClassDescriptor.IsStruct == false) {
    PointerRecord PointerToClass(ClassIndex, 0);
    TypeIndex PointerIndex = TypeTable.writeKnownType(PointerToClass);
    return PointerIndex.getIndex();
  }
  return ClassIndex.getIndex();
}
