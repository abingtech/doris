// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
// This file is copied from
// https://github.com/ClickHouse/ClickHouse/blob/master/src/Columns/ColumnArray.cpp
// and modified by Doris

#include "vec/columns/column_array.h"

#include <algorithm>
#include <boost/iterator/iterator_facade.hpp>
#include <cstring>
#include <vector>

#include "common/status.h"
#include "runtime/primitive_type.h"
#include "vec/columns/column_const.h"
#include "vec/columns/column_decimal.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_string.h"
#include "vec/columns/columns_common.h"
#include "vec/common/arena.h"
#include "vec/common/assert_cast.h"
#include "vec/common/memcpy_small.h"
#include "vec/common/typeid_cast.h"
#include "vec/common/unaligned.h"
#include "vec/data_types/data_type.h"

class SipHash;

namespace doris::vectorized {

ColumnArray::ColumnArray(MutableColumnPtr&& nested_column, MutableColumnPtr&& offsets_column)
        : data(std::move(nested_column)), offsets(std::move(offsets_column)) {
    data = data->convert_to_full_column_if_const();
    offsets = offsets->convert_to_full_column_if_const();
    const auto* offsets_concrete = typeid_cast<const ColumnOffsets*>(offsets.get());

    if (!offsets_concrete) {
        throw doris::Exception(ErrorCode::INTERNAL_ERROR, "offsets_column must be a ColumnUInt64");
        __builtin_unreachable();
    }

    if (!offsets_concrete->empty() && data) {
        auto last_offset = offsets_concrete->get_data().back();

        /// This will also prevent possible overflow in offset.
        if (data->size() != last_offset) {
            throw doris::Exception(
                    ErrorCode::INTERNAL_ERROR,
                    "nested_column's size {}, is not consistent with offsets_column's {}",
                    data->size(), last_offset);
        }
    }

    /** NOTE
      * Arrays with constant value are possible and used in implementation of higher order functions (see FunctionReplicate).
      * But in most cases, arrays with constant value are unexpected and code will work wrong. Use with caution.
      */
}

ColumnArray::ColumnArray(MutableColumnPtr&& nested_column) : data(std::move(nested_column)) {
    data = data->convert_to_full_column_if_const();
    if (!data->empty()) {
        throw doris::Exception(ErrorCode::INTERNAL_ERROR,
                               "Not empty data passed to ColumnArray, but no offsets passed");
        __builtin_unreachable();
    }
    offsets = ColumnOffsets::create();
}

void ColumnArray::shrink_padding_chars() {
    data->shrink_padding_chars();
}

std::string ColumnArray::get_name() const {
    return "Array(" + get_data().get_name() + ")";
}

MutableColumnPtr ColumnArray::clone_resized(size_t to_size) const {
    auto res = ColumnArray::create(get_data().clone_empty());

    if (to_size == 0) return res;
    size_t from_size = size();

    if (to_size <= from_size) {
        /// Just cut column.
        res->get_offsets().assign(get_offsets().begin(), get_offsets().begin() + to_size);
        res->get_data().insert_range_from(get_data(), 0, get_offsets()[to_size - 1]);
    } else {
        /// Copy column and append empty arrays for extra elements.
        Offset64 offset = 0;
        if (from_size > 0) {
            res->get_offsets().assign(get_offsets().begin(), get_offsets().end());
            res->get_data().insert_range_from(get_data(), 0, get_data().size());
            offset = get_offsets().back();
        }

        res->get_offsets().resize(to_size);
        for (size_t i = from_size; i < to_size; ++i) res->get_offsets()[i] = offset;
    }

    return res;
}

size_t ColumnArray::size() const {
    return get_offsets().size();
}

Field ColumnArray::operator[](size_t n) const {
    size_t offset = offset_at(n);
    size_t size = size_at(n);

    if (size > max_array_size_as_field)
        throw doris::Exception(ErrorCode::INVALID_ARGUMENT,
                               "Array of size {} in row {}, is too large to be manipulated as "
                               "single field, maximum size {}",
                               size, n, max_array_size_as_field);

    Array res(size);

    for (size_t i = 0; i < size; ++i) res[i] = get_data()[offset + i];

    return Field::create_field<TYPE_ARRAY>(res);
}

void ColumnArray::get(size_t n, Field& res) const {
    size_t offset = offset_at(n);
    size_t size = size_at(n);

    if (size > max_array_size_as_field)
        throw doris::Exception(ErrorCode::INVALID_ARGUMENT,
                               "Array of size {} in row {}, is too large to be manipulated as "
                               "single field, maximum size {}",
                               size, n, max_array_size_as_field);

    res = Field::create_field<TYPE_ARRAY>(Array(size));
    Array& res_arr = doris::vectorized::get<Array&>(res);

    for (size_t i = 0; i < size; ++i) get_data().get(offset + i, res_arr[i]);
}

bool ColumnArray::is_default_at(size_t n) const {
    const auto& offsets_data = get_offsets();
    return offsets_data[n] == offsets_data[static_cast<ssize_t>(n) - 1];
}

StringRef ColumnArray::serialize_value_into_arena(size_t n, Arena& arena,
                                                  char const*& begin) const {
    size_t array_size = size_at(n);
    size_t offset = offset_at(n);

    char* pos = arena.alloc_continue(sizeof(array_size), begin);
    memcpy(pos, &array_size, sizeof(array_size));

    StringRef res(pos, sizeof(array_size));

    for (size_t i = 0; i < array_size; ++i) {
        auto value_ref = get_data().serialize_value_into_arena(offset + i, arena, begin);
        res.data = value_ref.data - res.size;
        res.size += value_ref.size;
    }

    return res;
}

template <bool positive>
struct less {
    const ColumnArray& parent;
    const int nan_direction_hint;
    explicit less(const ColumnArray& parent_, int nan_direction_hint_)
            : parent(parent_), nan_direction_hint(nan_direction_hint_) {}
    bool operator()(size_t lhs, size_t rhs) const {
        size_t lhs_size = parent.size_at(lhs);
        size_t rhs_size = parent.size_at(rhs);
        size_t min_size = std::min(lhs_size, rhs_size);
        int res = 0;
        for (size_t i = 0; i < min_size; ++i) {
            if (res = parent.get_data().compare_at(
                        parent.offset_at(lhs) + i, parent.offset_at(rhs) + i,
                        *parent.get_data_ptr().get(), nan_direction_hint);
                res) {
                // if res != 0 , here is something different ,just return
                break;
            }
        }
        if (res == 0) {
            // then we check size of array
            res = lhs_size < rhs_size ? -1 : (lhs_size == rhs_size ? 0 : 1);
        }

        return positive ? (res < 0) : (res > 0);
    }
};

void ColumnArray::get_permutation(bool reverse, size_t limit, int nan_direction_hint,
                                  IColumn::Permutation& res) const {
    size_t s = size();
    res.resize(s);
    for (size_t i = 0; i < s; ++i) {
        res[i] = i;
    }

    if (reverse) {
        pdqsort(res.begin(), res.end(), less<false>(*this, nan_direction_hint));
    } else {
        pdqsort(res.begin(), res.end(), less<true>(*this, nan_direction_hint));
    }
}

int ColumnArray::compare_at(size_t n, size_t m, const IColumn& rhs_, int nan_direction_hint) const {
    // since column type is complex, we can't use this function
    const auto& rhs = assert_cast<const ColumnArray&, TypeCheckOnRelease::DISABLE>(rhs_);

    size_t lhs_size = size_at(n);
    size_t rhs_size = rhs.size_at(m);
    size_t min_size = std::min(lhs_size, rhs_size);
    for (size_t i = 0; i < min_size; ++i) {
        if (int res = get_data().compare_at(offset_at(n) + i, rhs.offset_at(m) + i, *rhs.data.get(),
                                            nan_direction_hint);
            res) {
            // if res != 0 , here is something different ,just return
            return res;
        }
    }

    // then we check size of array
    return lhs_size < rhs_size ? -1 : (lhs_size == rhs_size ? 0 : 1);
}

const char* ColumnArray::deserialize_and_insert_from_arena(const char* pos) {
    size_t array_size = unaligned_load<size_t>(pos);
    pos += sizeof(array_size);

    for (size_t i = 0; i < array_size; ++i) pos = get_data().deserialize_and_insert_from_arena(pos);

    get_offsets().push_back(get_offsets().back() + array_size);
    return pos;
}

void ColumnArray::update_hash_with_value(size_t n, SipHash& hash) const {
    size_t array_size = size_at(n);
    size_t offset = offset_at(n);

    for (size_t i = 0; i < array_size; ++i) get_data().update_hash_with_value(offset + i, hash);
}

// for every array row calculate xxHash
void ColumnArray::update_xxHash_with_value(size_t start, size_t end, uint64_t& hash,
                                           const uint8_t* __restrict null_data) const {
    auto& offsets_column = get_offsets();
    if (null_data) {
        for (size_t i = start; i < end; ++i) {
            if (null_data[i] == 0) {
                size_t elem_size = offsets_column[i] - offsets_column[i - 1];
                if (elem_size == 0) {
                    hash = HashUtil::xxHash64WithSeed(reinterpret_cast<const char*>(&elem_size),
                                                      sizeof(elem_size), hash);
                } else {
                    get_data().update_xxHash_with_value(offsets_column[i - 1], offsets_column[i],
                                                        hash, nullptr);
                }
            }
        }
    } else {
        for (size_t i = start; i < end; ++i) {
            size_t elem_size = offsets_column[i] - offsets_column[i - 1];
            if (elem_size == 0) {
                hash = HashUtil::xxHash64WithSeed(reinterpret_cast<const char*>(&elem_size),
                                                  sizeof(elem_size), hash);
            } else {
                get_data().update_xxHash_with_value(offsets_column[i - 1], offsets_column[i], hash,
                                                    nullptr);
            }
        }
    }
}

// for every array row calculate crcHash
void ColumnArray::update_crc_with_value(size_t start, size_t end, uint32_t& hash,
                                        const uint8_t* __restrict null_data) const {
    auto& offsets_column = get_offsets();
    if (null_data) {
        for (size_t i = start; i < end; ++i) {
            if (null_data[i] == 0) {
                size_t elem_size = offsets_column[i] - offsets_column[i - 1];
                if (elem_size == 0) {
                    hash = HashUtil::zlib_crc_hash(reinterpret_cast<const char*>(&elem_size),
                                                   sizeof(elem_size), hash);
                } else {
                    get_data().update_crc_with_value(offsets_column[i - 1], offsets_column[i], hash,
                                                     nullptr);
                }
            }
        }
    } else {
        for (size_t i = start; i < end; ++i) {
            size_t elem_size = offsets_column[i] - offsets_column[i - 1];
            if (elem_size == 0) {
                hash = HashUtil::zlib_crc_hash(reinterpret_cast<const char*>(&elem_size),
                                               sizeof(elem_size), hash);
            } else {
                get_data().update_crc_with_value(offsets_column[i - 1], offsets_column[i], hash,
                                                 nullptr);
            }
        }
    }
}

void ColumnArray::update_hashes_with_value(uint64_t* __restrict hashes,
                                           const uint8_t* __restrict null_data) const {
    auto s = size();
    if (null_data) {
        for (size_t i = 0; i < s; ++i) {
            if (null_data[i] == 0) {
                update_xxHash_with_value(i, i + 1, hashes[i], nullptr);
            }
        }
    } else {
        for (size_t i = 0; i < s; ++i) {
            update_xxHash_with_value(i, i + 1, hashes[i], nullptr);
        }
    }
}

void ColumnArray::update_crcs_with_value(uint32_t* __restrict hash, PrimitiveType type,
                                         uint32_t rows, uint32_t offset,
                                         const uint8_t* __restrict null_data) const {
    auto s = rows;
    DCHECK(s == size());

    if (null_data) {
        for (size_t i = 0; i < s; ++i) {
            // every row
            if (null_data[i] == 0) {
                update_crc_with_value(i, i + 1, hash[i], nullptr);
            }
        }
    } else {
        for (size_t i = 0; i < s; ++i) {
            update_crc_with_value(i, i + 1, hash[i], nullptr);
        }
    }
}

void ColumnArray::insert(const Field& x) {
    DCHECK_EQ(x.get_type(), PrimitiveType::TYPE_ARRAY);
    if (x.is_null()) {
        get_data().insert(Field::create_field<TYPE_NULL>(Null()));
        get_offsets().push_back(get_offsets().back() + 1);
    } else {
        const auto& array = doris::vectorized::get<const Array&>(x);
        size_t size = array.size();
        for (size_t i = 0; i < size; ++i) {
            get_data().insert(array[i]);
        }
        get_offsets().push_back(get_offsets().back() + size);
    }
}

void ColumnArray::insert_from(const IColumn& src_, size_t n) {
    DCHECK_LT(n, src_.size());
    const ColumnArray& src = assert_cast<const ColumnArray&>(src_);
    size_t size = src.size_at(n);
    size_t offset = src.offset_at(n);

    if (!get_data().is_nullable() && src.get_data().is_nullable()) {
        // Note: we can't process the case of 'Array(Nullable(nest))'
        throw Exception(ErrorCode::INTERNAL_ERROR, "insert '{}' into '{}'", src.get_name(),
                        get_name());
    } else if (get_data().is_nullable() && !src.get_data().is_nullable()) {
        // Note: here we should process the case of 'Array(NotNullable(nest))'
        reinterpret_cast<ColumnNullable*>(&get_data())
                ->insert_range_from_not_nullable(src.get_data(), offset, size);
    } else {
        get_data().insert_range_from(src.get_data(), offset, size);
    }
    get_offsets().push_back(get_offsets().back() + size);
}

void ColumnArray::insert_default() {
    /// NOTE 1: We can use back() even if the array is empty (due to zero -1th element in PODArray).
    /// NOTE 2: We cannot use reference in push_back, because reference get invalidated if array is reallocated.
    auto last_offset = get_offsets().back();
    get_offsets().push_back(last_offset);
}

void ColumnArray::pop_back(size_t n) {
    auto& offsets_data = get_offsets();
    DCHECK(n <= offsets_data.size()) << " n:" << n << " with offsets size: " << offsets_data.size();
    size_t nested_n = offsets_data.back() - offset_at(offsets_data.size() - n);
    if (nested_n) get_data().pop_back(nested_n);
    offsets_data.resize_assume_reserved(offsets_data.size() - n);
}

void ColumnArray::reserve(size_t n) {
    get_offsets().reserve(n);
    get_data().reserve(
            n); /// The average size of arrays is not taken into account here. Or it is considered to be no more than 1.
}

//please check you real need size in data column, because it's maybe need greater size when data is string column
void ColumnArray::resize(size_t n) {
    auto last_off = get_offsets().back();
    get_offsets().resize_fill(n, last_off);
    // make new size of data column
    get_data().resize(get_offsets().back());
}

size_t ColumnArray::byte_size() const {
    return get_data().byte_size() + get_offsets().size() * sizeof(get_offsets()[0]);
}

size_t ColumnArray::allocated_bytes() const {
    return get_data().allocated_bytes() + get_offsets().allocated_bytes();
}

bool ColumnArray::has_enough_capacity(const IColumn& src) const {
    const auto& src_concrete = assert_cast<const ColumnArray&>(src);
    return get_data().has_enough_capacity(src_concrete.get_data()) &&
           get_offsets_column().has_enough_capacity(src_concrete.get_offsets_column());
}

bool ColumnArray::has_equal_offsets(const ColumnArray& other) const {
    const Offsets64& offsets1 = get_offsets();
    const Offsets64& offsets2 = other.get_offsets();
    return offsets1.size() == offsets2.size() &&
           (offsets1.empty() ||
            0 == memcmp(offsets1.data(), offsets2.data(), sizeof(offsets1[0]) * offsets1.size()));
}

void ColumnArray::insert_range_from(const IColumn& src, size_t start, size_t length) {
    if (length == 0) return;

    const ColumnArray& src_concrete = assert_cast<const ColumnArray&>(src);

    if (start + length > src_concrete.get_offsets().size()) {
        throw doris::Exception(doris::ErrorCode::INTERNAL_ERROR,
                               "Parameter out of bound in ColumnArray::insert_range_from method. "
                               "[start({}) + length({}) > offsets.size({})]",
                               std::to_string(start), std::to_string(length),
                               std::to_string(src_concrete.get_offsets().size()));
    }

    size_t nested_offset = src_concrete.offset_at(start);
    size_t nested_length = src_concrete.get_offsets()[start + length - 1] - nested_offset;

    get_data().insert_range_from(src_concrete.get_data(), nested_offset, nested_length);

    auto& cur_offsets = get_offsets();
    const auto& src_offsets = src_concrete.get_offsets();

    if (start == 0 && cur_offsets.empty()) {
        cur_offsets.assign(src_offsets.begin(), src_offsets.begin() + length);
    } else {
        size_t old_size = cur_offsets.size();
        // -1 is ok, because PaddedPODArray pads zeros on the left.
        size_t prev_max_offset = cur_offsets.back();
        cur_offsets.resize(old_size + length);

        for (size_t i = 0; i < length; ++i)
            cur_offsets[old_size + i] = src_offsets[start + i] - nested_offset + prev_max_offset;
    }
}

void ColumnArray::insert_range_from_ignore_overflow(const IColumn& src, size_t start,
                                                    size_t length) {
    const ColumnArray& src_concrete = assert_cast<const ColumnArray&>(src);

    if (start + length > src_concrete.get_offsets().size()) {
        throw doris::Exception(doris::ErrorCode::INTERNAL_ERROR,
                               "Parameter out of bound in ColumnArray::insert_range_from method. "
                               "[start({}) + length({}) > offsets.size({})]",
                               std::to_string(start), std::to_string(length),
                               std::to_string(src_concrete.get_offsets().size()));
    }

    size_t nested_offset = src_concrete.offset_at(start);
    size_t nested_length = src_concrete.get_offsets()[start + length - 1] - nested_offset;

    get_data().insert_range_from_ignore_overflow(src_concrete.get_data(), nested_offset,
                                                 nested_length);

    auto& cur_offsets = get_offsets();
    const auto& src_offsets = src_concrete.get_offsets();

    if (start == 0 && cur_offsets.empty()) {
        cur_offsets.assign(src_offsets.begin(), src_offsets.begin() + length);
    } else {
        size_t old_size = cur_offsets.size();
        // -1 is ok, because PaddedPODArray pads zeros on the left.
        size_t prev_max_offset = cur_offsets.back();
        cur_offsets.resize(old_size + length);

        for (size_t i = 0; i < length; ++i)
            cur_offsets[old_size + i] = src_offsets[start + i] - nested_offset + prev_max_offset;
    }
}

ColumnPtr ColumnArray::filter(const Filter& filt, ssize_t result_size_hint) const {
    if (typeid_cast<const ColumnUInt8*>(data.get()))
        return filter_number<TYPE_BOOLEAN>(filt, result_size_hint);
    if (typeid_cast<const ColumnInt8*>(data.get()))
        return filter_number<TYPE_TINYINT>(filt, result_size_hint);
    if (typeid_cast<const ColumnInt16*>(data.get()))
        return filter_number<TYPE_SMALLINT>(filt, result_size_hint);
    if (typeid_cast<const ColumnInt32*>(data.get()))
        return filter_number<TYPE_INT>(filt, result_size_hint);
    if (typeid_cast<const ColumnInt64*>(data.get()))
        return filter_number<TYPE_BIGINT>(filt, result_size_hint);
    if (typeid_cast<const ColumnFloat32*>(data.get()))
        return filter_number<TYPE_FLOAT>(filt, result_size_hint);
    if (typeid_cast<const ColumnFloat64*>(data.get()))
        return filter_number<TYPE_DOUBLE>(filt, result_size_hint);
    if (typeid_cast<const ColumnDate*>(data.get()))
        return filter_number<TYPE_DATE>(filt, result_size_hint);
    if (typeid_cast<const ColumnDateV2*>(data.get()))
        return filter_number<TYPE_DATEV2>(filt, result_size_hint);
    if (typeid_cast<const ColumnDateTime*>(data.get()))
        return filter_number<TYPE_DATETIME>(filt, result_size_hint);
    if (typeid_cast<const ColumnDateTimeV2*>(data.get()))
        return filter_number<TYPE_DATETIMEV2>(filt, result_size_hint);
    if (typeid_cast<const ColumnTimeV2*>(data.get()))
        return filter_number<TYPE_TIMEV2>(filt, result_size_hint);
    if (typeid_cast<const ColumnTime*>(data.get()))
        return filter_number<TYPE_TIME>(filt, result_size_hint);
    if (typeid_cast<const ColumnIPv4*>(data.get()))
        return filter_number<TYPE_IPV4>(filt, result_size_hint);
    if (typeid_cast<const ColumnString*>(data.get())) return filter_string(filt, result_size_hint);
    //if (typeid_cast<const ColumnTuple *>(data.get()))      return filterTuple(filt, result_size_hint);
    if (typeid_cast<const ColumnNullable*>(data.get()))
        return filter_nullable(filt, result_size_hint);
    return filter_generic(filt, result_size_hint);
}

size_t ColumnArray::filter(const Filter& filter) {
    if (typeid_cast<const ColumnUInt8*>(data.get())) return filter_number<TYPE_BOOLEAN>(filter);
    if (typeid_cast<const ColumnInt8*>(data.get())) return filter_number<TYPE_TINYINT>(filter);
    if (typeid_cast<const ColumnInt16*>(data.get())) return filter_number<TYPE_SMALLINT>(filter);
    if (typeid_cast<const ColumnInt32*>(data.get())) return filter_number<TYPE_INT>(filter);
    if (typeid_cast<const ColumnInt64*>(data.get())) return filter_number<TYPE_BIGINT>(filter);
    if (typeid_cast<const ColumnFloat32*>(data.get())) return filter_number<TYPE_FLOAT>(filter);
    if (typeid_cast<const ColumnFloat64*>(data.get())) return filter_number<TYPE_DOUBLE>(filter);
    if (typeid_cast<const ColumnDate*>(data.get())) return filter_number<TYPE_DATE>(filter);
    if (typeid_cast<const ColumnDateV2*>(data.get())) return filter_number<TYPE_DATEV2>(filter);
    if (typeid_cast<const ColumnDateTime*>(data.get())) return filter_number<TYPE_DATETIME>(filter);
    if (typeid_cast<const ColumnDateTimeV2*>(data.get()))
        return filter_number<TYPE_DATETIMEV2>(filter);
    if (typeid_cast<const ColumnTimeV2*>(data.get())) return filter_number<TYPE_TIMEV2>(filter);
    if (typeid_cast<const ColumnTime*>(data.get())) return filter_number<TYPE_TIME>(filter);
    if (typeid_cast<const ColumnIPv4*>(data.get())) return filter_number<TYPE_IPV4>(filter);
    return filter_generic(filter);
}

template <PrimitiveType T>
ColumnPtr ColumnArray::filter_number(const Filter& filt, ssize_t result_size_hint) const {
    if (get_offsets().empty()) return ColumnArray::create(data);

    auto res = ColumnArray::create(data->clone_empty());

    auto& res_elems = assert_cast<ColumnVector<T>&>(res->get_data()).get_data();
    auto& res_offsets = res->get_offsets();

    filter_arrays_impl<typename PrimitiveTypeTraits<T>::ColumnItemType, Offset64>(
            assert_cast<const ColumnVector<T>&, TypeCheckOnRelease::DISABLE>(*data).get_data(),
            get_offsets(), res_elems, res_offsets, filt, result_size_hint);
    return res;
}

template <PrimitiveType T>
size_t ColumnArray::filter_number(const Filter& filter) {
    return filter_arrays_impl<typename PrimitiveTypeTraits<T>::ColumnItemType, Offset64>(
            assert_cast<ColumnVector<T>&, TypeCheckOnRelease::DISABLE>(*data).get_data(),
            get_offsets(), filter);
}

ColumnPtr ColumnArray::filter_string(const Filter& filt, ssize_t result_size_hint) const {
    size_t col_size = get_offsets().size();
    column_match_filter_size(col_size, filt.size());

    if (!col_size) {
        return ColumnArray::create(data);
    }

    auto res = ColumnArray::create(data->clone_empty());

    const auto& src_string = assert_cast<const ColumnString&>(*data);
    const ColumnString::Chars& src_chars = src_string.get_chars();
    const auto& src_string_offsets = src_string.get_offsets();
    const auto& src_offsets = get_offsets();

    ColumnString::Chars& res_chars = assert_cast<ColumnString&>(res->get_data()).get_chars();
    auto& res_string_offsets = assert_cast<ColumnString&>(res->get_data()).get_offsets();
    auto& res_offsets = res->get_offsets();

    if (result_size_hint < 0) {
        res_chars.reserve(src_chars.size());
        res_string_offsets.reserve(src_string_offsets.size());
        res_offsets.reserve(col_size);
    }

    Offset64 prev_src_offset = 0;
    IColumn::Offset prev_src_string_offset = 0;

    Offset64 prev_res_offset = 0;
    IColumn::Offset prev_res_string_offset = 0;

    for (size_t i = 0; i < col_size; ++i) {
        /// Number of rows in the array.
        size_t array_size = src_offsets[i] - prev_src_offset;

        if (filt[i]) {
            /// If the array is not empty - copy content.
            if (array_size) {
                size_t chars_to_copy = src_string_offsets[array_size + prev_src_offset - 1] -
                                       prev_src_string_offset;
                size_t res_chars_prev_size = res_chars.size();
                res_chars.resize(res_chars_prev_size + chars_to_copy);
                memcpy(&res_chars[res_chars_prev_size], &src_chars[prev_src_string_offset],
                       chars_to_copy);

                for (size_t j = 0; j < array_size; ++j)
                    res_string_offsets.push_back(src_string_offsets[j + prev_src_offset] +
                                                 prev_res_string_offset - prev_src_string_offset);

                prev_res_string_offset = res_string_offsets.back();
            }

            prev_res_offset += array_size;
            res_offsets.push_back(prev_res_offset);
        }

        if (array_size) {
            prev_src_offset += array_size;
            prev_src_string_offset = src_string_offsets[prev_src_offset - 1];
        }
    }

    return res;
}

size_t ColumnArray::filter_string(const Filter& filter) {
    size_t col_size = get_offsets().size();
    column_match_filter_size(col_size, filter.size());

    if (0 == col_size) {
        return ColumnArray::create(data);
    }

    auto& src_string = assert_cast<ColumnString&>(*data);
    auto* src_chars = src_string.get_chars().data();
    auto* src_string_offsets = src_string.get_offsets().data();
    auto* src_offsets = get_offsets().data();

    ColumnString::Chars& res_chars = src_string.get_chars();
    auto& res_string_offsets = src_string.get_offsets();
    auto& res_offsets = get_offsets();

    res_chars.set_end_ptr(res_chars.data());
    res_string_offsets.set_end_ptr(res_string_offsets.data());
    res_offsets.set_end_ptr(res_offsets.data());

    Offset64 prev_src_offset = 0;
    IColumn::Offset prev_src_string_offset = 0;

    Offset64 prev_res_offset = 0;
    IColumn::Offset prev_res_string_offset = 0;

    for (size_t i = 0; i < col_size; ++i) {
        /// Number of rows in the array.
        size_t array_size = src_offsets[i] - prev_src_offset;

        if (filter[i]) {
            /// If the array is not empty - copy content.
            if (array_size) {
                size_t chars_to_copy = src_string_offsets[array_size + prev_src_offset - 1] -
                                       prev_src_string_offset;
                size_t res_chars_prev_size = res_chars.size();
                res_chars.resize(res_chars_prev_size + chars_to_copy);
                memmove(&res_chars[res_chars_prev_size], &src_chars[prev_src_string_offset],
                        chars_to_copy);

                for (size_t j = 0; j < array_size; ++j) {
                    res_string_offsets.push_back(src_string_offsets[j + prev_src_offset] +
                                                 prev_res_string_offset - prev_src_string_offset);
                }

                prev_res_string_offset = res_string_offsets.back();
            }

            prev_res_offset += array_size;
            res_offsets.push_back(prev_res_offset);
        }

        if (array_size) {
            prev_src_offset += array_size;
            prev_src_string_offset = src_string_offsets[prev_src_offset - 1];
        }
    }

    return res_offsets.size();
}

ColumnPtr ColumnArray::filter_generic(const Filter& filt, ssize_t result_size_hint) const {
    size_t size = get_offsets().size();
    column_match_filter_size(size, filt.size());

    if (size == 0) return ColumnArray::create(data);

    Filter nested_filt(get_offsets().back());
    ssize_t nested_result_size_hint = 0;
    for (size_t i = 0; i < size; ++i) {
        if (filt[i]) {
            memset(&nested_filt[offset_at(i)], 1, size_at(i));
            nested_result_size_hint += size_at(i);
        } else {
            memset(&nested_filt[offset_at(i)], 0, size_at(i));
        }
    }

    auto res = ColumnArray::create(data->clone_empty());
    res->data = data->filter(nested_filt, nested_result_size_hint);

    auto& res_offsets = res->get_offsets();
    if (result_size_hint) res_offsets.reserve(result_size_hint > 0 ? result_size_hint : size);

    size_t current_offset = 0;
    for (size_t i = 0; i < size; ++i) {
        if (filt[i]) {
            current_offset += size_at(i);
            res_offsets.push_back(current_offset);
        }
    }

    return res;
}

size_t ColumnArray::filter_generic(const Filter& filter) {
    size_t size = get_offsets().size();
    column_match_filter_size(size, filter.size());

    if (size == 0) {
        return 0;
    }

    Filter nested_filter(get_offsets().back());
    for (size_t i = 0; i < size; ++i) {
        if (filter[i]) {
            memset(&nested_filter[offset_at(i)], 1, size_at(i));
        } else {
            memset(&nested_filter[offset_at(i)], 0, size_at(i));
        }
    }

    data->filter(nested_filter);
    // Make a new offset to avoid inplace operation
    auto res_offset = ColumnOffsets::create();
    auto& res_offset_data = res_offset->get_data();
    res_offset_data.reserve(size);
    size_t current_offset = 0;
    for (size_t i = 0; i < size; ++i) {
        if (filter[i]) {
            current_offset += size_at(i);
            res_offset_data.push_back(current_offset);
        }
    }
    get_offsets().swap(res_offset_data);
    return get_offsets().size();
}

ColumnPtr ColumnArray::filter_nullable(const Filter& filt, ssize_t result_size_hint) const {
    if (get_offsets().empty()) return ColumnArray::create(data);

    const ColumnNullable& nullable_elems =
            assert_cast<const ColumnNullable&, TypeCheckOnRelease::DISABLE>(*data);

    auto array_of_nested = ColumnArray::create(nullable_elems.get_nested_column_ptr(), offsets);
    auto filtered_array_of_nested_owner = array_of_nested->filter(filt, result_size_hint);
    const auto& filtered_array_of_nested =
            assert_cast<const ColumnArray&>(*filtered_array_of_nested_owner);
    const auto& filtered_offsets = filtered_array_of_nested.get_offsets_ptr();

    auto res_null_map = ColumnUInt8::create();

    filter_arrays_impl_only_data(nullable_elems.get_null_map_data(), get_offsets(),
                                 res_null_map->get_data(), filt, result_size_hint);

    return ColumnArray::create(ColumnNullable::create(filtered_array_of_nested.get_data_ptr(),
                                                      std::move(res_null_map)),
                               filtered_offsets);
}

size_t ColumnArray::filter_nullable(const Filter& filter) {
    if (get_offsets().empty()) {
        return 0;
    }

    ColumnNullable& nullable_elems =
            assert_cast<ColumnNullable&, TypeCheckOnRelease::DISABLE>(*data);
    const auto result_size =
            filter_arrays_impl_only_data(nullable_elems.get_null_map_data(), get_offsets(), filter);

    auto array_of_nested = ColumnArray::create(nullable_elems.get_nested_column_ptr(), offsets);
    const auto nested_result_size = array_of_nested->assume_mutable()->filter(filter);

    CHECK_EQ(result_size, nested_result_size);
    return result_size;
}

void ColumnArray::insert_indices_from(const IColumn& src, const uint32_t* indices_begin,
                                      const uint32_t* indices_end) {
    for (const auto* x = indices_begin; x != indices_end; ++x) {
        ColumnArray::insert_from(src, *x);
    }
}

void ColumnArray::insert_many_from(const IColumn& src, size_t position, size_t length) {
    for (auto x = 0; x != length; ++x) {
        ColumnArray::insert_from(src, position);
    }
}

ColumnPtr ColumnArray::replicate(const IColumn::Offsets& replicate_offsets) const {
    if (replicate_offsets.empty()) {
        return clone_empty();
    }

    // keep ColumnUInt8 for ColumnNullable::null_map
    if (typeid_cast<const ColumnUInt8*>(data.get())) {
        return replicate_number<TYPE_BOOLEAN>(replicate_offsets);
    }
    if (typeid_cast<const ColumnInt8*>(data.get())) {
        return replicate_number<TYPE_TINYINT>(replicate_offsets);
    }
    if (typeid_cast<const ColumnInt16*>(data.get())) {
        return replicate_number<TYPE_SMALLINT>(replicate_offsets);
    }
    if (typeid_cast<const ColumnInt32*>(data.get())) {
        return replicate_number<TYPE_INT>(replicate_offsets);
    }
    if (typeid_cast<const ColumnInt64*>(data.get())) {
        return replicate_number<TYPE_BIGINT>(replicate_offsets);
    }
    if (typeid_cast<const ColumnInt128*>(data.get())) {
        return replicate_number<TYPE_LARGEINT>(replicate_offsets);
    }
    if (typeid_cast<const ColumnIPv4*>(data.get())) {
        return replicate_number<TYPE_IPV4>(replicate_offsets);
    }
    if (typeid_cast<const ColumnIPv6*>(data.get())) {
        return replicate_number<TYPE_IPV6>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDate*>(data.get())) {
        return replicate_number<TYPE_DATE>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDateTime*>(data.get())) {
        return replicate_number<TYPE_DATETIME>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDateV2*>(data.get())) {
        return replicate_number<TYPE_DATEV2>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDateTimeV2*>(data.get())) {
        return replicate_number<TYPE_DATETIMEV2>(replicate_offsets);
    }
    if (typeid_cast<const ColumnFloat32*>(data.get())) {
        return replicate_number<TYPE_FLOAT>(replicate_offsets);
    }
    if (typeid_cast<const ColumnFloat64*>(data.get())) {
        return replicate_number<TYPE_DOUBLE>(replicate_offsets);
    }
    if (typeid_cast<const ColumnTime*>(data.get())) {
        return replicate_number<TYPE_TIME>(replicate_offsets);
    }
    if (typeid_cast<const ColumnTimeV2*>(data.get())) {
        return replicate_number<TYPE_TIMEV2>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDecimal32*>(data.get())) {
        return replicate_number<TYPE_DECIMAL32>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDecimal64*>(data.get())) {
        return replicate_number<TYPE_DECIMAL64>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDecimal128V2*>(data.get())) {
        return replicate_number<TYPE_DECIMALV2>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDecimal128V3*>(data.get())) {
        return replicate_number<TYPE_DECIMAL128I>(replicate_offsets);
    }
    if (typeid_cast<const ColumnDecimal256*>(data.get())) {
        return replicate_number<TYPE_DECIMAL256>(replicate_offsets);
    }
    if (typeid_cast<const ColumnString*>(data.get())) {
        return replicate_string(replicate_offsets);
    }
    if (typeid_cast<const ColumnNullable*>(data.get())) {
        return replicate_nullable(replicate_offsets);
    }
    return replicate_generic(replicate_offsets);
}

template <PrimitiveType T>
ColumnPtr ColumnArray::replicate_number(const IColumn::Offsets& replicate_offsets) const {
    size_t col_size = size();
    column_match_offsets_size(col_size, replicate_offsets.size());

    MutableColumnPtr res = clone_empty();

    if (!col_size) {
        return res;
    }

    auto& res_arr = assert_cast<ColumnArray&>(*res);

    const typename PrimitiveTypeTraits<T>::ColumnType::Container& src_data =
            assert_cast<const typename PrimitiveTypeTraits<T>::ColumnType&>(*data).get_data();
    const auto& src_offsets = get_offsets();

    typename PrimitiveTypeTraits<T>::ColumnType::Container& res_data =
            assert_cast<typename PrimitiveTypeTraits<T>::ColumnType&>(res_arr.get_data())
                    .get_data();
    auto& res_offsets = res_arr.get_offsets();

    res_data.reserve(data->size() / col_size * replicate_offsets.back());
    res_offsets.reserve(replicate_offsets.back());

    IColumn::Offset prev_replicate_offset = 0;
    Offset64 prev_data_offset = 0;
    Offset64 current_new_offset = 0;

    for (size_t i = 0; i < col_size; ++i) {
        size_t size_to_replicate = replicate_offsets[i] - prev_replicate_offset;
        size_t value_size = src_offsets[i] - prev_data_offset;

        for (size_t j = 0; j < size_to_replicate; ++j) {
            current_new_offset += value_size;
            res_offsets.push_back(current_new_offset);

            if (value_size) {
                res_data.resize(res_data.size() + value_size);
                memcpy(&res_data[res_data.size() - value_size], &src_data[prev_data_offset],
                       value_size * sizeof(typename PrimitiveTypeTraits<T>::ColumnItemType));
            }
        }

        prev_replicate_offset = replicate_offsets[i];
        prev_data_offset = src_offsets[i];
    }

    return res;
}

ColumnPtr ColumnArray::replicate_string(const IColumn::Offsets& replicate_offsets) const {
    size_t col_size = size();
    column_match_offsets_size(col_size, replicate_offsets.size());

    MutableColumnPtr res = clone_empty();

    if (!col_size) {
        return res;
    }

    auto& res_arr = assert_cast<ColumnArray&, TypeCheckOnRelease::DISABLE>(*res);

    const auto& src_string = assert_cast<const ColumnString&>(*data);
    const ColumnString::Chars& src_chars = src_string.get_chars();
    const auto& src_string_offsets = src_string.get_offsets();
    const auto& src_offsets = get_offsets();

    ColumnString::Chars& res_chars = assert_cast<ColumnString&>(res_arr.get_data()).get_chars();
    auto& res_string_offsets = assert_cast<ColumnString&>(res_arr.get_data()).get_offsets();
    auto& res_offsets = res_arr.get_offsets();

    res_chars.reserve(src_chars.size() / col_size * replicate_offsets.back());
    res_string_offsets.reserve(src_string_offsets.size() / col_size * replicate_offsets.back());
    res_offsets.reserve(replicate_offsets.back());

    IColumn::Offset prev_replicate_offset = 0;

    Offset64 prev_src_offset = 0;
    IColumn::Offset prev_src_string_offset = 0;

    Offset64 current_res_offset = 0;
    IColumn::Offset current_res_string_offset = 0;

    for (size_t i = 0; i < col_size; ++i) {
        /// How many times to replicate the array.
        size_t size_to_replicate = replicate_offsets[i] - prev_replicate_offset;
        /// The number of strings in the array.
        size_t value_size = src_offsets[i] - prev_src_offset;
        /// Number of characters in strings of the array, including zero bytes.
        size_t sum_chars_size = src_string_offsets[prev_src_offset + value_size - 1] -
                                prev_src_string_offset; /// -1th index is Ok, see PaddedPODArray.

        for (size_t j = 0; j < size_to_replicate; ++j) {
            current_res_offset += value_size;
            res_offsets.push_back(current_res_offset);

            size_t prev_src_string_offset_local = prev_src_string_offset;
            for (size_t k = 0; k < value_size; ++k) {
                /// Size of single string.
                size_t chars_size =
                        src_string_offsets[k + prev_src_offset] - prev_src_string_offset_local;

                current_res_string_offset += chars_size;
                res_string_offsets.push_back(current_res_string_offset);

                prev_src_string_offset_local += chars_size;
            }

            if (sum_chars_size) {
                /// Copies the characters of the array of strings.
                res_chars.resize(res_chars.size() + sum_chars_size);
                memcpy_small_allow_read_write_overflow15(
                        &res_chars[res_chars.size() - sum_chars_size],
                        &src_chars[prev_src_string_offset], sum_chars_size);
            }
        }

        prev_replicate_offset = replicate_offsets[i];
        prev_src_offset = src_offsets[i];
        prev_src_string_offset += sum_chars_size;
    }

    return res;
}

ColumnPtr ColumnArray::replicate_generic(const IColumn::Offsets& replicate_offsets) const {
    size_t col_size = size();
    column_match_offsets_size(col_size, replicate_offsets.size());

    MutableColumnPtr res = clone_empty();
    ColumnArray& res_concrete = assert_cast<ColumnArray&, TypeCheckOnRelease::DISABLE>(*res);

    if (0 == col_size) return res;

    Offset64 prev_offset = 0;
    for (size_t i = 0; i < col_size; ++i) {
        size_t size_to_replicate = replicate_offsets[i] - prev_offset;
        prev_offset = replicate_offsets[i];

        for (size_t j = 0; j < size_to_replicate; ++j) {
            res_concrete.insert_from(*this, i);
        }
    }

    return res;
}

ColumnPtr ColumnArray::replicate_nullable(const IColumn::Offsets& replicate_offsets) const {
    const ColumnNullable& nullable =
            assert_cast<const ColumnNullable&, TypeCheckOnRelease::DISABLE>(*data);

    /// Make temporary arrays for each components of Nullable. Then replicate them independently and collect back to result.
    /// NOTE Offsets are calculated twice and it is redundant.

    auto array_of_nested = ColumnArray(nullable.get_nested_column_ptr()->assume_mutable(),
                                       get_offsets_ptr()->assume_mutable())
                                   .replicate(replicate_offsets);
    auto array_of_null_map = ColumnArray(nullable.get_null_map_column_ptr()->assume_mutable(),
                                         get_offsets_ptr()->assume_mutable())
                                     .replicate(replicate_offsets);

    return ColumnArray::create(
            ColumnNullable::create(
                    assert_cast<const ColumnArray&, TypeCheckOnRelease::DISABLE>(*array_of_nested)
                            .get_data_ptr(),
                    assert_cast<const ColumnArray&, TypeCheckOnRelease::DISABLE>(*array_of_null_map)
                            .get_data_ptr()),
            assert_cast<const ColumnArray&, TypeCheckOnRelease::DISABLE>(*array_of_nested)
                    .get_offsets_ptr());
}

MutableColumnPtr ColumnArray::permute(const Permutation& perm, size_t limit) const {
    size_t size = offsets->size();
    if (limit == 0) {
        limit = size;
    } else {
        limit = std::min(size, limit);
    }
    if (perm.size() < limit) {
        throw doris::Exception(ErrorCode::INTERNAL_ERROR,
                               "Size of permutation is less than required.");
        __builtin_unreachable();
    }
    if (limit == 0) {
        return ColumnArray::create(data);
    }

    auto res = ColumnArray::create(data->clone_empty());
    auto& res_offsets = res->get_offsets();
    res_offsets.resize(limit);

    Permutation nested_perm;
    nested_perm.reserve(data->size());

    for (size_t i = 0; i < limit; ++i) {
        res_offsets[i] = res_offsets[i - 1] + size_at(perm[i]);
        for (size_t j = 0; j < size_at(perm[i]); ++j) {
            nested_perm.push_back(offset_at(perm[i]) + j);
        }
    }
    if (nested_perm.size() != 0) {
        res->data = data->permute(nested_perm, nested_perm.size());
    }
    return res;
}

void ColumnArray::erase(size_t start, size_t length) {
    if (start >= size() || length == 0) {
        return;
    }
    length = std::min(length, size() - start);

    const auto& offsets_data = get_offsets();
    auto data_start = offsets_data[start - 1];
    auto data_end = offsets_data[start + length - 1];
    auto data_length = data_end - data_start;
    data->erase(data_start, data_length);
    offsets->erase(start, length);

    for (auto i = start; i < size(); ++i) {
        get_offsets()[i] -= data_length;
    }
}

} // namespace doris::vectorized
