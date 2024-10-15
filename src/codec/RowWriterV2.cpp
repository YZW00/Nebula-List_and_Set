/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "codec/RowWriterV2.h"

#include <cmath>

#include "codec/Common.h"
#include "common/time/TimeUtils.h"
#include "common/time/WallClock.h"
#include "common/utils/DefaultValueContext.h"

namespace nebula {

using nebula::cpp2::PropertyType;

// Function used to identify the data type written
WriteResult writeItem(const Value& item, Value::Type valueType, std::string& buffer) {
  switch (valueType) {
    case Value::Type::STRING: {
      std::string str = item.getStr();
      int32_t strLen = str.size();
      buffer.append(reinterpret_cast<const char*>(&strLen), sizeof(int32_t));
      buffer.append(str.data(), strLen);
      break;
    }
    case Value::Type::INT: {
      int32_t intVal = item.getInt();
      buffer.append(reinterpret_cast<const char*>(&intVal), sizeof(int32_t));
      break;
    }
    case Value::Type::FLOAT: {
      float floatVal = item.getFloat();
      buffer.append(reinterpret_cast<const char*>(&floatVal), sizeof(float));
      break;
    }
    default:
      LOG(ERROR) << "Unsupported value type: " << static_cast<int>(valueType);
      return WriteResult::TYPE_MISMATCH;
  }
  return WriteResult::SUCCEEDED;
}

// Function used to identify List data types (List<string>, List<int>, List<float>)
template <typename List>
WriteResult writeList(const List& container, Value::Type valueType, std::string& buffer) {
  for (const auto& item : container.values) {
    if (item.type() != valueType) {
      LOG(ERROR) << "Type mismatch: Expected " << static_cast<int>(valueType) << " but got "
                 << static_cast<int>(item.type());
      return WriteResult::TYPE_MISMATCH;
    }
  }
  for (const auto& item : container.values) {
    auto result = writeItem(item, valueType, buffer);
    if (result != WriteResult::SUCCEEDED) {
      return result;
    }
  }
  return WriteResult::SUCCEEDED;
}

// Function used to identify Set data types (Set<string>, Set<int>, Set<float>)
template <typename Set>
WriteResult writeSet(const Set& container, Value::Type valueType, std::string& buffer) {
  for (const auto& item : container.values) {
    if (item.type() != valueType) {
      LOG(ERROR) << "Type mismatch: Expected " << static_cast<int>(valueType) << " but got "
                 << static_cast<int>(item.type());
      return WriteResult::TYPE_MISMATCH;
    }
  }
  std::unordered_set<Value> serialized;
  for (const auto& item : container.values) {
    if (serialized.find(item) != serialized.end()) {
      continue;
    }
    auto result = writeItem(item, valueType, buffer);
    if (result != WriteResult::SUCCEEDED) {
      return result;
    }
    serialized.insert(item);
  }
  return WriteResult::SUCCEEDED;
}

RowWriterV2::RowWriterV2(const meta::NebulaSchemaProvider* schema)
    : schema_(schema), numNullBytes_(0), approxStrLen_(0), finished_(false), outOfSpaceStr_(false) {
  CHECK(!!schema_);

  // Reserve space for the header, the data, and the string values
  buf_.reserve(schema_->size() + schema_->getNumFields() / 8 + 8 + 1024);

  char header = 0;

  // Header and schema version
  //
  // The maximum number of bytes for the header and the schema version is 8
  //
  // The first byte is the header (os signature), it has the fourth-bit (from
  // the right side) set to one (0x08), and the right three bits indicate
  // the number of bytes used for the schema version.
  //
  // If all three bits are zero, the schema version is zero. If the number
  // of schema version bytes is one, the maximum schema version that can be
  // represented is 255 (0xFF). If the number of schema version is two, the
  // maximum schema version could be 65535 (0xFFFF), and so on.
  //
  // The maximum schema version we support is 0x00FFFFFFFFFFFFFF (7 bytes)
  int64_t ver = schema_->getVersion();
  if (ver > 0) {
    if (ver <= 0x00FF) {
      header = 0x09;  // 0x08 | 0x01, one byte for the schema version
      headerLen_ = 2;
    } else if (ver < 0x00FFFF) {
      header = 0x0A;  // 0x08 | 0x02, two bytes for the schema version
      headerLen_ = 3;
    } else if (ver < 0x00FFFFFF) {
      header = 0x0B;  // 0x08 | 0x03, three bytes for the schema version
      headerLen_ = 4;
    } else if (ver < 0x00FFFFFFFF) {
      header = 0x0C;  // 0x08 | 0x04, four bytes for the schema version
      headerLen_ = 5;
    } else if (ver < 0x00FFFFFFFFFF) {
      header = 0x0D;  // 0x08 | 0x05, five bytes for the schema version
      headerLen_ = 6;
    } else if (ver < 0x00FFFFFFFFFFFF) {
      header = 0x0E;  // 0x08 | 0x06, six bytes for the schema version
      headerLen_ = 7;
    } else if (ver < 0x00FFFFFFFFFFFFFF) {
      header = 0x0F;  // 0x08 | 0x07, seven bytes for the schema version
      headerLen_ = 8;
    } else {
      LOG(FATAL) << "Schema version too big";
      header = 0x0F;  // 0x08 | 0x07, seven bytes for the schema version
      headerLen_ = 8;
    }
    buf_.append(&header, 1);
    buf_.append(reinterpret_cast<char*>(&ver), buf_[0] & 0x07);
  } else {
    header = 0x08;
    headerLen_ = 1;
    buf_.append(&header, 1);
  }

  // Null flags
  size_t numNullables = schema_->getNumNullableFields();
  if (numNullables > 0) {
    numNullBytes_ = ((numNullables - 1) >> 3) + 1;
  }

  // Reserve the space for the data, including the Null bits
  // All variant length string will be appended to the end
  buf_.resize(headerLen_ + numNullBytes_ + schema_->size(), '\0');

  isSet_.resize(schema_->getNumFields(), false);
}

RowWriterV2::RowWriterV2(const meta::NebulaSchemaProvider* schema, std::string&& encoded)
    : schema_(schema), finished_(false), outOfSpaceStr_(false) {
  auto len = encoded.size();
  buf_ = std::move(encoded).substr(0, len - sizeof(int64_t));
  processV2EncodedStr();
}

RowWriterV2::RowWriterV2(const meta::NebulaSchemaProvider* schema, const std::string& encoded)
    : schema_(schema),
      buf_(encoded.substr(0, encoded.size() - sizeof(int64_t))),
      finished_(false),
      outOfSpaceStr_(false) {
  processV2EncodedStr();
}

RowWriterV2::RowWriterV2(RowReaderWrapper& reader) : RowWriterV2(reader.getSchema()) {
  for (size_t i = 0; i < reader.numFields(); i++) {
    Value v = reader.getValueByIndex(i);
    switch (v.type()) {
      case Value::Type::NULLVALUE:
        setNull(i);
        break;
      case Value::Type::BOOL:
        set(i, v.getBool());
        break;
      case Value::Type::INT:
        set(i, v.getInt());
        break;
      case Value::Type::FLOAT:
        set(i, v.getFloat());
        break;
      case Value::Type::STRING:
        approxStrLen_ += v.getStr().size();
        set(i, v.moveStr());
        break;
      case Value::Type::DATE:
        set(i, v.moveDate());
        break;
      case Value::Type::TIME:
        set(i, v.moveTime());
        break;
      case Value::Type::DATETIME:
        set(i, v.moveDateTime());
        break;
      case Value::Type::GEOGRAPHY:
        set(i, v.moveGeography());
        break;
      case Value::Type::DURATION:
        set(i, v.moveDuration());
        break;
      case Value::Type::LIST:
        set(i, v.moveList());
        break;
      case Value::Type::SET:
        set(i, v.moveSet());
        break;
      default:
        LOG(FATAL) << "Invalid data: " << v << ", type: " << v.typeName();
        isSet_[i] = false;
        continue;
    }
    isSet_[i] = true;
  }
}

void RowWriterV2::processV2EncodedStr() {
  CHECK_EQ(0x08, buf_[0] & 0x18);
  int32_t verBytes = buf_[0] & 0x07;
  SchemaVer ver = 0;
  if (verBytes > 0) {
    memcpy(reinterpret_cast<void*>(&ver), &buf_[1], verBytes);
  }
  CHECK_EQ(ver, schema_->getVersion())
      << "The data is encoded by schema version " << ver
      << ", while the provided schema version is " << schema_->getVersion();

  headerLen_ = verBytes + 1;

  // Null flags
  size_t numNullables = schema_->getNumNullableFields();
  if (numNullables > 0) {
    numNullBytes_ = ((numNullables - 1) >> 3) + 1;
  } else {
    numNullBytes_ = 0;
  }

  approxStrLen_ = buf_.size() - headerLen_ - numNullBytes_ - schema_->size() - sizeof(int64_t);
  isSet_.resize(schema_->getNumFields(), true);
}

void RowWriterV2::setNullBit(ssize_t pos) {
  static const uint8_t orBits[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

  size_t offset = headerLen_ + (pos >> 3);
  buf_[offset] = buf_[offset] | orBits[pos & 0x0000000000000007L];
}

void RowWriterV2::clearNullBit(ssize_t pos) {
  static const uint8_t andBits[] = {0x7F, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE};

  size_t offset = headerLen_ + (pos >> 3);
  buf_[offset] = buf_[offset] & andBits[pos & 0x0000000000000007L];
}

bool RowWriterV2::checkNullBit(ssize_t pos) const {
  static const uint8_t bits[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};

  size_t offset = headerLen_ + (pos >> 3);
  int8_t flag = buf_[offset] & bits[pos & 0x0000000000000007L];
  return flag != 0;
}

WriteResult RowWriterV2::setValue(ssize_t index, const Value& val) {
  CHECK(!finished_) << "You have called finish()";
  if (index < 0 || static_cast<size_t>(index) >= schema_->getNumFields()) {
    return WriteResult::UNKNOWN_FIELD;
  }

  switch (val.type()) {
    case Value::Type::NULLVALUE: {
      if (val.isBadNull()) {
        // Property value never be bad null
        return WriteResult::TYPE_MISMATCH;
      }
      return setNull(index);
    }
    case Value::Type::BOOL:
      return write(index, val.getBool());
    case Value::Type::INT:
      return write(index, val.getInt());
    case Value::Type::FLOAT:
      return write(index, val.getFloat());
    case Value::Type::STRING:
      return write(index, val.getStr());
    case Value::Type::DATE:
      return write(index, val.getDate());
    case Value::Type::TIME:
      return write(index, val.getTime());
    case Value::Type::DATETIME:
      return write(index, val.getDateTime());
    case Value::Type::GEOGRAPHY:
      return write(index, val.getGeography());
    case Value::Type::DURATION:
      return write(index, val.getDuration());
    case Value::Type::LIST:
      return write(index, val.getList());
    case Value::Type::SET:
      return write(index, val.getSet());
    default:
      return WriteResult::TYPE_MISMATCH;
  }
}
WriteResult RowWriterV2::setValue(const std::string& name, const Value& val) {
  CHECK(!finished_) << "You have called finish()";
  int64_t index = schema_->getFieldIndex(name);
  return setValue(index, val);
}

WriteResult RowWriterV2::setNull(ssize_t index) {
  CHECK(!finished_) << "You have called finish()";
  if (index < 0 || static_cast<size_t>(index) >= schema_->getNumFields()) {
    return WriteResult::UNKNOWN_FIELD;
  }

  // Make sure the field is nullable
  auto field = schema_->field(index);
  if (!field->nullable()) {
    return WriteResult::NOT_NULLABLE;
  }

  setNullBit(field->nullFlagPos());
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::setNull(const std::string& name) {
  CHECK(!finished_) << "You have called finish()";
  int64_t index = schema_->getFieldIndex(name);
  return setNull(index);
}

WriteResult RowWriterV2::write(ssize_t index, bool v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::BOOL:
    case PropertyType::INT8:
      buf_[offset] = v ? 0x01 : 0;
      break;
    case PropertyType::INT64:
      buf_[offset + 7] = 0;
      buf_[offset + 6] = 0;
      buf_[offset + 5] = 0;
      buf_[offset + 4] = 0;  // fallthrough
    case PropertyType::INT32:
      buf_[offset + 3] = 0;
      buf_[offset + 2] = 0;  // fallthrough
    case PropertyType::INT16:
      buf_[offset + 1] = 0;
      buf_[offset + 0] = v ? 0x01 : 0;
      break;
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, float v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::INT8: {
      if (v > std::numeric_limits<int8_t>::max() || v < std::numeric_limits<int8_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int8_t iv = std::round(v);
      buf_[offset] = iv;
      break;
    }
    case PropertyType::INT16: {
      if (v > std::numeric_limits<int16_t>::max() || v < std::numeric_limits<int16_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int16_t iv = std::round(v);
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int16_t));
      break;
    }
    case PropertyType::INT32: {
      if (v > static_cast<float>(std::numeric_limits<int32_t>::max()) ||
          v < static_cast<float>(std::numeric_limits<int32_t>::min())) {
        return WriteResult::OUT_OF_RANGE;
      }
      int32_t iv = std::round(v);
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int32_t));
      break;
    }
    case PropertyType::INT64: {
      if (v > static_cast<float>(std::numeric_limits<int64_t>::max()) ||
          v < static_cast<float>(std::numeric_limits<int64_t>::min())) {
        return WriteResult::OUT_OF_RANGE;
      }
      int64_t iv = std::round(v);
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int64_t));
      break;
    }
    case PropertyType::FLOAT: {
      memcpy(&buf_[offset], reinterpret_cast<void*>(&v), sizeof(float));
      break;
    }
    case PropertyType::DOUBLE: {
      double dv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&dv), sizeof(double));
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, double v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::INT8: {
      if (v > std::numeric_limits<int8_t>::max() || v < std::numeric_limits<int8_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int8_t iv = std::round(v);
      buf_[offset] = iv;
      break;
    }
    case PropertyType::INT16: {
      if (v > std::numeric_limits<int16_t>::max() || v < std::numeric_limits<int16_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int16_t iv = std::round(v);
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int16_t));
      break;
    }
    case PropertyType::INT32: {
      if (v > std::numeric_limits<int32_t>::max() || v < std::numeric_limits<int32_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int32_t iv = std::round(v);
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int32_t));
      break;
    }
    case PropertyType::INT64: {
      if (v > static_cast<double>(std::numeric_limits<int64_t>::max()) ||
          v < static_cast<double>(std::numeric_limits<int64_t>::min())) {
        return WriteResult::OUT_OF_RANGE;
      }
      int64_t iv = std::round(v);
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int64_t));
      break;
    }
    case PropertyType::FLOAT: {
      if (v > std::numeric_limits<float>::max() || v < std::numeric_limits<float>::lowest()) {
        return WriteResult::OUT_OF_RANGE;
      }
      float fv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&fv), sizeof(float));
      break;
    }
    case PropertyType::DOUBLE: {
      memcpy(&buf_[offset], reinterpret_cast<void*>(&v), sizeof(double));
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, uint8_t v) {
  return write(index, static_cast<int8_t>(v));
}

WriteResult RowWriterV2::write(ssize_t index, int8_t v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::BOOL: {
      buf_[offset] = v == 0 ? 0x00 : 0x01;
      break;
    }
    case PropertyType::INT8: {
      buf_[offset] = v;
      break;
    }
    case PropertyType::INT16: {
      int16_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int16_t));
      break;
    }
    case PropertyType::INT32: {
      int32_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int32_t));
      break;
    }
    case PropertyType::INT64: {
      int64_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int64_t));
      break;
    }
    case PropertyType::FLOAT: {
      float fv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&fv), sizeof(float));
      break;
    }
    case PropertyType::DOUBLE: {
      double dv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&dv), sizeof(double));
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, uint16_t v) {
  return write(index, static_cast<int16_t>(v));
}

WriteResult RowWriterV2::write(ssize_t index, int16_t v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::BOOL: {
      buf_[offset] = v == 0 ? 0x00 : 0x01;
      break;
    }
    case PropertyType::INT8: {
      if (v > std::numeric_limits<int8_t>::max() || v < std::numeric_limits<int8_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int8_t iv = v;
      buf_[offset] = iv;
      break;
    }
    case PropertyType::INT16: {
      memcpy(&buf_[offset], reinterpret_cast<void*>(&v), sizeof(int16_t));
      break;
    }
    case PropertyType::INT32: {
      int32_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int32_t));
      break;
    }
    case PropertyType::INT64: {
      int64_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int64_t));
      break;
    }
    case PropertyType::FLOAT: {
      float fv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&fv), sizeof(float));
      break;
    }
    case PropertyType::DOUBLE: {
      double dv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&dv), sizeof(double));
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, uint32_t v) {
  return write(index, static_cast<int32_t>(v));
}

WriteResult RowWriterV2::write(ssize_t index, int32_t v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::BOOL: {
      buf_[offset] = v == 0 ? 0x00 : 0x01;
      break;
    }
    case PropertyType::INT8: {
      if (v > std::numeric_limits<int8_t>::max() || v < std::numeric_limits<int8_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int8_t iv = v;
      buf_[offset] = iv;
      break;
    }
    case PropertyType::INT16: {
      if (v > std::numeric_limits<int16_t>::max() || v < std::numeric_limits<int16_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int16_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int16_t));
      break;
    }
    case PropertyType::INT32: {
      memcpy(&buf_[offset], reinterpret_cast<void*>(&v), sizeof(int32_t));
      break;
    }
    case PropertyType::TIMESTAMP: {
      // 32-bit timestamp can only support upto 2038-01-19
      auto ret = time::TimeUtils::toTimestamp(v);
      if (!ret.ok()) {
        return WriteResult::OUT_OF_RANGE;
      }
      auto ts = ret.value().getInt();
      memcpy(&buf_[offset], reinterpret_cast<void*>(&ts), sizeof(int64_t));
      break;
    }
    case PropertyType::INT64: {
      int64_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int64_t));
      break;
    }
    case PropertyType::FLOAT: {
      float fv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&fv), sizeof(float));
      break;
    }
    case PropertyType::DOUBLE: {
      double dv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&dv), sizeof(double));
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, uint64_t v) {
  return write(index, static_cast<int64_t>(v));
}

WriteResult RowWriterV2::write(ssize_t index, int64_t v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::BOOL: {
      buf_[offset] = v == 0 ? 0x00 : 0x01;
      break;
    }
    case PropertyType::INT8: {
      if (v > std::numeric_limits<int8_t>::max() || v < std::numeric_limits<int8_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int8_t iv = v;
      buf_[offset] = iv;
      break;
    }
    case PropertyType::INT16: {
      if (v > std::numeric_limits<int16_t>::max() || v < std::numeric_limits<int16_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int16_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int16_t));
      break;
    }
    case PropertyType::INT32: {
      if (v > std::numeric_limits<int32_t>::max() || v < std::numeric_limits<int32_t>::min()) {
        return WriteResult::OUT_OF_RANGE;
      }
      int32_t iv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&iv), sizeof(int32_t));
      break;
    }
    case PropertyType::TIMESTAMP: {
      // 64-bit timestamp has way broader time range
      auto ret = time::TimeUtils::toTimestamp(v);
      if (!ret.ok()) {
        return WriteResult::OUT_OF_RANGE;
      }
      auto ts = ret.value().getInt();
      memcpy(&buf_[offset], reinterpret_cast<void*>(&ts), sizeof(int64_t));
      break;
    }
    case PropertyType::INT64: {
      memcpy(&buf_[offset], reinterpret_cast<void*>(&v), sizeof(int64_t));
      break;
    }
    case PropertyType::FLOAT: {
      float fv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&fv), sizeof(float));
      break;
    }
    case PropertyType::DOUBLE: {
      double dv = v;
      memcpy(&buf_[offset], reinterpret_cast<void*>(&dv), sizeof(double));
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const std::string& v) {
  return write(index, folly::StringPiece(v));
}

WriteResult RowWriterV2::write(ssize_t index, const char* v) {
  return write(index, folly::StringPiece(v));
}

WriteResult RowWriterV2::write(ssize_t index, folly::StringPiece v, bool isWKB) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::GEOGRAPHY: {
      // If v is a not a WKB string, we need report error.
      if (!isWKB) {
        return WriteResult::TYPE_MISMATCH;
      }
      [[fallthrough]];
    }
    case PropertyType::STRING: {
      if (isSet_[index]) {
        // The string value has already been set, we need to turn it
        // into out-of-space strings then
        outOfSpaceStr_ = true;
      }

      int32_t strOffset;
      int32_t strLen;
      if (outOfSpaceStr_) {
        strList_.emplace_back(v.data(), v.size());
        strOffset = 0;
        // Length field is the index to the out-of-space string list
        strLen = strList_.size() - 1;
      } else {
        // Append to the end
        strOffset = buf_.size();
        strLen = v.size();
        buf_.append(v.data(), strLen);
      }
      memcpy(&buf_[offset], reinterpret_cast<void*>(&strOffset), sizeof(int32_t));
      memcpy(&buf_[offset + sizeof(int32_t)], reinterpret_cast<void*>(&strLen), sizeof(int32_t));
      approxStrLen_ += v.size();
      break;
    }
    case PropertyType::FIXED_STRING: {
      // In-place string. If the pass-in string is longer than the pre-defined
      // fixed length, the string will be truncated to the fixed length
      size_t len = v.size() > field->size() ? utf8CutSize(v, field->size()) : v.size();
      strncpy(&buf_[offset], v.data(), len);
      if (len < field->size()) {
        memset(&buf_[offset + len], 0, field->size() - len);
      }
      break;
    }
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const Date& v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::DATE:
      memcpy(&buf_[offset], reinterpret_cast<const void*>(&v.year), sizeof(int16_t));
      buf_[offset + sizeof(int16_t)] = v.month;
      buf_[offset + sizeof(int16_t) + sizeof(int8_t)] = v.day;
      break;
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const Time& v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::TIME:
      buf_[offset] = v.hour;
      buf_[offset + sizeof(int8_t)] = v.minute;
      buf_[offset + 2 * sizeof(int8_t)] = v.sec;
      memcpy(&buf_[offset + 3 * sizeof(int8_t)],
             reinterpret_cast<const void*>(&v.microsec),
             sizeof(int32_t));
      break;
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const DateTime& v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  int16_t year = v.year;
  int8_t month = v.month;
  int8_t day = v.day;
  int8_t hour = v.hour;
  int8_t minute = v.minute;
  int8_t sec = v.sec;
  int32_t microsec = v.microsec;
  switch (field->type()) {
    case PropertyType::DATETIME:
      memcpy(&buf_[offset], reinterpret_cast<const void*>(&year), sizeof(int16_t));
      buf_[offset + sizeof(int16_t)] = month;
      buf_[offset + sizeof(int16_t) + sizeof(int8_t)] = day;
      buf_[offset + sizeof(int16_t) + 2 * sizeof(int8_t)] = hour;
      buf_[offset + sizeof(int16_t) + 3 * sizeof(int8_t)] = minute;
      buf_[offset + sizeof(int16_t) + 4 * sizeof(int8_t)] = sec;
      memcpy(&buf_[offset + sizeof(int16_t) + 5 * sizeof(int8_t)],
             reinterpret_cast<const void*>(&microsec),
             sizeof(int32_t));
      break;
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const Duration& v) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  switch (field->type()) {
    case PropertyType::DURATION:
      memcpy(&buf_[offset], reinterpret_cast<const void*>(&v.seconds), sizeof(int64_t));
      memcpy(&buf_[offset + sizeof(int64_t)],
             reinterpret_cast<const void*>(&v.microseconds),
             sizeof(int32_t));
      memcpy(&buf_[offset + sizeof(int64_t) + sizeof(int32_t)],
             reinterpret_cast<const void*>(&v.months),
             sizeof(int32_t));
      break;
    default:
      return WriteResult::TYPE_MISMATCH;
  }
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const Geography& v) {
  auto field = schema_->field(index);
  auto geoShape = field->geoShape();
  if (geoShape != meta::cpp2::GeoShape::ANY &&
      folly::to<uint32_t>(geoShape) != folly::to<uint32_t>(v.shape())) {
    return WriteResult::TYPE_MISMATCH;
  }
  // Geography is stored as WKB format.
  // WKB is a binary string.
  std::string wkb = v.asWKB();
  return write(index, folly::StringPiece(wkb), true);
}

WriteResult RowWriterV2::write(ssize_t index, const List& list) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  if (isSet_[index]) {
    outOfSpaceStr_ = true;
  }
  int32_t listSize = list.size();
  int32_t listOffset = buf_.size();
  buf_.append(reinterpret_cast<const char*>(&listSize), sizeof(int32_t));
  Value::Type valueType;
  if (field->type() == PropertyType::LIST_STRING) {
    valueType = Value::Type::STRING;
  } else if (field->type() == PropertyType::LIST_INT) {
    valueType = Value::Type::INT;
  } else if (field->type() == PropertyType::LIST_FLOAT) {
    valueType = Value::Type::FLOAT;
  } else {
    LOG(ERROR) << "Unsupported list type: " << static_cast<int>(field->type());
    return WriteResult::TYPE_MISMATCH;
  }
  auto result = writeList(list, valueType, buf_);
  if (result != WriteResult::SUCCEEDED) {
    return result;
  }
  memcpy(&buf_[offset], reinterpret_cast<void*>(&listOffset), sizeof(int32_t));
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::write(ssize_t index, const Set& set) {
  auto field = schema_->field(index);
  auto offset = headerLen_ + numNullBytes_ + field->offset();
  if (isSet_[index]) {
    outOfSpaceStr_ = true;
  }
  int32_t setSize = set.size();
  int32_t setOffset = buf_.size();
  buf_.append(reinterpret_cast<const char*>(&setSize), sizeof(int32_t));
  Value::Type valueType;
  if (field->type() == PropertyType::SET_STRING) {
    valueType = Value::Type::STRING;
  } else if (field->type() == PropertyType::SET_INT) {
    valueType = Value::Type::INT;
  } else if (field->type() == PropertyType::SET_FLOAT) {
    valueType = Value::Type::FLOAT;
  } else {
    LOG(ERROR) << "Unsupported set type: " << static_cast<int>(field->type());
    return WriteResult::TYPE_MISMATCH;
  }
  auto result = writeSet(set, valueType, buf_);
  if (result != WriteResult::SUCCEEDED) {
    return result;
  }
  memcpy(&buf_[offset], reinterpret_cast<void*>(&setOffset), sizeof(int32_t));
  if (field->nullable()) {
    clearNullBit(field->nullFlagPos());
  }
  isSet_[index] = true;
  return WriteResult::SUCCEEDED;
}

WriteResult RowWriterV2::checkUnsetFields() {
  DefaultValueContext expCtx;
  for (size_t i = 0; i < schema_->getNumFields(); i++) {
    if (!isSet_[i]) {
      auto field = schema_->field(i);
      if (!field->nullable() && !field->hasDefault()) {
        // The field neither can be NULL, nor has a default value
        return WriteResult::FIELD_UNSET;
      }

      WriteResult r = WriteResult::SUCCEEDED;
      if (field->hasDefault()) {
        ObjectPool pool;
        auto& exprStr = field->defaultValue();
        auto expr = Expression::decode(&pool, folly::StringPiece(exprStr.data(), exprStr.size()));
        auto defVal = Expression::eval(expr, expCtx);
        switch (defVal.type()) {
          case Value::Type::NULLVALUE:
            setNullBit(field->nullFlagPos());
            break;
          case Value::Type::BOOL:
            r = write(i, defVal.getBool());
            break;
          case Value::Type::INT:
            r = write(i, defVal.getInt());
            break;
          case Value::Type::FLOAT:
            r = write(i, defVal.getFloat());
            break;
          case Value::Type::STRING:
            r = write(i, defVal.getStr());
            break;
          case Value::Type::DATE:
            r = write(i, defVal.getDate());
            break;
          case Value::Type::TIME:
            r = write(i, defVal.getTime());
            break;
          case Value::Type::DATETIME:
            r = write(i, defVal.getDateTime());
            break;
          case Value::Type::GEOGRAPHY:
            r = write(i, defVal.getGeography());
            break;
          case Value::Type::DURATION:
            r = write(i, defVal.getDuration());
            break;
          case Value::Type::LIST:
            r = write(i, defVal.getList());
            break;
          case Value::Type::SET:
            r = write(i, defVal.getSet());
            break;
          default:
            LOG(FATAL) << "Unsupported default value type: " << defVal.typeName()
                       << ", default value: " << defVal
                       << ", default value expr: " << field->defaultValue();
            return WriteResult::TYPE_MISMATCH;
        }
      } else {
        // Set NULL
        setNullBit(field->nullFlagPos());
      }

      if (r != WriteResult::SUCCEEDED) {
        return r;
      }
    }
  }

  return WriteResult::SUCCEEDED;
}

std::string RowWriterV2::processOutOfSpace() {
  std::string temp;
  // Reserve enough space to avoid memory re-allocation
  temp.reserve(headerLen_ + numNullBytes_ + schema_->size() + approxStrLen_ + sizeof(int64_t));
  // Copy the data except the strings
  temp.append(buf_.data(), headerLen_ + numNullBytes_ + schema_->size());

  // Now let's process all strings
  for (size_t i = 0; i < schema_->getNumFields(); i++) {
    auto field = schema_->field(i);
    if (field->type() != PropertyType::STRING && field->type() != PropertyType::GEOGRAPHY) {
      continue;
    }

    size_t offset = headerLen_ + numNullBytes_ + field->offset();
    int32_t oldOffset;
    int32_t newOffset = temp.size();
    int32_t strLen;

    if (field->nullable() && checkNullBit(field->nullFlagPos())) {
      // Null string
      newOffset = strLen = 0;
    } else {
      // load the old offset and string length
      memcpy(reinterpret_cast<void*>(&oldOffset), &buf_[offset], sizeof(int32_t));
      memcpy(reinterpret_cast<void*>(&strLen), &buf_[offset + sizeof(int32_t)], sizeof(int32_t));

      if (oldOffset > 0) {
        temp.append(&buf_[oldOffset], strLen);
      } else {
        // Out of space string
        CHECK_LT(strLen, strList_.size());
        temp.append(strList_[strLen]);
        strLen = strList_[strLen].size();
      }
    }

    // Set the new offset and length
    memcpy(&temp[offset], reinterpret_cast<void*>(&newOffset), sizeof(int32_t));
    memcpy(&temp[offset + sizeof(int32_t)], reinterpret_cast<void*>(&strLen), sizeof(int32_t));
  }
  return temp;
}

WriteResult RowWriterV2::finish() {
  CHECK(!finished_) << "You have called finish()";

  // First to check whether all fields are set. If not, to check whether
  // it can be NULL or there is a default value for the field
  WriteResult res = checkUnsetFields();
  if (res != WriteResult::SUCCEEDED) {
    return res;
  }

  // Next to process out-of-space strings
  if (outOfSpaceStr_) {
    buf_ = processOutOfSpace();
  }

  // The timestamp will be saved to the tail of buf_
  auto ts = time::WallClock::fastNowInMicroSec();
  buf_.append(reinterpret_cast<char*>(&ts), sizeof(int64_t));

  finished_ = true;
  return WriteResult::SUCCEEDED;
}

}  // namespace nebula
