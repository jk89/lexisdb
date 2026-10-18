/*
 * Order-preserving encoding for partition key → token.
 *
 * Produces bytes where memcmp gives correct type-aware ordering,
 * enabling range scans over partition key prefixes.
 *
 * Float lex transforms ported from kaebase v2:
 * https://github.com/kaepek/kaebase/blob/FEAT/scaling/v2/crates/common/src/encoding/float.rs
 *
 * Composite key encoding (null-byte escaping, terminators) ported from:
 * https://github.com/kaepek/kaebase/blob/FEAT/scaling/v2/crates/common/src/encoding/cluster_key.rs
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "dht/ordered_encoding.hh"
#include "utils/big_decimal.hh"
#include <cstring>
#include <stdexcept>

namespace dht {
namespace ordered_encoding {

// ============================================================================
// float.rs — float64_to_lex_u64
// ============================================================================

uint64_t float64_to_lex_u64(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));

    uint64_t sign_bit = (bits >> 63) & 1;
    uint64_t exponent = (bits >> 52) & 0x7FF;
    uint64_t mantissa = bits & 0x000FFFFFFFFFFFFF;

    if (sign_bit == 0) {
        return (uint64_t(1) << 63) | (exponent << 52) | mantissa;
    } else {
        uint64_t new_exponent = 2047 - exponent;
        uint64_t new_mantissa = ((uint64_t(1) << 52) - 1) - mantissa;
        return (new_exponent << 52) | new_mantissa;
    }
}

// ============================================================================
// float.rs — lex_u64_to_float64
// ============================================================================

double lex_u64_to_float64(uint64_t lex) {
    uint64_t top_bit = (lex >> 63) & 1;
    uint64_t bits;

    if (top_bit == 1) {
        bits = lex & ~(uint64_t(1) << 63);
    } else {
        uint64_t exponent = (lex >> 52) & 0x7FF;
        uint64_t mantissa = lex & 0x000FFFFFFFFFFFFF;
        uint64_t orig_exponent = 2047 - exponent;
        uint64_t orig_mantissa = ((uint64_t(1) << 52) - 1) - mantissa;
        bits = (uint64_t(1) << 63) | (orig_exponent << 52) | orig_mantissa;
    }

    double result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// ============================================================================
// float.rs — float32_to_lex_u32
// ============================================================================

uint32_t float32_to_lex_u32(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));

    uint32_t sign_bit = (bits >> 31) & 1;
    uint32_t exponent = (bits >> 23) & 0xFF;
    uint32_t mantissa = bits & 0x007FFFFF;

    if (sign_bit == 0) {
        return (uint32_t(1) << 31) | (exponent << 23) | mantissa;
    } else {
        uint32_t new_exponent = 255 - exponent;
        uint32_t new_mantissa = ((uint32_t(1) << 23) - 1) - mantissa;
        return (new_exponent << 23) | new_mantissa;
    }
}

// ============================================================================
// Helpers: escape a byte into buf, and append terminator.
// ============================================================================

static inline void escape_byte(std::vector<uint8_t>& buf, uint8_t byte) {
    if (byte == 0x00) {
        buf.push_back(0x00);
        buf.push_back(0x01);
    } else {
        buf.push_back(byte);
    }
}

static inline void terminate(std::vector<uint8_t>& buf) {
    buf.push_back(0x00);
    buf.push_back(0x00);
}

static inline void emit_be64(std::vector<uint8_t>& buf, uint64_t val) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        escape_byte(buf, static_cast<uint8_t>(val >> shift));
    }
}

static inline void emit_be32(std::vector<uint8_t>& buf, uint32_t val) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        escape_byte(buf, static_cast<uint8_t>(val >> shift));
    }
}

static inline void emit_be16(std::vector<uint8_t>& buf, uint16_t val) {
    escape_byte(buf, static_cast<uint8_t>(val >> 8));
    escape_byte(buf, static_cast<uint8_t>(val));
}

// ============================================================================
// varint: sign byte, magnitude length, magnitude.
//
//   v >= 0:  0x80, BE32(len), magnitude
//   v <  0:  0x7f, ~BE32(len), ~magnitude
//
// The magnitude is minimal (no leading zero bytes; 0 has length 0), so among
// non-negatives a longer magnitude is larger, and equal lengths compare
// big-endian. Complementing reverses both for negatives.
// ============================================================================

static void encode_varint(std::vector<uint8_t>& buf, const bytes& twos_complement) {
    std::vector<uint8_t> mag(twos_complement.begin(), twos_complement.end());
    bool negative = !mag.empty() && (mag[0] & 0x80);
    if (negative) {
        // |v| = ~v + 1
        for (auto& b : mag) {
            b = ~b;
        }
        for (size_t i = mag.size(); i-- > 0;) {
            if (++mag[i] != 0) {
                break;
            }
        }
    }
    size_t start = 0;
    while (start < mag.size() && mag[start] == 0) {
        ++start;
    }
    uint32_t len = mag.size() - start;
    uint8_t flip = negative ? 0xff : 0x00;
    escape_byte(buf, negative ? 0x7f : 0x80);
    emit_be32(buf, len ^ (negative ? 0xffffffffu : 0u));
    for (size_t i = start; i < mag.size(); ++i) {
        escape_byte(buf, mag[i] ^ flip);
    }
}

// ============================================================================
// decimal: normalized to sign, decimal exponent and digits, so that equal
// values (1.0, 1.00) encode identically.
//
//   value = 0.d1 d2 ... dn * 10^e, d1 != 0, dn != 0
//   zero:   0x80
//   v > 0:  0x81, BE64(e + 2^63), digits as ASCII
//   v < 0:  0x7f, ~BE64(e + 2^63), ~digits, 0xff
//
// For positives a longer digit string with the same prefix is larger, which
// prefix order already gives. For negatives it must be smaller; the 0xff end
// marker sorts above every complemented digit, so it does.
// ============================================================================

static void encode_decimal(std::vector<uint8_t>& buf, const big_decimal& d) {
    const auto& unscaled = d.unscaled_value();
    if (unscaled == 0) {
        escape_byte(buf, 0x80);
        return;
    }
    bool negative = unscaled < 0;
    std::string digits = (negative ? boost::multiprecision::cpp_int(-unscaled) : unscaled).str();
    int64_t scale = d.scale();
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
        --scale;
    }
    int64_t exponent = int64_t(digits.size()) - scale;
    uint64_t biased = uint64_t(exponent) ^ (uint64_t(1) << 63);
    uint8_t flip = negative ? 0xff : 0x00;
    escape_byte(buf, negative ? 0x7f : 0x81);
    emit_be64(buf, biased ^ (negative ? ~uint64_t(0) : uint64_t(0)));
    for (char c : digits) {
        escape_byte(buf, uint8_t(c) ^ flip);
    }
    if (negative) {
        escape_byte(buf, 0xff);
    }
}

// ============================================================================
// Per-type encoding into output buffer.
// Partition key columns are always ASC.
// ============================================================================

static void encode_type_into(std::vector<uint8_t>& buf, const data_value& value, const data_type& type) {
    auto kind = type->get_kind();

    switch (kind) {
    case abstract_type::kind::utf8:
    case abstract_type::kind::ascii:
    case abstract_type::kind::duration:
    case abstract_type::kind::bytes:
    case abstract_type::kind::uuid:
    case abstract_type::kind::timeuuid:
    case abstract_type::kind::inet: {
        bytes raw = value.serialize_nonnull();
        for (size_t i = 0; i < raw.size(); ++i) {
            escape_byte(buf, static_cast<uint8_t>(raw[i]));
        }
        break;
    }

    case abstract_type::kind::long_kind:
    case abstract_type::kind::counter: {
        int64_t v = value_cast<int64_t>(value);
        uint64_t unsigned_val = static_cast<uint64_t>(v) + (uint64_t(1) << 63);
        emit_be64(buf, unsigned_val);
        break;
    }

    case abstract_type::kind::int32: {
        int32_t v = value_cast<int32_t>(value);
        uint32_t unsigned_val = static_cast<uint32_t>(v) + (uint32_t(1) << 31);
        emit_be32(buf, unsigned_val);
        break;
    }

    case abstract_type::kind::short_kind: {
        int16_t v = value_cast<int16_t>(value);
        uint16_t unsigned_val = static_cast<uint16_t>(v) + (uint16_t(1) << 15);
        emit_be16(buf, unsigned_val);
        break;
    }

    case abstract_type::kind::byte: {
        int8_t v = value_cast<int8_t>(value);
        uint8_t unsigned_val = static_cast<uint8_t>(v) ^ 0x80;
        escape_byte(buf, unsigned_val);
        break;
    }

    case abstract_type::kind::boolean: {
        bool b = value_cast<bool>(value);
        escape_byte(buf, b ? uint8_t(1) : uint8_t(0));
        break;
    }

    case abstract_type::kind::timestamp:
    case abstract_type::kind::date:
    case abstract_type::kind::simple_date:
    case abstract_type::kind::time: {
        bytes raw = value.serialize_nonnull();
        int64_t ms = 0;
        for (size_t i = 0; i < raw.size() && i < 8; ++i) {
            ms = (ms << 8) | static_cast<uint8_t>(raw[i]);
        }
        uint64_t unsigned_val = static_cast<uint64_t>(ms) + (uint64_t(1) << 63);
        emit_be64(buf, unsigned_val);
        break;
    }

    case abstract_type::kind::double_kind: {
        double v = value_cast<double>(value);
        emit_be64(buf, float64_to_lex_u64(v));
        break;
    }

    case abstract_type::kind::float_kind: {
        float v = value_cast<float>(value);
        emit_be64(buf, float64_to_lex_u64(static_cast<double>(v)));
        break;
    }

    case abstract_type::kind::varint: {
        encode_varint(buf, value.serialize_nonnull());
        break;
    }

    case abstract_type::kind::decimal: {
        encode_decimal(buf, value_cast<big_decimal>(value));
        break;
    }

    default: {
        // No order-preserving rule for this type (tuples, UDTs, frozen
        // collections, ...): fall back to the serialized bytes. This is still
        // a total order, consistent across nodes, just not the type's own order.
        bytes raw = value.serialize_nonnull();
        for (size_t i = 0; i < raw.size(); ++i) {
            escape_byte(buf, static_cast<uint8_t>(raw[i]));
        }
        break;
    }
    }

    terminate(buf);
}

// ============================================================================
// encode_partition_key — encode partition key columns into ordered bytes
// ============================================================================

std::vector<uint8_t> encode_partition_key(
    const std::vector<data_value>& values,
    const std::vector<data_type>& types)
{
    if (values.size() != types.size()) {
        throw std::runtime_error("values and types length mismatch");
    }

    std::vector<uint8_t> buf;

    for (size_t i = 0; i < values.size(); ++i) {
        encode_type_into(buf, values[i], types[i]);
    }

    return buf;
}

bytes encode_serialized_partition_key(
    const std::vector<bytes>& components,
    const std::vector<data_type>& types)
{
    std::vector<uint8_t> buf;

    for (size_t i = 0; i < components.size(); ++i) {
        // A key with more components than the schema has partition key columns
        // (only built internally, never from CQL) encodes the extras as raw bytes.
        if (i >= types.size()) {
            for (auto b : components[i]) {
                escape_byte(buf, static_cast<uint8_t>(b));
            }
            terminate(buf);
            continue;
        }
        if (components[i].empty()) {
            terminate(buf);
            continue;
        }
        // The token of stored data must always be computable. A value that does
        // not deserialize as its type (CQL validates input, but keys built
        // internally or read from old data may not) falls back to its raw
        // bytes: still deterministic, just not in the type's order.
        auto mark = buf.size();
        try {
            encode_type_into(buf, types[i]->deserialize_value(components[i]), types[i]);
        } catch (const marshal_exception&) {
            buf.resize(mark);
            for (auto b : components[i]) {
                escape_byte(buf, static_cast<uint8_t>(b));
            }
            terminate(buf);
        }
    }

    return bytes(reinterpret_cast<const int8_t*>(buf.data()), buf.size());
}

} // namespace ordered_encoding
} // namespace dht
