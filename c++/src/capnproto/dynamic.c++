// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define CAPNPROTO_PRIVATE
#include "dynamic.h"
#include "logging.h"
#include <unordered_map>
#include <string>

namespace capnproto {

struct IdTextHash {
  size_t operator()(std::pair<uint64_t, Text::Reader> p) const {
    // djb2a hash, but seeded with ID.
    size_t result = p.first;
    int c;
    const char* str = p.second.c_str();

    while ((c = *str++)) {
      result = ((result << 5) + result) ^ c;  // (result * 33) ^ c
    }

    return result;
  }
};

struct SchemaPool::Impl {
  std::unordered_map<uint64_t, schema::Node::Reader> nodeMap;
  std::unordered_map<std::pair<uint64_t, Text::Reader>, schema::StructNode::Member::Reader,
                     IdTextHash>
      memberMap;
  std::unordered_map<std::pair<uint64_t, Text::Reader>, schema::EnumNode::Enumerant::Reader,
                     IdTextHash>
      enumerantMap;
};

SchemaPool::~SchemaPool() {
  delete impl;
}

// TODO(now):  Implement this.  Need to copy, ick.
void add(schema::Node::Reader node) {
  FAIL_CHECK("Not implemented: copying/validating schemas.");
}

void SchemaPool::addNoCopy(schema::Node::Reader node) {
  if (impl == nullptr) {
    impl = new Impl;
  }

  // TODO(soon):  Check if node is in base.
  // TODO(soon):  Check if existing node came from generated code.

  auto entry = std::make_pair(node.getId(), node);
  auto ins = impl->nodeMap.insert(entry);
  if (!ins.second) {
    // TODO(soon):  Check for compatibility.
    FAIL_CHECK("TODO:  Check schema compatibility when adding.");
  }
}

bool SchemaPool::has(uint64_t id) const {
  return (impl != nullptr && impl->nodeMap.count(id) != 0) || (base != nullptr && base->has(id));
}

// =======================================================================================

namespace {

template <typename T, typename U>
CAPNPROTO_ALWAYS_INLINE(T bitCast(U value));

template <typename T, typename U>
inline T bitCast(U value) {
  static_assert(sizeof(T) == sizeof(U), "Size must match.");
  return value;
}
template <>
inline float bitCast<float, uint32_t>(uint32_t value) {
  float result;
  memcpy(&result, &value, sizeof(value));
  return result;
}
template <>
inline double bitCast<double, uint64_t>(uint64_t value) {
  double result;
  memcpy(&result, &value, sizeof(value));
  return result;
}
template <>
inline uint32_t bitCast<uint32_t, float>(float value) {
  uint32_t result;
  memcpy(&result, &value, sizeof(value));
  return result;
}
template <>
inline uint64_t bitCast<uint64_t, double>(double value) {
  uint64_t result;
  memcpy(&result, &value, sizeof(value));
  return result;
}

internal::FieldSize elementSizeFor(schema::Type::Body::Which elementType) {
  switch (elementType) {
    case schema::Type::Body::VOID_TYPE: return internal::FieldSize::VOID;
    case schema::Type::Body::BOOL_TYPE: return internal::FieldSize::BIT;
    case schema::Type::Body::INT8_TYPE: return internal::FieldSize::BYTE;
    case schema::Type::Body::INT16_TYPE: return internal::FieldSize::TWO_BYTES;
    case schema::Type::Body::INT32_TYPE: return internal::FieldSize::FOUR_BYTES;
    case schema::Type::Body::INT64_TYPE: return internal::FieldSize::EIGHT_BYTES;
    case schema::Type::Body::UINT8_TYPE: return internal::FieldSize::BYTE;
    case schema::Type::Body::UINT16_TYPE: return internal::FieldSize::TWO_BYTES;
    case schema::Type::Body::UINT32_TYPE: return internal::FieldSize::FOUR_BYTES;
    case schema::Type::Body::UINT64_TYPE: return internal::FieldSize::EIGHT_BYTES;
    case schema::Type::Body::FLOAT32_TYPE: return internal::FieldSize::FOUR_BYTES;
    case schema::Type::Body::FLOAT64_TYPE: return internal::FieldSize::EIGHT_BYTES;

    case schema::Type::Body::TEXT_TYPE: return internal::FieldSize::REFERENCE;
    case schema::Type::Body::DATA_TYPE: return internal::FieldSize::REFERENCE;
    case schema::Type::Body::LIST_TYPE: return internal::FieldSize::REFERENCE;
    case schema::Type::Body::ENUM_TYPE: return internal::FieldSize::TWO_BYTES;
    case schema::Type::Body::STRUCT_TYPE: return internal::FieldSize::INLINE_COMPOSITE;
    case schema::Type::Body::INTERFACE_TYPE: return internal::FieldSize::REFERENCE;
    case schema::Type::Body::OBJECT_TYPE: FAIL_CHECK("List(Object) not supported.");
  }
  FAIL_CHECK("Can't get here.");
  return internal::FieldSize::VOID;
}

inline internal::StructSize structSizeFromSchema(schema::StructNode::Reader schema) {
  return internal::StructSize(
      schema.getDataSectionWordSize() * WORDS,
      schema.getPointerSectionSize() * REFERENCES,
      static_cast<internal::FieldSize>(schema.getPreferredListEncoding()));
}

}  // namespace

// =======================================================================================

schema::EnumNode::Reader DynamicEnum::getSchema() {
  return schema.getBody().getEnumNode();
}

Maybe<schema::EnumNode::Enumerant::Reader> DynamicEnum::getEnumerant() {
  auto enumerants = getSchema().getEnumerants();
  if (value < enumerants.size()) {
    return enumerants[value];
  } else {
    return nullptr;
  }
}

Maybe<schema::EnumNode::Enumerant::Reader> DynamicEnum::findEnumerantByName(Text::Reader name) {
  auto iter = pool->impl->enumerantMap.find(std::make_pair(schema.getId(), name));
  if (iter == pool->impl->enumerantMap.end()) {
    return nullptr;
  } else {
    return iter->second;
  }
}

uint16_t DynamicEnum::asImpl(uint64_t requestedTypeId) {
  VALIDATE_INPUT(requestedTypeId == schema.getId(), "Type mismatch in DynamicEnum.as().") {
    // Go on with value.
  }
  return value;
}

// =======================================================================================

DynamicStruct::Reader DynamicObject::Reader::toStruct(schema::Node::Reader schema) {
  PRECOND(schema.getBody().which() == schema::Node::Body::STRUCT_NODE,
          "toStruct() passed a non-struct schema.");
  if (reader.kind == internal::ObjectKind::NULL_POINTER) {
    return DynamicStruct::Reader(pool, schema, internal::StructReader());
  }
  VALIDATE_INPUT(reader.kind == internal::ObjectKind::STRUCT, "Object is not a struct.") {
    return DynamicStruct::Reader(pool, schema, internal::StructReader());
  }
  return DynamicStruct::Reader(pool, schema, reader.structReader);
}
DynamicStruct::Builder DynamicObject::Builder::toStruct(schema::Node::Reader schema) {
  PRECOND(schema.getBody().which() == schema::Node::Body::STRUCT_NODE,
          "toStruct() passed a non-struct schema.");
  if (builder.kind == internal::ObjectKind::NULL_POINTER) {
    return DynamicStruct::Builder(pool, schema, internal::StructBuilder());
  }
  VALIDATE_INPUT(builder.kind == internal::ObjectKind::STRUCT, "Object is not a struct.") {
    return DynamicStruct::Builder(pool, schema, internal::StructBuilder());
  }
  return DynamicStruct::Builder(pool, schema, builder.structBuilder);
}

DynamicStruct::Reader DynamicObject::Reader::toStruct(uint64_t typeId) {
  return toStruct(pool->getStruct(typeId));
}
DynamicStruct::Builder DynamicObject::Builder::toStruct(uint64_t typeId) {
  return toStruct(pool->getStruct(typeId));
}

DynamicList::Reader DynamicObject::Reader::toList(schema::Type::Reader elementType) {
  return toList(internal::ListSchema(elementType));
}
DynamicList::Builder DynamicObject::Builder::toList(schema::Type::Reader elementType) {
  return toList(internal::ListSchema(elementType));
}

DynamicList::Reader DynamicObject::Reader::toList(internal::ListSchema schema) {
  if (reader.kind == internal::ObjectKind::NULL_POINTER) {
    return DynamicList::Reader(pool, schema, internal::ListReader());
  }
  VALIDATE_INPUT(reader.kind == internal::ObjectKind::LIST, "Object is not a list.") {
    return DynamicList::Reader(pool, schema, internal::ListReader());
  }
  return DynamicList::Reader(pool, schema, reader.listReader);
}
DynamicList::Builder DynamicObject::Builder::toList(internal::ListSchema schema) {
  if (builder.kind == internal::ObjectKind::NULL_POINTER) {
    return DynamicList::Builder(pool, schema, internal::ListBuilder());
  }
  VALIDATE_INPUT(builder.kind == internal::ObjectKind::LIST, "Object is not a list.") {
    return DynamicList::Builder(pool, schema, internal::ListBuilder());
  }
  return DynamicList::Builder(pool, schema, builder.listBuilder);
}

// =======================================================================================

Maybe<schema::StructNode::Member::Reader> DynamicUnion::Reader::which() {
  auto members = schema.getMembers();
  uint16_t discrim = reader.getDataField<uint32_t>(schema.getDiscriminantOffset() * ELEMENTS);

  if (discrim < members.size()) {
    return members[discrim];
  } else {
    return nullptr;
  }
}
Maybe<schema::StructNode::Member::Reader> DynamicUnion::Builder::which() {
  auto members = schema.getMembers();
  uint16_t discrim = builder.getDataField<uint32_t>(schema.getDiscriminantOffset() * ELEMENTS);

  if (discrim < members.size()) {
    return members[discrim];
  } else {
    return nullptr;
  }
}

DynamicValue::Reader DynamicUnion::Reader::get() {
  auto w = which();
  RECOVERABLE_PRECOND(w != nullptr, "Can't get() unknown union value.") {
    return DynamicValue::Reader();
  }
  auto body = w->getBody();
  CHECK(body.which() == schema::StructNode::Member::Body::FIELD_MEMBER,
        "Unsupported union member type.");
  return DynamicValue::Reader(DynamicStruct::Reader::getFieldImpl(
      pool, reader, body.getFieldMember()));
}

DynamicValue::Builder DynamicUnion::Builder::get() {
  auto w = which();
  RECOVERABLE_PRECOND(w != nullptr, "Can't get() unknown union value.") {
    return DynamicValue::Builder();
  }
  auto body = w->getBody();
  CHECK(body.which() == schema::StructNode::Member::Body::FIELD_MEMBER,
        "Unsupported union member type.");
  return DynamicValue::Builder(DynamicStruct::Builder::getFieldImpl(
      pool, builder, body.getFieldMember()));
}

void DynamicUnion::Builder::set(
    schema::StructNode::Field::Reader field, DynamicValue::Reader value) {
  builder.setDataField<uint16_t>(schema.getDiscriminantOffset() * ELEMENTS, field.getIndex());
  DynamicStruct::Builder::setFieldImpl(pool, builder, field, value);
}

DynamicValue::Builder DynamicUnion::Builder::init(schema::StructNode::Field::Reader field) {
  builder.setDataField<uint16_t>(schema.getDiscriminantOffset() * ELEMENTS, field.getIndex());
  return DynamicStruct::Builder::initFieldImpl(pool, builder, field);
}

DynamicValue::Builder DynamicUnion::Builder::init(schema::StructNode::Field::Reader field,
                                                  uint size) {
  builder.setDataField<uint16_t>(schema.getDiscriminantOffset() * ELEMENTS, field.getIndex());
  return DynamicStruct::Builder::initFieldImpl(pool, builder, field, size);
}

// =======================================================================================

void DynamicStruct::Reader::verifyTypeId(uint64_t id) {
  VALIDATE_INPUT(id == schema.getId(),
                 "Type mismatch when using DynamicStruct::Reader::as().") {
    // Go on with bad type ID.
  }
}
void DynamicStruct::Builder::verifyTypeId(uint64_t id) {
  VALIDATE_INPUT(id == schema.getId(),
                 "Type mismatch when using DynamicStruct::Builder::as().") {
    // Go on with bad type ID.
  }
}

schema::StructNode::Reader DynamicStruct::Reader::getSchema() {
  return schema.getBody().getStructNode();
}
schema::StructNode::Reader DynamicStruct::Builder::getSchema() {
  return schema.getBody().getStructNode();
}

Maybe<schema::StructNode::Member::Reader> DynamicStruct::Reader::findMemberByName(
    Text::Reader name) {
  auto iter = pool->impl->memberMap.find(std::make_pair(schema.getId(), name));
  if (iter == pool->impl->memberMap.end()) {
    return nullptr;
  } else {
    return iter->second;
  }
}
Maybe<schema::StructNode::Member::Reader> DynamicStruct::Builder::findMemberByName(
    Text::Reader name) {
  auto iter = pool->impl->memberMap.find(std::make_pair(schema.getId(), name));
  if (iter == pool->impl->memberMap.end()) {
    return nullptr;
  } else {
    return iter->second;
  }
}

DynamicValue::Builder DynamicStruct::Builder::initObjectField(
    schema::StructNode::Field::Reader field, schema::Type::Reader type) {
  VALIDATE_INPUT(field.getType().getBody().which() == schema::Type::Body::OBJECT_TYPE,
                 "Expected an Object.  (To dynamically initialize a non-Object field, do not "
                 "pass an element type to initObjectField().)") {
    return initFieldImpl(pool, builder, field);
  }
  return initFieldImpl(pool, builder, field, type);
}
DynamicValue::Builder DynamicStruct::Builder::initObjectField(
    schema::StructNode::Field::Reader field, schema::Type::Reader type, uint size) {
  VALIDATE_INPUT(field.getType().getBody().which() == schema::Type::Body::OBJECT_TYPE,
                 "Expected an Object.  (To dynamically initialize a non-Object field, do not "
                 "pass a struct schema to initObjectField().)") {
    return initFieldImpl(pool, builder, field, size);
  }
  return initFieldImpl(pool, builder, field, type, size);
}

DynamicUnion::Reader DynamicStruct::Reader::getUnion(schema::StructNode::Union::Reader un) {
  return DynamicUnion::Reader(pool, un, reader);
}
DynamicUnion::Builder DynamicStruct::Builder::getUnion(schema::StructNode::Union::Reader un) {
  return DynamicUnion::Builder(pool, un, builder);
}

void DynamicStruct::Builder::copyFrom(Reader other) {
  // TODO(now): copyFrom on StructBuilder.
  // TODO(now): don't forget to check types match.
  FAIL_CHECK("Unimplemented: copyFrom()");
}

DynamicValue::Reader DynamicStruct::Reader::getFieldImpl(
    const SchemaPool* pool, internal::StructReader reader,
    schema::StructNode::Field::Reader field) {
  auto type = field.getType().getBody();
  auto dval = field.getDefaultValue().getBody();

  switch (type.which()) {
    case schema::Type::Body::VOID_TYPE:
      return DynamicValue::Reader(reader.getDataField<Void>(field.getOffset() * ELEMENTS));

#define HANDLE_TYPE(discrim, titleCase, type) \
    case schema::Type::Body::discrim##_TYPE: \
      return DynamicValue::Reader(reader.getDataField<type>( \
          field.getOffset() * ELEMENTS, \
          bitCast<typename internal::MaskType<type>::Type>(dval.get##titleCase##Value())));

    HANDLE_TYPE(BOOL, Bool, bool)
    HANDLE_TYPE(INT8, Int8, int8_t)
    HANDLE_TYPE(INT16, Int16, int16_t)
    HANDLE_TYPE(INT32, Int32, int32_t)
    HANDLE_TYPE(INT64, Int64, int64_t)
    HANDLE_TYPE(UINT8, Uint8, uint8_t)
    HANDLE_TYPE(UINT16, Uint16, uint16_t)
    HANDLE_TYPE(UINT32, Uint32, uint32_t)
    HANDLE_TYPE(UINT64, Uint64, uint64_t)
    HANDLE_TYPE(FLOAT32, Float32, float)
    HANDLE_TYPE(FLOAT64, Float64, double)

#undef HANDLE_TYPE

    case schema::Type::Body::ENUM_TYPE: {
      uint16_t typedDval;
      typedDval = dval.getEnumValue();
      return DynamicValue::Reader(DynamicEnum(
          pool, pool->getEnum(type.getEnumType()),
          reader.getDataField<uint16_t>(field.getOffset() * ELEMENTS, typedDval)));
    }

    case schema::Type::Body::TEXT_TYPE: {
      Text::Reader typedDval = dval.getTextValue();
      return DynamicValue::Reader(
          reader.getBlobField<Text>(field.getOffset() * REFERENCES,
                                    typedDval.data(), typedDval.size() * BYTES));
    }

    case schema::Type::Body::DATA_TYPE: {
      Data::Reader typedDval = dval.getDataValue();
      return DynamicValue::Reader(
          reader.getBlobField<Data>(field.getOffset() * REFERENCES,
                                    typedDval.data(), typedDval.size() * BYTES));
    }

    case schema::Type::Body::LIST_TYPE: {
      auto elementType = type.getListType();
      return DynamicValue::Reader(DynamicList::Reader(
          pool, elementType,
          reader.getListField(field.getOffset() * REFERENCES,
                              elementSizeFor(elementType.getBody().which()),
                              dval.getListValue<internal::TrustedMessage>())));
    }

    case schema::Type::Body::STRUCT_TYPE: {
      return DynamicValue::Reader(DynamicStruct::Reader(
          pool, pool->getStruct(type.getStructType()),
          reader.getStructField(field.getOffset() * REFERENCES,
                                dval.getStructValue<internal::TrustedMessage>())));
    }

    case schema::Type::Body::OBJECT_TYPE: {
      return DynamicValue::Reader(DynamicObject::Reader(
          pool, reader.getObjectField(field.getOffset() * REFERENCES,
                                      dval.getObjectValue<internal::TrustedMessage>())));
    }

    case schema::Type::Body::INTERFACE_TYPE:
      FAIL_CHECK("Interfaces not yet implemented.");
      break;
  }

  FAIL_CHECK("switch() missing case.", type.which());
  return DynamicValue::Reader();
}

DynamicValue::Builder DynamicStruct::Builder::getFieldImpl(
    const SchemaPool* pool, internal::StructBuilder builder,
    schema::StructNode::Field::Reader field) {
  auto type = field.getType().getBody();
  auto dval = field.getDefaultValue().getBody();

  switch (type.which()) {
    case schema::Type::Body::VOID_TYPE:
      return DynamicValue::Builder(builder.getDataField<Void>(field.getOffset() * ELEMENTS));

#define HANDLE_TYPE(discrim, titleCase, type) \
    case schema::Type::Body::discrim##_TYPE: \
      return DynamicValue::Builder(builder.getDataField<type>( \
          field.getOffset() * ELEMENTS, \
          bitCast<typename internal::MaskType<type>::Type>(dval.get##titleCase##Value())));

    HANDLE_TYPE(BOOL, Bool, bool)
    HANDLE_TYPE(INT8, Int8, int8_t)
    HANDLE_TYPE(INT16, Int16, int16_t)
    HANDLE_TYPE(INT32, Int32, int32_t)
    HANDLE_TYPE(INT64, Int64, int64_t)
    HANDLE_TYPE(UINT8, Uint8, uint8_t)
    HANDLE_TYPE(UINT16, Uint16, uint16_t)
    HANDLE_TYPE(UINT32, Uint32, uint32_t)
    HANDLE_TYPE(UINT64, Uint64, uint64_t)
    HANDLE_TYPE(FLOAT32, Float32, float)
    HANDLE_TYPE(FLOAT64, Float64, double)

#undef HANDLE_TYPE

    case schema::Type::Body::ENUM_TYPE: {
      uint16_t typedDval;
      typedDval = dval.getEnumValue();
      return DynamicValue::Builder(DynamicEnum(
          pool, pool->getEnum(type.getEnumType()),
          builder.getDataField<uint16_t>(field.getOffset() * ELEMENTS, typedDval)));
    }

    case schema::Type::Body::TEXT_TYPE: {
      Text::Reader typedDval = dval.getTextValue();
      return DynamicValue::Builder(
          builder.getBlobField<Text>(field.getOffset() * REFERENCES,
                                     typedDval.data(), typedDval.size() * BYTES));
    }

    case schema::Type::Body::DATA_TYPE: {
      Data::Reader typedDval = dval.getDataValue();
      return DynamicValue::Builder(
          builder.getBlobField<Data>(field.getOffset() * REFERENCES,
                                     typedDval.data(), typedDval.size() * BYTES));
    }

    case schema::Type::Body::LIST_TYPE: {
      auto elementType = type.getListType();
      return DynamicValue::Builder(DynamicList::Builder(
          pool, elementType,
          builder.getListField(field.getOffset() * REFERENCES,
                               dval.getListValue<internal::TrustedMessage>())));
    }

    case schema::Type::Body::STRUCT_TYPE: {
      auto structNode = pool->getStruct(type.getStructType());
      auto structSchema = structNode.getBody().getStructNode();
      return DynamicValue::Builder(DynamicStruct::Builder(
          pool, structNode,
          builder.getStructField(
              field.getOffset() * REFERENCES,
              internal::StructSize(
                  structSchema.getDataSectionWordSize() * WORDS,
                  structSchema.getPointerSectionSize() * REFERENCES,
                  static_cast<internal::FieldSize>(structSchema.getPreferredListEncoding())),
              dval.getStructValue<internal::TrustedMessage>())));
    }

    case schema::Type::Body::OBJECT_TYPE: {
      return DynamicValue::Builder(DynamicObject::Builder(
          pool, builder.getObjectField(field.getOffset() * REFERENCES,
                                       dval.getObjectValue<internal::TrustedMessage>())));
    }

    case schema::Type::Body::INTERFACE_TYPE:
      FAIL_CHECK("Interfaces not yet implemented.");
      break;
  }

  FAIL_CHECK("switch() missing case.", type.which());
  return DynamicValue::Builder();
}

void DynamicStruct::Builder::setFieldImpl(
    const SchemaPool* pool, internal::StructBuilder builder,
    schema::StructNode::Field::Reader field, DynamicValue::Reader value) {
  auto type = field.getType().getBody();
  auto dval = field.getDefaultValue().getBody();

  switch (type.which()) {
    case schema::Type::Body::VOID_TYPE:
      builder.setDataField<Void>(field.getOffset() * ELEMENTS, value.as<Void>());
      break;

#define HANDLE_TYPE(discrim, titleCase, type) \
    case schema::Type::Body::discrim##_TYPE: \
      builder.setDataField<type>( \
          field.getOffset() * ELEMENTS, value.as<type>(), \
          bitCast<internal::Mask<type> >(dval.get##titleCase##Value()));
      break;

    HANDLE_TYPE(BOOL, Bool, bool)
    HANDLE_TYPE(INT8, Int8, int8_t)
    HANDLE_TYPE(INT16, Int16, int16_t)
    HANDLE_TYPE(INT32, Int32, int32_t)
    HANDLE_TYPE(INT64, Int64, int64_t)
    HANDLE_TYPE(UINT8, Uint8, uint8_t)
    HANDLE_TYPE(UINT16, Uint16, uint16_t)
    HANDLE_TYPE(UINT32, Uint32, uint32_t)
    HANDLE_TYPE(UINT64, Uint64, uint64_t)
    HANDLE_TYPE(FLOAT32, Float32, float)
    HANDLE_TYPE(FLOAT64, Float64, double)

#undef HANDLE_TYPE

    case schema::Type::Body::ENUM_TYPE:
      builder.setDataField<uint16_t>(
          field.getOffset() * ELEMENTS, value.as<DynamicEnum>().getRaw(),
          dval.getEnumValue());
      break;

    case schema::Type::Body::TEXT_TYPE:
      builder.setBlobField<Text>(field.getOffset() * REFERENCES, value.as<Text>());
      break;

    case schema::Type::Body::DATA_TYPE:
      builder.setBlobField<Data>(field.getOffset() * REFERENCES, value.as<Data>());
      break;

    case schema::Type::Body::LIST_TYPE: {
      // TODO(now):  We need to do a schemaless copy to avoid losing information if the values are
      //   larger than what the schema defines.
      auto listValue = value.as<DynamicList>();
      initFieldImpl(pool, builder, field, listValue.size()).as<DynamicList>().copyFrom(listValue);
      break;
    }

    case schema::Type::Body::STRUCT_TYPE: {
      // TODO(now):  We need to do a schemaless copy to avoid losing information if the values are
      //   larger than what the schema defines.
      initFieldImpl(pool, builder, field).as<DynamicStruct>().copyFrom(value.as<DynamicStruct>());
      break;
    }

    case schema::Type::Body::OBJECT_TYPE: {
      // TODO(now):  Perform schemaless copy.
      FAIL_CHECK("TODO");
      break;
    }

    case schema::Type::Body::INTERFACE_TYPE:
      FAIL_CHECK("Interfaces not yet implemented.");
      break;
  }
}

DynamicValue::Builder DynamicStruct::Builder::initFieldImpl(
    const SchemaPool* pool, internal::StructBuilder builder,
    schema::StructNode::Field::Reader field, uint size) {
  return initFieldImpl(pool, builder, field, field.getType(), size);
}

DynamicValue::Builder DynamicStruct::Builder::initFieldImpl(
    const SchemaPool* pool, internal::StructBuilder builder,
    schema::StructNode::Field::Reader field) {
  return initFieldImpl(pool, builder, field, field.getType());
}

DynamicValue::Builder DynamicStruct::Builder::initFieldImpl(
    const SchemaPool* pool, internal::StructBuilder builder,
    schema::StructNode::Field::Reader field,
    schema::Type::Reader type, uint size) {
  switch (type.getBody().which()) {
    case schema::Type::Body::VOID_TYPE:
    case schema::Type::Body::BOOL_TYPE:
    case schema::Type::Body::INT8_TYPE:
    case schema::Type::Body::INT16_TYPE:
    case schema::Type::Body::INT32_TYPE:
    case schema::Type::Body::INT64_TYPE:
    case schema::Type::Body::UINT8_TYPE:
    case schema::Type::Body::UINT16_TYPE:
    case schema::Type::Body::UINT32_TYPE:
    case schema::Type::Body::UINT64_TYPE:
    case schema::Type::Body::FLOAT32_TYPE:
    case schema::Type::Body::FLOAT64_TYPE:
    case schema::Type::Body::ENUM_TYPE:
    case schema::Type::Body::STRUCT_TYPE:
    case schema::Type::Body::INTERFACE_TYPE:
      FAIL_VALIDATE_INPUT("Expected a list or blob.");
      return getFieldImpl(pool, builder, field);

    case schema::Type::Body::TEXT_TYPE:
      return DynamicValue::Builder(
          builder.initBlobField<Text>(field.getOffset() * REFERENCES, size * BYTES));

    case schema::Type::Body::DATA_TYPE:
      return DynamicValue::Builder(
          builder.initBlobField<Data>(field.getOffset() * REFERENCES, size * BYTES));

    case schema::Type::Body::LIST_TYPE: {
      auto elementType = type.getBody().getListType();
      if (elementType.getBody().which() == schema::Type::Body::STRUCT_TYPE) {
        auto structType = pool->getStruct(elementType.getBody().getStructType());
        return DynamicValue::Builder(DynamicList::Builder(
            pool, schema::Type::Body::STRUCT_TYPE, 0, structType, builder.initStructListField(
                field.getOffset() * REFERENCES, size * ELEMENTS,
                structSizeFromSchema(structType.getBody().getStructNode()))));
      } else {
        return DynamicValue::Builder(DynamicList::Builder(
            pool, elementType, builder.initListField(
                field.getOffset() * REFERENCES,
                elementSizeFor(elementType.getBody().which()),
                size * ELEMENTS)));
      }
    }

    case schema::Type::Body::OBJECT_TYPE: {
      FAIL_VALIDATE_INPUT(
          "Expected a list or blob, but found Object.  (To dynamically initialize an object "
          "field, you must pass an element type to initField().)");
      return DynamicValue::Builder();
    }
  }

  FAIL_CHECK("switch() missing case.", type.getBody().which());
  return DynamicValue::Builder();
}
DynamicValue::Builder DynamicStruct::Builder::initFieldImpl(
    const SchemaPool* pool, internal::StructBuilder builder,
    schema::StructNode::Field::Reader field,
    schema::Type::Reader type) {
  switch (type.getBody().which()) {
    case schema::Type::Body::VOID_TYPE:
    case schema::Type::Body::BOOL_TYPE:
    case schema::Type::Body::INT8_TYPE:
    case schema::Type::Body::INT16_TYPE:
    case schema::Type::Body::INT32_TYPE:
    case schema::Type::Body::INT64_TYPE:
    case schema::Type::Body::UINT8_TYPE:
    case schema::Type::Body::UINT16_TYPE:
    case schema::Type::Body::UINT32_TYPE:
    case schema::Type::Body::UINT64_TYPE:
    case schema::Type::Body::FLOAT32_TYPE:
    case schema::Type::Body::FLOAT64_TYPE:
    case schema::Type::Body::ENUM_TYPE:
    case schema::Type::Body::TEXT_TYPE:
    case schema::Type::Body::DATA_TYPE:
    case schema::Type::Body::LIST_TYPE:
    case schema::Type::Body::INTERFACE_TYPE:
      FAIL_VALIDATE_INPUT("Expected a list or blob.");
      return getFieldImpl(pool, builder, field);

    case schema::Type::Body::STRUCT_TYPE: {
      auto structType = pool->getStruct(type.getBody().getStructType());
      return DynamicValue::Builder(DynamicStruct::Builder(
          pool, structType, builder.initStructField(
              field.getOffset() * REFERENCES,
              structSizeFromSchema(structType.getBody().getStructNode()))));
    }

    case schema::Type::Body::OBJECT_TYPE: {
      FAIL_VALIDATE_INPUT(
          "Expected a struct, but found Object.  (To dynamically initialize an object "
          "field, you must pass an element type to initField().)");
      return DynamicValue::Builder();
    }
  }

  FAIL_CHECK("switch() missing case.", type.getBody().which());
  return DynamicValue::Builder();
}

// =======================================================================================

DynamicValue::Reader DynamicList::Reader::operator[](uint index) {
  PRECOND(index < size(), "List index out-of-bounds.");

  if (depth == 0) {
    switch (elementType) {
#define HANDLE_TYPE(name, discrim, typeName) \
      case schema::Type::Body::discrim##_TYPE: \
        return DynamicValue::Reader(reader.getDataElement<typeName>(index * ELEMENTS));

      HANDLE_TYPE(void, VOID, Void)
      HANDLE_TYPE(bool, BOOL, bool)
      HANDLE_TYPE(int8, INT8, int8_t)
      HANDLE_TYPE(int16, INT16, int16_t)
      HANDLE_TYPE(int32, INT32, int32_t)
      HANDLE_TYPE(int64, INT64, int64_t)
      HANDLE_TYPE(uint8, UINT8, uint8_t)
      HANDLE_TYPE(uint16, UINT16, uint16_t)
      HANDLE_TYPE(uint32, UINT32, uint32_t)
      HANDLE_TYPE(uint64, UINT64, uint64_t)
      HANDLE_TYPE(float32, FLOAT32, float)
      HANDLE_TYPE(float64, FLOAT64, double)
#undef HANDLE_TYPE

      case schema::Type::Body::TEXT_TYPE:
        return DynamicValue::Reader(reader.getBlobElement<Text>(index * ELEMENTS));
      case schema::Type::Body::DATA_TYPE:
        return DynamicValue::Reader(reader.getBlobElement<Data>(index * ELEMENTS));

      case schema::Type::Body::LIST_TYPE:
        FAIL_CHECK("elementType should not be LIST_TYPE when depth == 0.");

      case schema::Type::Body::STRUCT_TYPE:
        return DynamicValue::Reader(DynamicStruct::Reader(
            pool, elementSchema, reader.getStructElement(index * ELEMENTS)));

      case schema::Type::Body::ENUM_TYPE:
        return DynamicValue::Reader(DynamicEnum(
            pool, elementSchema, reader.getDataElement<uint16_t>(index * ELEMENTS)));

      case schema::Type::Body::OBJECT_TYPE:
        return DynamicValue::Reader(DynamicObject::Reader(
            pool, reader.getObjectElement(index * ELEMENTS)));

      case schema::Type::Body::INTERFACE_TYPE:
        FAIL_RECOVERABLE_CHECK("Interfaces not implemented.") {}
        return DynamicValue::Reader();
    }

    FAIL_CHECK("switch() missing case.", elementType);
    return DynamicValue::Reader();
  } else {
    // List of lists.
    return DynamicValue::Reader(DynamicList::Reader(
        pool, elementType, depth - 1, elementSchema,
        reader.getListElement(index * ELEMENTS,
            depth == 1 ? elementSizeFor(elementType) : internal::FieldSize::REFERENCE)));
  }
}

DynamicValue::Builder DynamicList::Builder::operator[](uint index) {
  PRECOND(index < size(), "List index out-of-bounds.");

  if (depth == 0) {
    switch (elementType) {
#define HANDLE_TYPE(name, discrim, typeName) \
      case schema::Type::Body::discrim##_TYPE: \
        return DynamicValue::Builder(builder.getDataElement<typeName>(index * ELEMENTS));

      HANDLE_TYPE(void, VOID, Void)
      HANDLE_TYPE(bool, BOOL, bool)
      HANDLE_TYPE(int8, INT8, int8_t)
      HANDLE_TYPE(int16, INT16, int16_t)
      HANDLE_TYPE(int32, INT32, int32_t)
      HANDLE_TYPE(int64, INT64, int64_t)
      HANDLE_TYPE(uint8, UINT8, uint8_t)
      HANDLE_TYPE(uint16, UINT16, uint16_t)
      HANDLE_TYPE(uint32, UINT32, uint32_t)
      HANDLE_TYPE(uint64, UINT64, uint64_t)
      HANDLE_TYPE(float32, FLOAT32, float)
      HANDLE_TYPE(float64, FLOAT64, double)
#undef HANDLE_TYPE

      case schema::Type::Body::TEXT_TYPE:
        return DynamicValue::Builder(builder.getBlobElement<Text>(index * ELEMENTS));
      case schema::Type::Body::DATA_TYPE:
        return DynamicValue::Builder(builder.getBlobElement<Data>(index * ELEMENTS));

      case schema::Type::Body::LIST_TYPE:
        FAIL_CHECK("elementType should not be LIST_TYPE when depth == 0.");
        return DynamicValue::Builder();

      case schema::Type::Body::STRUCT_TYPE:
        return DynamicValue::Builder(DynamicStruct::Builder(
            pool, elementSchema, builder.getStructElement(index * ELEMENTS)));

      case schema::Type::Body::ENUM_TYPE:
        return DynamicValue::Builder(DynamicEnum(
            pool, elementSchema, builder.getDataElement<uint16_t>(index * ELEMENTS)));

      case schema::Type::Body::OBJECT_TYPE:
        FAIL_CHECK("List(Object) not supported.");
        break;

      case schema::Type::Body::INTERFACE_TYPE:
        FAIL_RECOVERABLE_CHECK("Interfaces not implemented.") {}
        return DynamicValue::Builder();
    }

    FAIL_CHECK("switch() missing case.", elementType);
    return DynamicValue::Builder();
  } else {
    // List of lists.
    return DynamicValue::Builder(DynamicList::Builder(
        pool, elementType, depth - 1, elementSchema,
        builder.getListElement(index * ELEMENTS)));
  }
}

void DynamicList::Builder::set(uint index, DynamicValue::Reader value) {
  PRECOND(index < size(), "List index out-of-bounds.");

  if (depth == 0) {
    switch (elementType) {
#define HANDLE_TYPE(name, discrim, typeName) \
      case schema::Type::Body::discrim##_TYPE: \
        builder.setDataElement<typeName>(index * ELEMENTS, value.as<typeName>()); \
        break;

      HANDLE_TYPE(void, VOID, Void)
      HANDLE_TYPE(bool, BOOL, bool)
      HANDLE_TYPE(int8, INT8, int8_t)
      HANDLE_TYPE(int16, INT16, int16_t)
      HANDLE_TYPE(int32, INT32, int32_t)
      HANDLE_TYPE(int64, INT64, int64_t)
      HANDLE_TYPE(uint8, UINT8, uint8_t)
      HANDLE_TYPE(uint16, UINT16, uint16_t)
      HANDLE_TYPE(uint32, UINT32, uint32_t)
      HANDLE_TYPE(uint64, UINT64, uint64_t)
      HANDLE_TYPE(float32, FLOAT32, float)
      HANDLE_TYPE(float64, FLOAT64, double)
#undef HANDLE_TYPE

      case schema::Type::Body::TEXT_TYPE:
        builder.setBlobElement<Text>(index * ELEMENTS, value.as<Text>());
        break;
      case schema::Type::Body::DATA_TYPE:
        builder.setBlobElement<Data>(index * ELEMENTS, value.as<Data>());
        break;

      case schema::Type::Body::LIST_TYPE:
        FAIL_CHECK("elementType should not be LIST_TYPE when depth == 0.");
        break;

      case schema::Type::Body::STRUCT_TYPE:
        // Note we can't do a schemaless copy here because the space is already allocated.
        DynamicStruct::Builder(pool, elementSchema, builder.getStructElement(index * ELEMENTS))
            .copyFrom(value.as<DynamicStruct>());
        break;

      case schema::Type::Body::ENUM_TYPE: {
        auto enumValue = value.as<DynamicEnum>();
        VALIDATE_INPUT(elementSchema.getId() == enumValue.getSchemaNode().getId(),
                       "Type mismatch when using DynamicList::Builder::set().");
        builder.setDataElement<uint16_t>(index * ELEMENTS, value.as<DynamicEnum>().getRaw());
        break;
      }

      case schema::Type::Body::OBJECT_TYPE:
        FAIL_CHECK("List(Object) not supported.");
        break;

      case schema::Type::Body::INTERFACE_TYPE:
        FAIL_RECOVERABLE_CHECK("Interfaces not implemented.") {}
        break;
    }
  } else {
    // List of lists.
    // TODO(now):  Perform schemaless copy.
    auto listValue = value.as<DynamicList>();
    init(index, listValue.size()).as<DynamicList>().copyFrom(listValue);
  }
}

DynamicValue::Builder DynamicList::Builder::init(uint index, uint size) {
  PRECOND(index < this->size(), "List index out-of-bounds.");

  if (depth == 0) {
    switch (elementType) {
      case schema::Type::Body::VOID_TYPE:
      case schema::Type::Body::BOOL_TYPE:
      case schema::Type::Body::INT8_TYPE:
      case schema::Type::Body::INT16_TYPE:
      case schema::Type::Body::INT32_TYPE:
      case schema::Type::Body::INT64_TYPE:
      case schema::Type::Body::UINT8_TYPE:
      case schema::Type::Body::UINT16_TYPE:
      case schema::Type::Body::UINT32_TYPE:
      case schema::Type::Body::UINT64_TYPE:
      case schema::Type::Body::FLOAT32_TYPE:
      case schema::Type::Body::FLOAT64_TYPE:
      case schema::Type::Body::ENUM_TYPE:
      case schema::Type::Body::STRUCT_TYPE:
      case schema::Type::Body::INTERFACE_TYPE:
        FAIL_VALIDATE_INPUT("Expected a list or blob.");
        return DynamicValue::Builder();

      case schema::Type::Body::TEXT_TYPE:
        return DynamicValue::Builder(builder.initBlobElement<Text>(index * ELEMENTS, size * BYTES));

      case schema::Type::Body::DATA_TYPE:
        return DynamicValue::Builder(builder.initBlobElement<Data>(index * ELEMENTS, size * BYTES));

      case schema::Type::Body::LIST_TYPE:
        FAIL_CHECK("elementType should not be LIST_TYPE when depth == 0.");
        return DynamicValue::Builder();

      case schema::Type::Body::OBJECT_TYPE: {
        FAIL_CHECK("List(Object) not supported.");
        return DynamicValue::Builder();
      }
    }

    FAIL_CHECK("switch() missing case.", elementType);
    return DynamicValue::Builder();
  } else {
    // List of lists.
    internal::FieldSize elementSize = depth == 1 ?
        elementSizeFor(elementType) : internal::FieldSize::REFERENCE;

    if (elementSize == internal::FieldSize::INLINE_COMPOSITE) {
      return DynamicValue::Builder(DynamicList::Builder(
          pool, elementType, depth - 1, elementSchema, builder.initStructListElement(
              index * ELEMENTS, size * ELEMENTS,
              structSizeFromSchema(elementSchema.getBody().getStructNode()))));
    } else {
      return DynamicValue::Builder(DynamicList::Builder(
          pool, elementType, depth - 1, elementSchema, builder.initListElement(
              index * ELEMENTS, elementSizeFor(elementType), size * ELEMENTS)));
    }
  }
}

void DynamicList::Builder::copyFrom(Reader other) {
  // TODO(now): copyFrom on ListBuilder.
  // TODO(now): don't forget to check types match.
  FAIL_CHECK("Unimplemented: copyFrom()");
}

DynamicList::Reader DynamicList::Builder::asReader() {
  return DynamicList::Reader(pool, elementType, depth, elementSchema, builder.asReader());
}

DynamicList::Reader::Reader(const SchemaPool* pool, schema::Type::Reader elementType,
                            internal::ListReader reader)
    : Reader(pool, internal::ListSchema(elementType), reader) {}
DynamicList::Reader::Reader(const SchemaPool* pool, internal::ListSchema schema,
                            internal::ListReader reader)
    : pool(pool), elementType(schema.elementType), depth(schema.nestingDepth), reader(reader) {
  switch (elementType) {
    case schema::Type::Body::ENUM_TYPE:
      elementSchema = pool->getEnum(schema.elementTypeId);
      break;
    case schema::Type::Body::STRUCT_TYPE:
      elementSchema = pool->getStruct(schema.elementTypeId);
      break;
    case schema::Type::Body::INTERFACE_TYPE:
      elementSchema = pool->getInterface(schema.elementTypeId);
      break;
    default:
      // Leave schema default-initialized.
      break;
  }
}

DynamicList::Builder::Builder(const SchemaPool* pool, schema::Type::Reader elementType,
                              internal::ListBuilder builder)
    : Builder(pool, internal::ListSchema(elementType), builder) {}
DynamicList::Builder::Builder(const SchemaPool* pool, internal::ListSchema schema,
                              internal::ListBuilder builder)
    : pool(pool), elementType(schema.elementType), depth(schema.nestingDepth), builder(builder) {
  switch (elementType) {
    case schema::Type::Body::ENUM_TYPE:
      elementSchema = pool->getEnum(schema.elementTypeId);
      break;
    case schema::Type::Body::STRUCT_TYPE:
      elementSchema = pool->getStruct(schema.elementTypeId);
      break;
    case schema::Type::Body::INTERFACE_TYPE:
      elementSchema = pool->getInterface(schema.elementTypeId);
      break;
    default:
      // Leave schema default-initialized.
      break;
  }
}

void DynamicList::Reader::verifySchema(internal::ListSchema schema) {
  VALIDATE_INPUT(schema.elementType == elementType &&
                 schema.nestingDepth == depth &&
                 schema.elementTypeId == elementSchema.getId(),
                 "Type mismatch when using DynamicList::Reader::as().");
}
void DynamicList::Builder::verifySchema(internal::ListSchema schema) {
  VALIDATE_INPUT(schema.elementType == elementType &&
                 schema.nestingDepth == depth &&
                 schema.elementTypeId == elementSchema.getId(),
                 "Type mismatch when using DynamicList::Reader::as().");
}

// =======================================================================================

#define HANDLE_TYPE(name, discrim, typeName) \
ReaderFor<typeName> DynamicValue::Reader::asImpl<typeName>::apply(Reader reader) { \
  VALIDATE_INPUT(reader.type == schema::Type::Body::discrim##_TYPE, \
      "Type mismatch when using DynamicValue::Reader::as().") { \
    return ReaderFor<typeName>(); \
  } \
  return reader.name##Value; \
} \
BuilderFor<typeName> DynamicValue::Builder::asImpl<typeName>::apply(Builder builder) { \
  VALIDATE_INPUT(builder.type == schema::Type::Body::discrim##_TYPE, \
      "Type mismatch when using DynamicValue::Builder::as().") { \
    return BuilderFor<typeName>(); \
  } \
  return builder.name##Value; \
}

//HANDLE_TYPE(void, VOID, Void)
HANDLE_TYPE(bool, BOOL, bool)
HANDLE_TYPE(int8, INT8, int8_t)
HANDLE_TYPE(int16, INT16, int16_t)
HANDLE_TYPE(int32, INT32, int32_t)
HANDLE_TYPE(int64, INT64, int64_t)
HANDLE_TYPE(uint8, UINT8, uint8_t)
HANDLE_TYPE(uint16, UINT16, uint16_t)
HANDLE_TYPE(uint32, UINT32, uint32_t)
HANDLE_TYPE(uint64, UINT64, uint64_t)
HANDLE_TYPE(float32, FLOAT32, float)
HANDLE_TYPE(float64, FLOAT64, double)

HANDLE_TYPE(text, TEXT, Text)
HANDLE_TYPE(data, DATA, Data)
HANDLE_TYPE(list, LIST, DynamicList)
HANDLE_TYPE(struct, STRUCT, DynamicStruct)
HANDLE_TYPE(enum, ENUM, DynamicEnum)
HANDLE_TYPE(object, OBJECT, DynamicObject)

#undef HANDLE_TYPE

// As in the header, HANDLE_TYPE(void, VOID, Void) crashes GCC 4.7.
Void DynamicValue::Reader::asImpl<Void>::apply(Reader reader) {
  VALIDATE_INPUT(reader.type == schema::Type::Body::VOID_TYPE,
      "Type mismatch when using DynamicValue::Reader::as().") {
    return Void();
  }
  return reader.voidValue;
}
Void DynamicValue::Builder::asImpl<Void>::apply(Builder builder) {
  VALIDATE_INPUT(builder.type == schema::Type::Body::VOID_TYPE,
      "Type mismatch when using DynamicValue::Builder::as().") {
    return Void();
  }
  return builder.voidValue;
}

// =======================================================================================

template <>
DynamicStruct::Reader MessageReader::getRoot<DynamicStruct>(
    const SchemaPool& pool, uint64_t typeId) {
  return DynamicStruct::Reader(&pool, pool.getStruct(typeId), getRootInternal());
}

template <>
DynamicStruct::Builder MessageBuilder::initRoot<DynamicStruct>(
    const SchemaPool& pool, uint64_t typeId) {
  auto schema = pool.getStruct(typeId);
  return DynamicStruct::Builder(&pool, schema,
      initRoot(structSizeFromSchema(schema.getBody().getStructNode())));
}

template <>
DynamicStruct::Builder MessageBuilder::getRoot<DynamicStruct>(
    const SchemaPool& pool, uint64_t typeId) {
  auto schema = pool.getStruct(typeId);
  return DynamicStruct::Builder(&pool, schema,
      getRoot(structSizeFromSchema(schema.getBody().getStructNode())));
}

namespace internal {

DynamicStruct::Reader PointerHelpers<DynamicStruct, Kind::UNKNOWN>::get(
    StructReader reader, WireReferenceCount index, const SchemaPool& pool, uint64_t typeId) {
  return DynamicStruct::Reader(&pool, pool.getStruct(typeId),
      reader.getStructField(index, nullptr));
}
DynamicStruct::Builder PointerHelpers<DynamicStruct, Kind::UNKNOWN>::get(
    StructBuilder builder, WireReferenceCount index, const SchemaPool& pool, uint64_t typeId) {
  auto schema = pool.getStruct(typeId);
  return DynamicStruct::Builder(&pool, schema, builder.getStructField(
      index, structSizeFromSchema(schema.getBody().getStructNode()), nullptr));
}
void PointerHelpers<DynamicStruct, Kind::UNKNOWN>::set(
    StructBuilder builder, WireReferenceCount index, DynamicStruct::Reader value) {
  // TODO(now):  schemaless copy
  FAIL_CHECK("Unimplemented: copyFrom()");
}
DynamicStruct::Builder PointerHelpers<DynamicStruct, Kind::UNKNOWN>::init(
    StructBuilder builder, WireReferenceCount index, const SchemaPool& pool, uint64_t typeId) {
  auto schema = pool.getStruct(typeId);
  return DynamicStruct::Builder(&pool, schema, builder.initStructField(
      index, structSizeFromSchema(schema.getBody().getStructNode())));
}

DynamicList::Reader PointerHelpers<DynamicList, Kind::UNKNOWN>::get(
    StructReader reader, WireReferenceCount index, const SchemaPool& pool,
    schema::Type::Reader elementType) {
  return DynamicList::Reader(&pool, elementType,
      reader.getListField(index, elementSizeFor(elementType.getBody().which()), nullptr));
}
DynamicList::Builder PointerHelpers<DynamicList, Kind::UNKNOWN>::get(
    StructBuilder builder, WireReferenceCount index, const SchemaPool& pool,
    schema::Type::Reader elementType) {
  return DynamicList::Builder(&pool, elementType, builder.getListField(index, nullptr));
}
void PointerHelpers<DynamicList, Kind::UNKNOWN>::set(
    StructBuilder builder, WireReferenceCount index, DynamicList::Reader value) {
  // TODO(now):  schemaless copy
  FAIL_CHECK("Unimplemented: copyFrom()");
}
DynamicList::Builder PointerHelpers<DynamicList, Kind::UNKNOWN>::init(
    StructBuilder builder, WireReferenceCount index,
    const SchemaPool& pool, schema::Type::Reader elementType, uint size) {
  auto elementSize = elementSizeFor(elementType.getBody().which());
  if (elementSize == FieldSize::INLINE_COMPOSITE) {
    auto elementSchema = pool.getStruct(elementType.getBody().getStructType());
    return DynamicList::Builder(&pool, schema::Type::Body::STRUCT_TYPE, 0, elementSchema,
        builder.initStructListField(index, size * ELEMENTS,
            structSizeFromSchema(elementSchema.getBody().getStructNode())));
  } else {
    return DynamicList::Builder(&pool, elementType,
        builder.initListField(index, elementSize, size * ELEMENTS));
  }
}

}  // namespace internal

}  // namespace capnproto
