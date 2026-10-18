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

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include "types/types.hh"

namespace dht {
namespace ordered_encoding {

uint64_t float64_to_lex_u64(double value);
double lex_u64_to_float64(uint64_t lex);
uint32_t float32_to_lex_u32(float value);

std::vector<uint8_t> encode_partition_key(
    const std::vector<data_value>& values,
    const std::vector<data_type>& types);

// Encodes the serialized components of a partition key (as held by
// partition_key) into token bytes. An empty component, which materialized
// views allow, encodes as just the terminator and sorts before every
// non-empty value of its column.
bytes encode_serialized_partition_key(
    const std::vector<bytes>& components,
    const std::vector<data_type>& types);

} // namespace ordered_encoding
} // namespace dht
