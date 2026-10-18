/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "dht/ordered_partitioner.hh"
#include "dht/ordered_encoding.hh"
#include "schema/schema.hh"
#include "sstables/key.hh"
#include "utils/class_registrator.hh"

namespace dht {

token
ordered_partitioner::get_token(const schema& s, partition_key_view key) const {
    return token(ordered_encoding::encode_serialized_partition_key(key.explode(s), s.partition_key_type()->types()));
}

token
ordered_partitioner::get_token(const schema& s, const sstables::key_view& key) const {
    return get_token(s, key.to_partition_key(s));
}

using registry = class_registrator<i_partitioner, ordered_partitioner>;
static registry registrator(ordered_partitioner::classname);
static registry registrator_short_name("LexisByteOrderedPartitioner");

}
