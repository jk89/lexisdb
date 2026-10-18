/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "i_partitioner.hh"
#include "bytes.hh"

namespace dht {

// Order-preserving partitioner: the token of a partition key is its
// order-preserving encoding (dht/ordered_encoding.hh), so token order is
// partition key order and contiguous key ranges map to contiguous token ranges.
//
// The name ends in "ByteOrderedPartitioner" on purpose: drivers recognise that
// suffix and then treat tokens as opaque byte strings compared in byte order,
// printed as hex -- which Lexis tokens are. Drivers that do not recognise the
// partitioner name may fail outright (the Python driver does).
class ordered_partitioner final : public i_partitioner {
public:
    static constexpr auto classname = "org.lexis.dht.LexisByteOrderedPartitioner";

    ordered_partitioner() = default;
    virtual const sstring name() const override { return classname; }
    virtual token get_token(const schema& s, partition_key_view key) const override;
    virtual token get_token(const schema& s, const sstables::key_view& key) const override;
};

}
