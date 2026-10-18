/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "bytes.hh"
#include "types/types.hh"
#include "utils/managed_bytes.hh"

#include <limits>
#include <seastar/net/byteorder.hh>
#include <fmt/format.h>
#include <functional>
#include <utility>
#include <compare>

namespace dht {

class token;

enum class token_kind {
    before_all_keys,
    key,
    after_all_keys,
};

// A token is the order-preserving encoding of a partition key
// (see dht/ordered_encoding.hh). Tokens of kind::key compare as unsigned
// byte strings (memcmp, then length), so token order is partition key order.
// before_all_keys / after_all_keys tokens carry no data.
//
// The bytes are managed_bytes, like partition keys: a token inside an object
// allocated in an LSA region (memtable and cache entries) lives in that
// region too, so it is accounted, compacted and evicted with the region
// instead of hiding in the standard heap. The same allocation rules as for
// partition keys apply: copy a token out of LSA memory under the standard
// allocator. Long tokens may be fragmented; see data().
class token {
public:
    using kind = token_kind;
    kind _kind;
    managed_bytes _data;

public:
    token() noexcept : _kind(kind::before_all_keys) {}

    // Also used by IDL deserialization, whose deserialized_bytes_proxy converts
    // to managed_bytes. (managed_bytes can only be constructed explicitly from
    // bytes, so a bytes argument picks the bytes_view overload unambiguously.)
    token(kind k, managed_bytes d) noexcept : _kind(k), _data(k == kind::key ? std::move(d) : managed_bytes()) {}
    token(kind k, bytes_view d) : token(k, k == kind::key ? managed_bytes(d) : managed_bytes()) {}

    explicit token(managed_bytes d) noexcept : token(kind::key, std::move(d)) {}
    explicit token(bytes_view d) : token(kind::key, d) {}

    // Tokens used to be integers. An integer (or 0, which is also a null
    // pointer) would silently convert to bytes here and construct a garbage
    // token -- or crash, for 0 -- so these are compile errors. Use
    // from_int64() for 64-bit boundary tokens.
    template <std::integral T>
    explicit token(T) = delete;
    explicit token(std::nullptr_t) = delete;
    template <std::integral T>
    token(kind, T) = delete;
    token(kind, std::nullptr_t) = delete;

    std::strong_ordering operator<=>(const token& o) const noexcept {
        if (_kind != o._kind) {
            return _kind <=> o._kind;
        } else if (_kind == token_kind::key) [[likely]] {
            return compare_unsigned(managed_bytes_view(_data), managed_bytes_view(o._data));
        }
        return std::strong_ordering::equal;
    }

    bool operator==(const token& o) const noexcept {
        return _kind == o._kind && (_kind != kind::key || _data == o._data);
    }

    // Constructing managed_bytes from data is an allocation point (it may
    // throw under allocation failure injection even when the data fits
    // inline), so noexcept functions use the default constructor or copies of
    // inline values, which are not.
    static token minimum() noexcept {
        return token{ kind::before_all_keys, managed_bytes() };
    }

    static token maximum() noexcept {
        return token{ kind::after_all_keys, managed_bytes() };
    }

    // Returns the smallest token in the ring which can be associated with a partition key.
    // 0x00 0x00 is the encoding of a single empty component (just the terminator);
    // every other encoded key sorts after it.
    //
    // There is no last(): encoded keys have no upper bound in byte order.
    // A copy of a token built at startup; its two bytes are inline, so
    // copying does not allocate (see minimum()).
    static token first() noexcept;

    // Returns the immediate successor of this token in byte order: the same
    // bytes with 0x00 appended. No byte string sorts strictly between t and
    // t + 0x00, so (t, u] and [t.next(), u] contain exactly the same keys.
    // The result is generally not a valid encoded key; it is only meaningful
    // as a range bound.
    //
    // Precondition: t.kind() == dht::token::kind::key
    token next() const {
        bytes d(bytes::initialized_later(), _data.size() + 1);
        auto out = d.begin();
        for (bytes_view frag : fragment_range(managed_bytes_view(_data))) {
            out = std::copy(frag.begin(), frag.end(), out);
        }
        *out = 0;
        return token{ kind::key, bytes_view(d) };
    }

    bool is_minimum() const noexcept {
        return _kind == kind::before_all_keys;
    }

    bool is_maximum() const noexcept {
        return _kind == kind::after_all_keys;
    }

    bool is_first() const noexcept {
        return _kind == kind::key && _data.size() == 2 && _data[0] == 0 && _data[1] == 0;
    }

    size_t external_memory_usage() const noexcept {
        return _data.external_memory_usage();
    }

    size_t memory_usage() const noexcept {
        return sizeof(token) + external_memory_usage();
    }

    // The token bytes. Long tokens may be fragmented (as LSA allocates large
    // objects in chunks); use with_linearized() or to_bytes() for a contiguous
    // copy.
    const managed_bytes& data() const noexcept {
        return _data;
    }

    template <typename Func>
    std::invoke_result_t<Func, bytes_view> with_linearized(Func&& func) const {
        return _data.with_linearized(std::forward<Func>(func));
    }

    /**
     * @return a string representation of this token (hex-encoded bytes)
     */
    sstring to_sstring() const;

    /**
     * Calculate a token representing the approximate "middle" of the given
     * range.
     *
     * @return The approximate midpoint between left and right.
     */
    static token midpoint(const token& left, const token& right);

    /**
     * @return a randomly generated token
     */
    static token get_random_token();

    /**
     * @return a token from string representation (hex-encoded bytes)
     */
    static dht::token from_sstring(const sstring& t);

    /**
     * @return a token from its byte representation
     */
    static dht::token from_bytes(bytes_view bytes);

    // Order-preserving serialization that also covers minimum/maximum tokens:
    // a kind byte (0x00 before_all_keys, 0x01 key, 0x02 after_all_keys)
    // followed by the key bytes. Unsigned byte order of the result is token
    // order, so it can be used as a blob clustering key (system.tablets) or
    // sent to clients (tablet routing).
    bytes to_tagged_bytes() const;
    static dht::token from_tagged_bytes(bytes_view b);

    /**
     * Calculate the deltas between tokens in the ring in order to compare
     *  relative sizes.
     *
     * @param sortedtokens a sorted List of tokens
     * @return the mapping from 'token' to 'percentage of the ring owned by that token'.
     */
    static std::map<token, float> describe_ownership(const std::vector<token>& sorted_tokens);

    static data_type get_token_validator();

    /**
     * Gets the first shard of the minimum token.
     */
    static constexpr unsigned shard_of_minimum_token() noexcept {
        return 0;  // hardcoded for now; unlikely to change
    }

    // Interim, until shard and tablet routing move to explicit split points:
    // projects the token onto the 64-bit ring used by static sharding and
    // power-of-two compaction groups. The projection is the first 8 bytes,
    // big-endian, zero-padded. It is monotonic (non-strictly) in token order,
    // so contiguous 64-bit ranges map to contiguous token ranges.
    uint64_t unbias() const noexcept;

    // Smallest token whose unbias() is n: the 8 big-endian bytes of n with
    // trailing zero bytes removed. For n != 0 this is never a valid encoded
    // key, because encoded keys always end in 0x00 0x00.
    static token bias(uint64_t n);

    // Interim, for formats that still store 64-bit tokens (CDC stream ids and
    // generations, repair history): the signed form of unbias(), whose order
    // matches token order. Only tokens produced by bias() -- vnode tokens,
    // tablet and shard boundaries -- round-trip, so to_int64() throws for
    // any other token instead of silently storing an approximation.
    // minimum_token() and maximum_token() map to INT64_MIN, as before.
    static int64_t to_int64(const token& t);
    static token from_int64(int64_t v);
};

inline token minimum_token() noexcept {
    return token::minimum();
}

inline token maximum_token() noexcept {
    return token::maximum();
}

std::ostream& operator<<(std::ostream& out, const token& t);

// Returns a successor for token t.
//
// Precondition: t.kind() == dht::token::kind::key
inline
token next_token(const token& t) {
    return t.next();
}

// Returns the smallest token in the ring which can be associated with a partition key.
inline
token first_token() noexcept {
    return token::first();
}

inline uint64_t unbias(const token& t) noexcept {
    return t.unbias();
}
inline token bias(uint64_t n) {
    return token::bias(n);
}
size_t compaction_group_of(unsigned most_significant_bits, const token& t);
token last_token_of_compaction_group(unsigned most_significant_bits, size_t group);

struct token_comparator {
    // Return values are those of a trichotomic comparison.
    std::strong_ordering operator()(const token& t1, const token& t2) const noexcept {
        return t1 <=> t2;
    }
};

} // namespace dht

template <>
struct fmt::formatter<dht::token> : fmt::formatter<string_view> {
    template <typename FormatContext>
    auto format(const dht::token& t, FormatContext& ctx) const {
        if (t.is_maximum()) {
            return fmt::format_to(ctx.out(), "maximum token");
        } else if (t.is_minimum()) {
            return fmt::format_to(ctx.out(), "minimum token");
        } else {
            return fmt::format_to(ctx.out(), "{}", managed_bytes_view(t._data));
        }
    }
};

namespace std {

template<>
struct hash<dht::token> {
    size_t operator()(const dht::token& t) const {
        return std::hash<managed_bytes_view>()(managed_bytes_view(t._data));
    }
};

} // namespace std
