/*
 * Order-preserving encoding tests for partition key → token.
 *
 * Float lex transform tests ported from kaebase v2:
 *   crates/common/src/encoding/float.rs (7 tests)
 * Partition key encoding tests ported from:
 *   crates/common/src/encoding/cluster_key.rs (ASC and DESC ordering,
 *   composite ASC/DESC/ASC keys)
 *   crates/common/src/encoding/tests.rs (1 test)
 *
 * Reference:
 * https://github.com/kaepek/kaebase/blob/FEAT/scaling/v2/crates/common/src/encoding/float.rs
 * https://github.com/kaepek/kaebase/blob/FEAT/scaling/v2/crates/common/src/encoding/cluster_key.rs
 * https://github.com/kaepek/kaebase/blob/FEAT/scaling/v2/crates/common/src/encoding/tests.rs
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define BOOST_TEST_MODULE ordered_encoding

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <limits>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "types/types.hh"
#include "utils/UUID.hh"
#include "dht/ordered_encoding.hh"
#include "utils/big_decimal.hh"
#include "utils/multiprecision_int.hh"

using namespace dht::ordered_encoding;

// ============================================================================
// float.rs — test_positive_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_positive_ordering) {
    double values[] = {0.0, 0.1, 1.0, 100.0, std::numeric_limits<double>::max()};
    size_t n = sizeof(values) / sizeof(values[0]);
    std::vector<uint64_t> encoded;
    for (size_t i = 0; i < n; ++i) {
        encoded.push_back(float64_to_lex_u64(values[i]));
    }
    for (size_t i = 1; i < n; ++i) {
        BOOST_REQUIRE_MESSAGE(encoded[i - 1] < encoded[i],
            std::to_string(values[i - 1]) + " should encode before " + std::to_string(values[i]));
    }
}

// ============================================================================
// float.rs — test_negative_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_negative_ordering) {
    double values[] = {
        std::numeric_limits<double>::lowest(),
        -100.0, -1.0, -0.1,
        -std::numeric_limits<double>::min()
    };
    size_t n = sizeof(values) / sizeof(values[0]);
    std::vector<uint64_t> encoded;
    for (size_t i = 0; i < n; ++i) {
        encoded.push_back(float64_to_lex_u64(values[i]));
    }
    for (size_t i = 1; i < n; ++i) {
        BOOST_REQUIRE_MESSAGE(encoded[i - 1] < encoded[i],
            std::to_string(values[i - 1]) + " should encode before " + std::to_string(values[i]));
    }
}

// ============================================================================
// float.rs — test_cross_sign_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_cross_sign_ordering) {
    double values[] = {
        -std::numeric_limits<double>::infinity(),
        -1.0,
        -std::numeric_limits<double>::min(),
        0.0,
        std::numeric_limits<double>::min(),
        1.0,
        std::numeric_limits<double>::infinity()
    };
    size_t n = sizeof(values) / sizeof(values[0]);
    std::vector<uint64_t> encoded;
    for (size_t i = 0; i < n; ++i) {
        encoded.push_back(float64_to_lex_u64(values[i]));
    }
    for (size_t i = 1; i < n; ++i) {
        BOOST_REQUIRE_MESSAGE(encoded[i - 1] < encoded[i],
            std::to_string(values[i - 1]) + " should encode before " + std::to_string(values[i]));
    }
}

// ============================================================================
// float.rs — test_roundtrip
// ============================================================================

BOOST_AUTO_TEST_CASE(test_roundtrip) {
    double values[] = {
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::lowest(),
        -1.0,
        -std::numeric_limits<double>::min(),
        0.0,
        std::numeric_limits<double>::min(),
        1.0,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
    };
    for (double v : values) {
        uint64_t encoded = float64_to_lex_u64(v);
        double decoded = lex_u64_to_float64(encoded);
        uint64_t v_bits, decoded_bits;
        std::memcpy(&v_bits, &v, sizeof(v_bits));
        std::memcpy(&decoded_bits, &decoded, sizeof(decoded_bits));
        BOOST_REQUIRE_EQUAL(v_bits, decoded_bits);
    }
}

// ============================================================================
// float.rs — test_negative_zero
// ============================================================================

BOOST_AUTO_TEST_CASE(test_negative_zero) {
    uint64_t neg_zero = float64_to_lex_u64(-0.0);
    uint64_t pos_zero = float64_to_lex_u64(0.0);
    BOOST_REQUIRE(neg_zero < pos_zero);
}

// ============================================================================
// float.rs — test_large_sample_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_large_sample_ordering) {
    std::vector<double> samples;
    samples.push_back(-std::numeric_limits<double>::infinity());
    samples.push_back(std::numeric_limits<double>::lowest());
    samples.push_back(-std::numeric_limits<double>::min());
    samples.push_back(0.0);
    samples.push_back(std::numeric_limits<double>::min());
    samples.push_back(std::numeric_limits<double>::max());
    samples.push_back(std::numeric_limits<double>::infinity());

    int n = 10000;
    for (int i = 0; i < n; ++i) {
        double t = (static_cast<double>(i) + 1.0) / (static_cast<double>(n) + 1.0);
        double log_min = std::log10(std::numeric_limits<double>::min());
        double log_max = std::log10(std::numeric_limits<double>::max());
        double pos = std::pow(10.0, log_min + (log_max - log_min) * t);
        samples.push_back(pos);
        samples.push_back(-pos);
    }

    std::sort(samples.begin(), samples.end());

    std::vector<uint64_t> encoded;
    for (double v : samples) {
        encoded.push_back(float64_to_lex_u64(v));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        if (samples[i - 1] == samples[i]) {
            continue;
        }
        BOOST_REQUIRE_MESSAGE(encoded[i - 1] < encoded[i],
            "Order broken at index " + std::to_string(i));
    }
}

// ============================================================================
// float.rs — test_float32_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_float32_ordering) {
    float values[] = {
        -std::numeric_limits<float>::infinity(),
        -1.0f, 0.0f, 1.0f,
        std::numeric_limits<float>::infinity()
    };
    size_t n = sizeof(values) / sizeof(values[0]);
    std::vector<uint32_t> encoded;
    for (size_t i = 0; i < n; ++i) {
        encoded.push_back(float32_to_lex_u32(values[i]));
    }
    for (size_t i = 1; i < n; ++i) {
        BOOST_REQUIRE(encoded[i - 1] < encoded[i]);
    }
}

// ============================================================================
// Partition key encoding — test_asc_terminator
// ============================================================================

BOOST_AUTO_TEST_CASE(test_asc_terminator) {
    auto buf = encode_partition_key({data_value("ab")}, {utf8_type});
    std::vector<uint8_t> expected = {0x61, 0x62, 0x00, 0x00};
    BOOST_REQUIRE(buf == expected);
}

// ============================================================================
// Partition key encoding — test_escape_zero_byte
// ============================================================================

BOOST_AUTO_TEST_CASE(test_escape_zero_byte) {
    auto val = data_value(bytes({0x41, 0x00, 0x42}));
    auto buf = encode_partition_key({val}, {bytes_type});
    std::vector<uint8_t> expected = {0x41, 0x00, 0x01, 0x42, 0x00, 0x00};
    BOOST_REQUIRE(buf == expected);
}

// ============================================================================
// Partition key encoding — test_text_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_text_ordering) {
    std::vector<std::string> vals = {"aaa", "abc", "abd", "xyz", "zzz"};
    std::vector<std::vector<uint8_t>> encoded;
    for (auto& s : vals) {
        encoded.push_back(encode_partition_key({data_value(s)}, {utf8_type}));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        BOOST_REQUIRE_MESSAGE(encoded[i - 1] < encoded[i],
            "ordering broken: " + vals[i - 1] + " >= " + vals[i]);
    }
}

// ============================================================================
// Partition key encoding — test_text_variable_length
// ============================================================================

BOOST_AUTO_TEST_CASE(test_text_variable_length) {
    auto short_key = encode_partition_key({data_value("ab")}, {utf8_type});
    auto long_key = encode_partition_key({data_value("abc")}, {utf8_type});
    BOOST_REQUIRE(short_key < long_key);
}

// ============================================================================
// Partition key encoding — test_int_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_int_ordering) {
    std::vector<int32_t> vals = {-100, -1, 0, 1, 100, std::numeric_limits<int32_t>::max()};
    std::vector<std::vector<uint8_t>> encoded;
    for (auto v : vals) {
        encoded.push_back(encode_partition_key({data_value(v)}, {int32_type}));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        BOOST_REQUIRE(encoded[i - 1] < encoded[i]);
    }
}

// ============================================================================
// Partition key encoding — test_double_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_double_ordering) {
    std::vector<double> vals = {-100.0, -1.0, 0.0, 1.0, 100.0, std::numeric_limits<double>::max()};
    std::vector<std::vector<uint8_t>> encoded;
    for (auto v : vals) {
        encoded.push_back(encode_partition_key({data_value(v)}, {double_type}));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        BOOST_REQUIRE(encoded[i - 1] < encoded[i]);
    }
}

// ============================================================================
// Partition key encoding — test_boolean_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_boolean_ordering) {
    auto f = encode_partition_key({data_value(false)}, {boolean_type});
    auto t = encode_partition_key({data_value(true)}, {boolean_type});
    BOOST_REQUIRE(f < t);
}

// ============================================================================
// Partition key encoding — test_bigint_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_bigint_ordering) {
    std::vector<int64_t> vals = {
        std::numeric_limits<int64_t>::min(), int64_t(-1), int64_t(0), int64_t(1),
        std::numeric_limits<int64_t>::max()
    };
    std::vector<std::vector<uint8_t>> encoded;
    for (auto v : vals) {
        encoded.push_back(encode_partition_key({data_value(v)}, {long_type}));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        BOOST_REQUIRE(encoded[i - 1] < encoded[i]);
    }
}

// ============================================================================
// Partition key encoding — test_uuid_ordering
// ============================================================================

BOOST_AUTO_TEST_CASE(test_uuid_ordering) {
    // UUID(most_sig_bits, least_sig_bits)
    auto uuid1 = utils::UUID(int64_t(0x0000000000001000), int64_t(0x8000000000000001LL));
    auto uuid2 = utils::UUID(int64_t(0xffffffffffff1fffLL), int64_t(0xbfffffffffffffffLL));
    auto e1 = encode_partition_key({data_value(uuid1)}, {uuid_type});
    auto e2 = encode_partition_key({data_value(uuid2)}, {uuid_type});
    BOOST_REQUIRE(e1 < e2);
}

// ============================================================================
// Partition key encoding — test_composite_partition_key
// Composite partition key: PRIMARY KEY ((text, int, int))
// ============================================================================

BOOST_AUTO_TEST_CASE(test_composite_partition_key) {
    std::vector<data_type> types = {utf8_type, int32_type, int32_type};

    std::vector<std::vector<data_value>> rows = {
        {data_value("aaa"), data_value(int32_t(1)),   data_value(int32_t(10))},
        {data_value("aaa"), data_value(int32_t(1)),   data_value(int32_t(20))},
        {data_value("aaa"), data_value(int32_t(2)),   data_value(int32_t(5))},
        {data_value("aaa"), data_value(int32_t(100)), data_value(int32_t(1))},
        {data_value("abc"), data_value(int32_t(-1)),  data_value(int32_t(1))},
        {data_value("abc"), data_value(int32_t(0)),   data_value(int32_t(1))},
        {data_value("zzz"), data_value(int32_t(1)),   data_value(int32_t(1))},
    };

    std::vector<std::vector<uint8_t>> encoded;
    for (auto& row : rows) {
        encoded.push_back(encode_partition_key(row, types));
    }

    for (size_t i = 1; i < encoded.size(); ++i) {
        BOOST_REQUIRE_MESSAGE(encoded[i - 1] < encoded[i],
            "Composite ordering broken at row " + std::to_string(i));
    }
}

// ============================================================================
// tests.rs — test_float64_roundtrip_comprehensive
// ============================================================================

BOOST_AUTO_TEST_CASE(test_float64_roundtrip_comprehensive) {
    double values[] = {
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::lowest(),
        -1e100, -1.0,
        -std::numeric_limits<double>::min(),
        -0.0, 0.0,
        std::numeric_limits<double>::min(),
        1.0, 1e100,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
    };
    for (double v : values) {
        uint64_t encoded = float64_to_lex_u64(v);
        double decoded = lex_u64_to_float64(encoded);
        uint64_t v_bits, decoded_bits;
        std::memcpy(&v_bits, &v, sizeof(v_bits));
        std::memcpy(&decoded_bits, &decoded, sizeof(decoded_bits));
        BOOST_REQUIRE_EQUAL(v_bits, decoded_bits);
    }
}

// ============================================================================
// varint / decimal: encoded order equals CQL order, including equality across
// decimal scales (1 == 1.0 == 1.00).
// ============================================================================

static void require_same_order_as_type(const data_type& type, const std::vector<data_value>& values) {
    for (const auto& a : values) {
        for (const auto& b : values) {
            auto sa = a.serialize_nonnull();
            auto sb = b.serialize_nonnull();
            auto want = type->compare(sa, sb);
            auto ea = encode_serialized_partition_key({sa}, {type});
            auto eb = encode_serialized_partition_key({sb}, {type});
            auto got = compare_unsigned(ea, eb);
            BOOST_REQUIRE_MESSAGE((want < 0) == (got < 0) && (want == 0) == (got == 0),
                    type->to_string(sa) + " vs " + type->to_string(sb));
        }
    }
}

BOOST_AUTO_TEST_CASE(test_varint_ordering) {
    std::vector<data_value> values;
    for (const char* s : {"0", "1", "-1", "127", "128", "-128", "-129", "255", "256", "-256",
                          "9223372036854775807", "9223372036854775808", "-9223372036854775809",
                          "123456789012345678901234567890", "-123456789012345678901234567890"}) {
        values.emplace_back(utils::multiprecision_int(boost::multiprecision::cpp_int(s)));
    }
    require_same_order_as_type(varint_type, values);
}

BOOST_AUTO_TEST_CASE(test_decimal_ordering) {
    std::vector<data_value> values;
    for (const char* s : {"0", "0.00", "1", "1.0", "1.00", "10", "1E+1", "0.1", "0.10", "-1", "-1.0",
                          "-0.1", "-0.12", "-0.121", "0.12", "0.121", "123.456", "-123.456",
                          "1E-10", "-1E-10", "1E+10", "-1E+10"}) {
        values.emplace_back(big_decimal(std::string_view(s)));
    }
    require_same_order_as_type(decimal_type, values);
}

// ============================================================================
// Every partition key type, ASC and DESC: the encoded order equals the CQL
// comparator's. A reversed type compares in the opposite order, so the same
// check covers DESC.
// ============================================================================

static void require_same_order_both_directions(const data_type& type, const std::vector<data_value>& values) {
    require_same_order_as_type(type, values);
    require_same_order_as_type(reversed_type_impl::get_instance(type), values);
}

// A version 1 UUID from a 60-bit timestamp and the least significant bits.
static utils::UUID v1_uuid(uint64_t t, uint64_t lsb) {
    uint64_t msb = (t & 0xffffffff) << 32 | ((t >> 32) & 0xffff) << 16 | 0x1000 | ((t >> 48) & 0x0fff);
    return utils::UUID(int64_t(msb), int64_t(lsb));
}

static std::vector<utils::UUID> v1_uuids() {
    std::vector<utils::UUID> r;
    // Time order differs from byte order: time_low is serialized first.
    for (uint64_t t : {uint64_t(0), uint64_t(1), uint64_t(0xffffffff), uint64_t(0x100000000), uint64_t(0x1000000000000),
                       uint64_t(0xfffffffffffffff), uint64_t(0x123456789abcdef)}) {
        for (uint64_t lsb : {uint64_t(0), uint64_t(0x7fffffffffffffff), uint64_t(0x8000000000000000),
                             uint64_t(0x80ff00ff80ff00ff), uint64_t(0xffffffffffffffff), uint64_t(0x0080008000800080)}) {
            r.push_back(v1_uuid(t, lsb));
        }
    }
    return r;
}

BOOST_AUTO_TEST_CASE(test_text_and_blob_both_directions) {
    std::vector<data_value> texts;
    for (const char* s : {"", "a", "aa", "ab", "abc", "abd", "b", "xy", "xyz", "zzz", "\x7f", "\xc3\xa9"}) {
        texts.emplace_back(sstring(s));
    }
    texts.emplace_back(sstring("a\0", 2));
    texts.emplace_back(sstring("a\0\0", 3));
    texts.emplace_back(sstring("a\0b", 3));
    require_same_order_both_directions(utf8_type, texts);
    std::vector<data_value> blobs;
    for (auto b : {bytes(), bytes({int8_t(0)}), bytes({int8_t(0), int8_t(0)}), bytes({int8_t(0), int8_t(1)}),
                   bytes({int8_t(1)}), bytes({int8_t(-1)}), bytes({int8_t(-1), int8_t(-1)}), bytes({int8_t(-2), int8_t(-1)})}) {
        blobs.emplace_back(data_value(b));
    }
    require_same_order_both_directions(bytes_type, blobs);
}

BOOST_AUTO_TEST_CASE(test_integers_both_directions) {
    std::vector<data_value> ints, bigints, smallints, tinyints;
    for (int64_t v : {int64_t(0), int64_t(1), int64_t(-1), int64_t(127), int64_t(-128), int64_t(255), int64_t(256),
                      int64_t(-100), int64_t(32767), int64_t(-32768), int64_t(70000), int64_t(-70000),
                      int64_t(std::numeric_limits<int32_t>::max()), int64_t(std::numeric_limits<int32_t>::min()),
                      std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}) {
        bigints.emplace_back(v);
        ints.emplace_back(int32_t(v));
        smallints.emplace_back(int16_t(v));
        tinyints.emplace_back(int8_t(v));
    }
    require_same_order_both_directions(long_type, bigints);
    require_same_order_both_directions(int32_type, ints);
    require_same_order_both_directions(short_type, smallints);
    require_same_order_both_directions(byte_type, tinyints);
    std::vector<data_value> varints;
    for (const char* s : {"0", "1", "-1", "127", "128", "-128", "-129", "255", "256", "-256",
                          "123456789012345678901234567890", "-123456789012345678901234567890"}) {
        varints.emplace_back(utils::multiprecision_int(boost::multiprecision::cpp_int(s)));
    }
    require_same_order_both_directions(varint_type, varints);
}

BOOST_AUTO_TEST_CASE(test_decimal_both_directions) {
    std::vector<data_value> values;
    for (const char* s : {"0", "0.00", "1", "1.0", "10", "1E+1", "0.1", "-1", "-0.1", "-0.12", "0.12",
                          "123.456", "-123.456", "1E-10", "-1E-10", "1E+10", "-1E+10"}) {
        values.emplace_back(big_decimal(std::string_view(s)));
    }
    require_same_order_both_directions(decimal_type, values);
}

BOOST_AUTO_TEST_CASE(test_boolean_both_directions) {
    require_same_order_both_directions(boolean_type, {data_value(false), data_value(true)});
}

BOOST_AUTO_TEST_CASE(test_times_both_directions) {
    std::vector<data_value> timestamps, dates, times;
    for (int64_t ms : {int64_t(0), int64_t(1), int64_t(-1), int64_t(1000), int64_t(-86400000), int64_t(1714564800000)}) {
        timestamps.emplace_back(db_clock::time_point(db_clock::duration(ms)));
    }
    for (uint32_t d : {uint32_t(0), uint32_t(1), uint32_t(1u << 31), uint32_t((1u << 31) + 1), uint32_t(0xffffffff)}) {
        dates.emplace_back(simple_date_native_type{d});
    }
    for (int64_t ns : {int64_t(0), int64_t(1), int64_t(1000000000), int64_t(86399999999999)}) {
        times.emplace_back(time_native_type{ns});
    }
    require_same_order_both_directions(timestamp_type, timestamps);
    require_same_order_both_directions(simple_date_type, dates);
    require_same_order_both_directions(time_type, times);
}

// CQL compares floats as Java does: -0.0 < 0.0, and NaN (any NaN) equals NaN
// and is greater than everything else.
BOOST_AUTO_TEST_CASE(test_floats_both_directions) {
    std::vector<double> ds = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1e300, -1e300, 5e-324, -5e-324,
                              std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
                              std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    std::vector<data_value> doubles, floats;
    for (double d : ds) {
        doubles.emplace_back(d);
        floats.emplace_back(float(d));
    }
    // NaNs: the default quiet NaN, a negative NaN and one with a payload.
    for (uint64_t bits : {uint64_t(0x7ff8000000000000), uint64_t(0xfff8000000000000), uint64_t(0x7ff0000000000001)}) {
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        doubles.emplace_back(d);
    }
    for (uint32_t bits : {uint32_t(0x7fc00000), uint32_t(0xffc00000), uint32_t(0x7f800001)}) {
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        floats.emplace_back(f);
    }
    require_same_order_both_directions(double_type, doubles);
    require_same_order_both_directions(float_type, floats);
}

BOOST_AUTO_TEST_CASE(test_uuid_both_directions) {
    std::vector<data_value> values;
    for (auto& u : v1_uuids()) {
        values.emplace_back(u);
    }
    // Other versions, which CQL orders by version first, then by bytes.
    for (uint64_t msb : {uint64_t(0x0000000000004000), uint64_t(0xffffffffffff4fff), uint64_t(0x1234567890ab4cde),
                         uint64_t(0x0000000000000000), uint64_t(0xffffffffffff0fff), uint64_t(0x0000000000002000),
                         uint64_t(0xffffffffffff7fff), uint64_t(0x000000000000f000)}) {
        for (uint64_t lsb : {uint64_t(0), uint64_t(0x8000000000000000), uint64_t(0xffffffffffffffff)}) {
            values.emplace_back(utils::UUID(int64_t(msb), int64_t(lsb)));
        }
    }
    require_same_order_both_directions(uuid_type, values);
}

BOOST_AUTO_TEST_CASE(test_timeuuid_both_directions) {
    std::vector<data_value> values;
    for (auto& u : v1_uuids()) {
        values.emplace_back(timeuuid_native_type{u});
    }
    require_same_order_both_directions(timeuuid_type, values);
}

BOOST_AUTO_TEST_CASE(test_inet_both_directions) {
    std::vector<data_value> values;
    for (const char* a : {"0.0.0.0", "0.0.0.1", "1.2.3.4", "10.0.0.1", "127.0.0.1", "192.168.0.1", "255.255.255.255",
                          "::", "::1", "::ffff:1.2.3.4", "fe80::1", "2001:db8::1", "ffff::", "0:ff::", "100::"}) {
        values.emplace_back(seastar::net::inet_address(a));
    }
    require_same_order_both_directions(inet_addr_type, values);
}

// ============================================================================
// DESC is the bitwise complement of the full ascending encoding.
// ============================================================================

BOOST_AUTO_TEST_CASE(test_desc_is_complement_of_asc) {
    auto desc_text = reversed_type_impl::get_instance(utf8_type);
    for (const char* s : {"", "a", "xyz"}) {
        auto v = utf8_type->decompose(sstring(s));
        auto asc = encode_serialized_partition_key({v}, {utf8_type});
        auto desc = encode_serialized_partition_key({v}, {desc_text});
        BOOST_REQUIRE_EQUAL(asc.size(), desc.size());
        for (size_t i = 0; i < asc.size(); ++i) {
            BOOST_REQUIRE_EQUAL(uint8_t(desc[i]), uint8_t(~asc[i]));
        }
    }
    // The terminator becomes 0xFF 0xFF, an escaped 0x00 becomes 0xFF 0xFE.
    auto desc = encode_serialized_partition_key({bytes({int8_t(0)})}, {reversed_type_impl::get_instance(bytes_type)});
    BOOST_REQUIRE(desc == bytes({int8_t(0xff), int8_t(0xfe), int8_t(0xff), int8_t(0xff)}));
}

// kaebase cluster_key.rs test_text_desc_variable_length: "xyz" > "xy", so DESC
// puts "xyz" first.
BOOST_AUTO_TEST_CASE(test_text_desc_variable_length) {
    auto desc_text = reversed_type_impl::get_instance(utf8_type);
    auto xyz = encode_serialized_partition_key({utf8_type->decompose(sstring("xyz"))}, {desc_text});
    auto xy = encode_serialized_partition_key({utf8_type->decompose(sstring("xy"))}, {desc_text});
    auto abz = encode_serialized_partition_key({utf8_type->decompose(sstring("abz"))}, {desc_text});
    BOOST_REQUIRE(compare_unsigned(xyz, xy) < 0);
    BOOST_REQUIRE(compare_unsigned(xy, abz) < 0);
}

// ============================================================================
// Composite keys with mixed directions: the first differing component decides,
// each in its own direction. Ported from kaebase cluster_key.rs
// test_composite_asc_desc_asc_variable_length and test_composite_fixed_width.
// ============================================================================

static void require_rows_ascending(const std::vector<data_type>& types, const std::vector<std::vector<data_value>>& rows) {
    std::vector<bytes> encoded;
    for (auto& row : rows) {
        std::vector<bytes> components;
        for (auto& v : row) {
            components.push_back(v.serialize_nonnull());
        }
        encoded.push_back(encode_serialized_partition_key(components, types));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        BOOST_REQUIRE_MESSAGE(compare_unsigned(encoded[i - 1], encoded[i]) < 0, format("rows {} and {}", i - 1, i));
    }
}

BOOST_AUTO_TEST_CASE(test_composite_asc_desc_asc_variable_length) {
    std::vector<data_type> types = {utf8_type, reversed_type_impl::get_instance(utf8_type), int32_type};
    auto row = [] (const char* a, const char* b, int32_t c) {
        return std::vector<data_value>{data_value(sstring(a)), data_value(sstring(b)), data_value(c)};
    };
    require_rows_ascending(types, {
        row("aaa", "zz", 25), row("aaa", "yy", 23), row("aaa", "yy", 24), row("aaa", "xx", 22), row("aaa", "aa", 21),
        row("abc", "xyz", 20), row("abc", "xy", 18), row("abc", "xy", 19), row("abc", "abz", 17), row("abc", "aaa", 16),
        row("xy", "m", 15), row("xy", "l", 13), row("xy", "l", 14), row("xy", "k", 12), row("xy", "a", 11),
        row("zyx", "c", 10), row("zyx", "b", 8), row("zyx", "b", 9), row("zyx", "a", 6), row("zyx", "a", 7),
        row("zzz", "aba", 5), row("zzz", "aac", 4), row("zzz", "aab", 3), row("zzz", "aaa", 1), row("zzz", "aaa", 2),
    });
}

BOOST_AUTO_TEST_CASE(test_composite_fixed_width) {
    std::vector<data_type> types = {int32_type, reversed_type_impl::get_instance(int32_type), int32_type};
    auto row = [] (int32_t a, int32_t b, int32_t c) {
        return std::vector<data_value>{data_value(a), data_value(b), data_value(c)};
    };
    require_rows_ascending(types, {row(1, 100, 1), row(1, 50, 2), row(1, 50, 3), row(1, 10, 4), row(2, 200, 5), row(2, 100, 6)});
}

// An empty component (allowed in materialized view keys) sorts first ASC and
// last DESC.
BOOST_AUTO_TEST_CASE(test_desc_empty_component) {
    auto desc_text = reversed_type_impl::get_instance(utf8_type);
    auto empty = encode_serialized_partition_key({bytes()}, {desc_text});
    auto a = encode_serialized_partition_key({utf8_type->decompose(sstring("a"))}, {desc_text});
    BOOST_REQUIRE(compare_unsigned(a, empty) < 0);
}

// ============================================================================
// prefix_successor: the planner's upper bound for "every key starting with p".
// ============================================================================

BOOST_AUTO_TEST_CASE(test_prefix_successor) {
    auto b = [] (std::initializer_list<uint8_t> l) {
        bytes r(bytes::initialized_later(), l.size());
        std::copy(l.begin(), l.end(), reinterpret_cast<uint8_t*>(r.data()));
        return r;
    };
    BOOST_REQUIRE(prefix_successor(b({0x61, 0x00, 0x00})) == b({0x61, 0x00, 0x01}));
    BOOST_REQUIRE(prefix_successor(b({0x61, 0xff, 0xff})) == b({0x62}));
    BOOST_REQUIRE(prefix_successor(b({0x61, 0x00, 0xff})) == b({0x61, 0x01}));
    BOOST_REQUIRE(prefix_successor(b({0xfe})) == b({0xff}));
    BOOST_REQUIRE(!prefix_successor(b({0xff, 0xff})));
    BOOST_REQUIRE(!prefix_successor(bytes()));
    // Every key with a DESC prefix lies in [p, successor(p)).
    auto desc_text = reversed_type_impl::get_instance(utf8_type);
    auto p = encode_serialized_partition_key({utf8_type->decompose(sstring("b"))}, {desc_text});
    auto s = *prefix_successor(p);
    for (const char* next : {"", "a", "zzz"}) {
        auto k = encode_serialized_partition_key({utf8_type->decompose(sstring("b")), utf8_type->decompose(sstring(next))},
                {desc_text, desc_text});
        BOOST_REQUIRE(compare_unsigned(p, k) <= 0);
        BOOST_REQUIRE(compare_unsigned(k, s) < 0);
    }
    for (const char* other : {"a", "ba", "c", ""}) {
        auto k = encode_serialized_partition_key({utf8_type->decompose(sstring(other))}, {desc_text});
        BOOST_REQUIRE(compare_unsigned(k, p) < 0 || compare_unsigned(s, k) <= 0);
    }
}
