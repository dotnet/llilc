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

unsigned
TypeBuilder::GetEnumFieldListType(ulong Count,
                                  EnumRecordTypeDescriptor *TypeRecords) {
  FieldListRecordBuilder FLRB(TypeTable);
  FLRB.begin();
  for (int i = 0; i < Count; ++i) {
    EnumRecordTypeDescriptor record = TypeRecords[i];
    EnumeratorRecord ER(MemberAccess::Public, APSInt::getUnsigned(record.Value),
                        record.Name);
    FLRB.writeMemberType(ER);
  }
  TypeIndex Type = FLRB.end();
  return Type.getIndex();
}

unsigned TypeBuilder::GetEnumTypeIndex(EnumTypeDescriptor TypeDescriptor,
                                       EnumRecordTypeDescriptor *TypeRecords) {

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

unsigned TypeBuilder::GetClassTypeIndex(ClassTypeDescriptor ClassDescriptor) {
  TypeRecordKind Kind =
      ClassDescriptor.IsStruct ? TypeRecordKind::Struct : TypeRecordKind::Class;
  ClassOptions CO = ClassOptions::ForwardReference | GetCommonClassOptions();
  ClassRecord CR(Kind, 0, CO, TypeIndex(), TypeIndex(), TypeIndex(), 0,
                 ClassDescriptor.Name, ClassDescriptor.UniqueName);
  TypeIndex FwdDeclTI = TypeTable.writeKnownType(CR);
  return FwdDeclTI.getIndex();
}

void TypeBuilder::CompleteClassDescription(
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
    TypeIndex MemberBaseType(desc.typeIndex);
    int MemberOffsetInBytes = desc.offset;
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
  TypeIndex CompleteTI = TypeTable.writeKnownType(CR);

  if (ClassDescriptor.IsStruct == false) {
    PointerRecord pr(CompleteTI, 0);
    TypeIndex prIndex = TypeTable.writeKnownType(pr);
  }
}