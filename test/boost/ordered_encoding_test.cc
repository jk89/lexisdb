/*
 * Order-preserving encoding tests for partition key → token.
 *
 * Float lex transform tests ported from kaebase v2:
 *   crates/common/src/encoding/float.rs (7 tests)
 * Partition key encoding tests ported from:
 *   crates/common/src/encoding/cluster_key.rs (ASC-only subset)
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
