/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <algorithm>
#include <limits>
#include <ostream>
#include <random>

#include "dht/token.hh"
#include "dht/token-sharding.hh"

namespace dht {

using uint128_t = unsigned __int128;

std::ostream& operator<<(std::ostream& out, const token& t) {
    fmt::print(out, "{}", t);
    return out;
}

static const token first_token_value{token::kind::key, bytes_view(std::array<int8_t, 2>{0x00, 0x00}.data(), 2)};

token token::first() noexcept {
    return first_token_value;
}

sstring token::to_sstring() const {
    return to_hex(to_bytes(_data));
}

// Treats each token as a base-256 fraction 0.b0 b1 b2 ... in [0, 1), with
// maximum_token() as 1.0, and returns the average. A wrapping range
// (left > right) is measured going around the ring, i.e. right + 1.
// The result keeps one byte more precision than the longer input, so it
// never collapses onto an endpoint unless the endpoints are equal as fractions.
token token::midpoint(const token& t1, const token& t2) {
    if (t1 == t2) {
        return t1;
    }
    const bytes a = to_bytes(t1._data);
    const bytes b = to_bytes(t2._data);
    const size_t n = std::max(a.size(), b.size()) + 1;
    bytes mid(bytes::initialized_later(), n);
    unsigned carry = 0;
    for (size_t i = n; i-- > 0;) {
        unsigned x = (i < a.size() ? uint8_t(a[i]) : 0u) + (i < b.size() ? uint8_t(b[i]) : 0u) + carry;
        mid[i] = int8_t(x & 0xff);
        carry = x >> 8;
    }
    unsigned integer_part = carry + t1.is_maximum() + t2.is_maximum() + (t2 < t1 ? 1 : 0);
    // Divide by two; the integer part of the result is dropped (mod 1).
    unsigned shift_in = integer_part & 1;
    for (size_t i = 0; i < n; ++i) {
        uint8_t v = mid[i];
        mid[i] = int8_t((shift_in << 7) | (v >> 1));
        shift_in = v & 1;
    }
    // Trailing zero bytes do not change the fraction; dropping them keeps the
    // midpoint of two bias() boundaries a bias() boundary.
    size_t len = mid.size();
    while (len > 0 && mid[len - 1] == 0) {
        --len;
    }
    mid.resize(len);
    token result(kind::key, bytes_view(mid));
    if (t1 < t2 && !(t1 <= result && result <= t2)) {
        // t2 is t1 padded with zero bytes: nothing lies strictly between them.
        return t1;
    }
    return result;
}

token token::get_random_token() {
    static thread_local std::default_random_engine re{std::random_device{}()};
    static thread_local std::uniform_int_distribution<uint64_t> dist(1);
    // Vnode tokens are boundaries, so build them with bias(): they then
    // round-trip through the 64-bit formats (see to_int64()).
    return bias(dist(re));
}

token token::from_sstring(const sstring& t) {
    if (t.empty()) {
        return minimum_token();
    }
    return token(kind::key, bytes_view(from_hex(t)));
}

token token::from_bytes(bytes_view bytes) {
    if (bytes.empty()) {
        return minimum_token();
    }
    return token(kind::key, bytes);
}

bytes token::to_tagged_bytes() const {
    bytes b(bytes::initialized_later(), _data.size() + 1);
    b[0] = int8_t(_kind);
    auto out = b.begin() + 1;
    for (bytes_view frag : fragment_range(managed_bytes_view(_data))) {
        out = std::copy(frag.begin(), frag.end(), out);
    }
    return b;
}

token token::from_tagged_bytes(bytes_view b) {
    if (b.empty() || uint8_t(b[0]) > uint8_t(kind::after_all_keys)) {
        throw runtime_exception(format("Invalid tagged token: {}", fmt_hex(b)));
    }
    auto k = kind(b[0]);
    if (k != kind::key && b.size() != 1) {
        throw runtime_exception(format("Invalid tagged token: {}", fmt_hex(b)));
    }
    return token(k, b.substr(1));
}

static float ratio_helper(uint64_t a, uint64_t b) {
    uint64_t val = (a > b) ? a - b : (a - b - 1);
    return val/(float)std::numeric_limits<uint64_t>::max();
}

std::map<token, float>
token::describe_ownership(const std::vector<token>& sorted_tokens) {
    std::map<token, float> ownerships;
    auto i = sorted_tokens.begin();

    // 0-case
    if (i == sorted_tokens.end()) {
        throw runtime_exception("No nodes present in the cluster. Has this node finished starting up?");
    }
    // 1-case
    if (sorted_tokens.size() == 1) {
        ownerships[sorted_tokens[0]] = 1.0;
    // n-case
    } else {
        // Ownership is approximated on the 64-bit projection of each token.
        const token& start = sorted_tokens[0];

        uint64_t ti = start.unbias();  // The first token and its value
        uint64_t start_long = ti;
        uint64_t tim1 = ti; // The last token and its value (after loop)
        for (i++; i != sorted_tokens.end(); i++) {
            ti = i->unbias(); // The next token and its value
            ownerships[*i]= ratio_helper(ti, tim1);  // save (T(i) -> %age)
            tim1 = ti;
        }

        // The start token's range extends backward to the last token, which is why both were saved above.
        ownerships[start] = ratio_helper(start_long, ti);
    }

    return ownerships;
}

data_type
token::get_token_validator() {
    return bytes_type;
}

uint64_t token::unbias() const noexcept {
    uint64_t n = 0;
    size_t i = 0;
    for (bytes_view frag : fragment_range(managed_bytes_view(_data))) {
        for (auto c : frag) {
            if (i++ == sizeof(uint64_t)) {
                return n;
            }
            n = (n << 8) | uint8_t(c);
        }
    }
    for (; i < sizeof(uint64_t); ++i) {
        n <<= 8;
    }
    return n;
}

token token::bias(uint64_t n) {
    bytes b(bytes::initialized_later(), sizeof(uint64_t));
    write_unaligned<uint64_t>(b.begin(), net::hton(n));
    size_t len = sizeof(uint64_t);
    while (len > 0 && b[len - 1] == 0) {
        --len;
    }
    b.resize(len);
    return token(kind::key, std::move(b));
}

int64_t token::to_int64(const token& t) {
    if (t._kind != kind::key) {
        return std::numeric_limits<int64_t>::min();
    }
    uint64_t n = t.unbias();
    if (bias(n) != t) {
        throw runtime_exception(format("Token {} has no 64-bit representation", t));
    }
    return int64_t(n ^ (uint64_t(1) << 63));
}

token token::from_int64(int64_t v) {
    return bias(uint64_t(v) ^ (uint64_t(1) << 63));
}

inline
unsigned
zero_based_shard_of(uint64_t token, unsigned shards, unsigned sharding_ignore_msb_bits) {
    // This is the master function, the inverses have to match it wrt. rounding errors.
    token <<= sharding_ignore_msb_bits;
    // Treat "token" as a fraction in the interval [0, 1); compute:
    //    shard = floor((0.token) * shards)
    return (uint128_t(token) * shards) >> 64;
}

std::vector<uint64_t>
init_zero_based_shard_start(unsigned shards, unsigned sharding_ignore_msb_bits) {
    // computes the inverse of zero_based_shard_of(). ret[s] will return the smallest token that belongs to s
    if (shards == 1) {
        // Avoid the while loops below getting confused finding the "edge" between two nonexistent shards
        return std::vector<uint64_t>(1, uint64_t(0));
    }
    auto ret = std::vector<uint64_t>(shards);
    for (auto s : boost::irange<unsigned>(0, shards)) {
        uint64_t token = (uint128_t(s) << 64) / shards;
        token >>= sharding_ignore_msb_bits;   // leftmost bits are ignored by zero_based_shard_of
        // token is the start of the next shard, and can be slightly before due to rounding errors; adjust
        while (zero_based_shard_of(token, shards, sharding_ignore_msb_bits) != s) {
            ++token;
        }
        ret[s] = token;
    }
    return ret;
}

unsigned
shard_of(unsigned shard_count, unsigned sharding_ignore_msb_bits, const token& t) {
    switch (t._kind) {
        case token::kind::before_all_keys:
            return token::shard_of_minimum_token();
        case token::kind::after_all_keys:
            return shard_count - 1;
        case token::kind::key:
            uint64_t adjusted = unbias(t);
            return zero_based_shard_of(adjusted, shard_count, sharding_ignore_msb_bits);
    }
    abort();
}

token
token_for_next_shard(const std::vector<uint64_t>& shard_start, unsigned shard_count, unsigned sharding_ignore_msb_bits, const token& t, shard_id shard, unsigned spans) {
    uint64_t n = 0;
    switch (t._kind) {
        case token::kind::before_all_keys:
            break;
        case token::kind::after_all_keys:
            return maximum_token();
        case token::kind::key:
            n = unbias(t);
            break;
    }
    auto s = zero_based_shard_of(n, shard_count, sharding_ignore_msb_bits);

    if (!sharding_ignore_msb_bits) {
        // This ought to be the same as the else branch, but avoids shifts by 64
        n = shard_start[shard];
        if (spans > 1 || shard <= s) {
            return maximum_token();
        }
    } else {
        auto left_part = n >> (64 - sharding_ignore_msb_bits);
        left_part += spans - unsigned(shard > s);
        if (left_part >= (1u << sharding_ignore_msb_bits)) {
            return maximum_token();
        }
        left_part <<= (64 - sharding_ignore_msb_bits);
        auto right_part = shard_start[shard];
        n = left_part | right_part;
    }
    return bias(n);
}

static
dht::token find_first_token_for_shard_in_not_wrap_around_range(const dht::static_sharder& sharder, dht::token start, dht::token end, size_t shard_idx) {
    // Invariant start < end, so start is not maximum_token()
    auto t = start.is_minimum() ? dht::token(dht::token::kind::key, bytes()) : start.next();
    if (sharder.shard_of(t) != shard_idx) {
        t = sharder.token_for_next_shard(t, shard_idx);
    }
    return std::min(t, end);
}

dht::token find_first_token_for_shard(
        const dht::static_sharder& sharder, dht::token start, dht::token end, size_t shard_idx) {
    if (start < end) { // Not a wrap around token range
        return find_first_token_for_shard_in_not_wrap_around_range(sharder, start, end, shard_idx);
    } else { // A wrap around token range
        dht::token t;
        if (!start.is_maximum()) {
            t = find_first_token_for_shard_in_not_wrap_around_range(sharder, start, dht::maximum_token(), shard_idx);
            if (!t.is_maximum()) {
                // This means we have found a token for shard shard_idx after start
                return t;
            }
        }
        // No token owned by shard shard_idx was found in (start, maximum)
        // so we have to search in (minimum, end]
        return find_first_token_for_shard_in_not_wrap_around_range(
                sharder, dht::minimum_token(), end, shard_idx);
    }
}

// Compaction group g covers the tokens in (last_token_of_compaction_group(g - 1),
// last_token_of_compaction_group(g)]. Group boundaries are bias(start of next group),
// which are never valid encoded keys, so the 64-bit projection decides the group
// for every key; only a boundary token itself has to be moved down into the group
// it closes.
size_t
compaction_group_of(unsigned most_significant_bits, const token& t) {
    if (!most_significant_bits) {
        return 0;
    }
    switch (t._kind) {
        case token::kind::before_all_keys:
            return 0;
        case token::kind::after_all_keys:
            return (1 << most_significant_bits) - 1;
        case token::kind::key:
            size_t group = unbias(t) >> (64 - most_significant_bits);
            if (group > 0 && t == bias(uint64_t(group) << (64 - most_significant_bits))) {
                return group - 1;
            }
            return group;
    }
    __builtin_unreachable();
}

token last_token_of_compaction_group(unsigned most_significant_bits, size_t group) {
    if (group == ((1ul << most_significant_bits) - 1)) {
        return maximum_token();
    }
    return bias((uint64_t(group) + 1) << (64 - most_significant_bits));
}

} // namespace dht
