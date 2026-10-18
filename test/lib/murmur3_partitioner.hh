/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "dht/i_partitioner.hh"
#include "bytes.hh"

namespace tests {

// Cassandra's Murmur3Partitioner, for tests only: it lets the tests read the
// sstable fixtures in test/resource/sstables, which Cassandra and ScyllaDB
// wrote with it, as they are.
//
// The token of a key is token::from_int64() of its Murmur3 hash. from_int64()
// is monotonic, so these byte-string tokens sort exactly like the 64-bit
// Murmur3 tokens, and the static sharder (which shards on token::unbias(),
// i.e. hash + 2^63) places keys on the same shards as it did for Murmur3.
class murmur3_partitioner final : public dht::i_partitioner {
public:
    static constexpr auto classname = "org.apache.cassandra.dht.Murmur3Partitioner";

    murmur3_partitioner() = default;
    virtual const sstring name() const override { return classname; }
    virtual dht::token get_token(const schema& s, partition_key_view key) const override;
    virtual dht::token get_token(const schema& s, const sstables::key_view& key) const override;
    static dht::token get_token(bytes_view legacy_key);
};

// Makes tables use the Murmur3 partitioner by default, for test binaries that
// read the Murmur3 sstable fixtures. Call before any schema is created.
void use_murmur3_partitioner_by_default();

// The same, as a Boost.Test global fixture:
//   using tests::murmur3_partitioner_by_default;
//   BOOST_TEST_GLOBAL_FIXTURE(murmur3_partitioner_by_default);
struct murmur3_partitioner_by_default {
    murmur3_partitioner_by_default() { use_murmur3_partitioner_by_default(); }
};

}
