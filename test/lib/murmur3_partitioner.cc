/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "test/lib/murmur3_partitioner.hh"
#include "utils/murmur_hash.hh"
#include "schema/schema.hh"
#include "sstables/key.hh"
#include "utils/class_registrator.hh"

namespace tests {

// Like Cassandra, but without its special case of the empty key (see the
// upstream dht/murmur3_partitioner.cc): the token is the hash of the key's
// legacy (Cassandra composite) form.
dht::token
murmur3_partitioner::get_token(bytes_view legacy_key) {
    std::array<uint64_t, 2> hash;
    utils::murmur_hash::hash3_x64_128(legacy_key, 0, hash);
    return dht::token::from_int64(int64_t(hash[0]));
}

dht::token
murmur3_partitioner::get_token(const schema& s, const sstables::key_view& key) const {
    // On-disk keys are in the legacy form already.
    return key.with_linearized([] (bytes_view v) {
        return get_token(v);
    });
}

dht::token
murmur3_partitioner::get_token(const schema& s, partition_key_view key) const {
    std::array<uint64_t, 2> hash;
    auto&& legacy = key.legacy_form(s);
    utils::murmur_hash::hash3_x64_128(legacy.begin(), legacy.size(), 0, hash);
    return dht::token::from_int64(int64_t(hash[0]));
}

void use_murmur3_partitioner_by_default() {
    schema::set_default_partitioner(murmur3_partitioner::classname, 12);
}

using registry = class_registrator<dht::i_partitioner, murmur3_partitioner>;
static registry registrator(murmur3_partitioner::classname);
static registry registrator_short_name("Murmur3Partitioner");

}
