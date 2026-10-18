# SPDX-License-Identifier: AGPL-3.0-or-later

#############################################################################
# Tests for ordered partitioning (Lexis): the token of a partition is an
# order-preserving encoding of its key, so a prefix of the partition key, or
# a range on its next column, is a contiguous token range. Such queries need
# no ALLOW FILTERING, and full scans return partitions in key order.
#############################################################################

import pytest
from cassandra.protocol import InvalidRequest
from util import new_test_table

TENANTS = ['a', 'b', 'b\x00', 'ba', 'c']
USERS = [-1000, -3, -1, 0, 1, 2, 5, 255, 256, 70000]


@pytest.fixture(scope="module")
def table_tu(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "tenant text, user int, ck int, v int, PRIMARY KEY ((tenant, user), ck)") as table:
        stmt = cql.prepare(f"INSERT INTO {table} (tenant, user, ck, v) VALUES (?, ?, ?, ?)")
        for t in TENANTS:
            for u in USERS:
                for ck in range(2):
                    cql.execute(stmt, [t, u, ck, ck])
        yield table


def keys(rows):
    return [(r.tenant, r.user) for r in rows]


def expected(pred):
    return [(t, u) for t in sorted(TENANTS) for u in sorted(USERS) for _ in range(2) if pred(t, u)]


def test_full_scan_is_in_key_order(cql, table_tu):
    assert keys(cql.execute(f"SELECT tenant, user FROM {table_tu}")) == expected(lambda t, u: True)


def test_prefix_eq(cql, table_tu):
    assert keys(cql.execute(f"SELECT tenant, user FROM {table_tu} WHERE tenant = 'b'")) == \
        expected(lambda t, u: t == 'b')


def test_prefix_eq_with_nul(cql, table_tu):
    # 'b' and 'b\0' must not share a token prefix.
    stmt = cql.prepare(f"SELECT tenant, user FROM {table_tu} WHERE tenant = ?")
    assert keys(cql.execute(stmt, ['b\x00'])) == expected(lambda t, u: t == 'b\x00')


@pytest.mark.parametrize("op,fn", [
    ('>', lambda u, v: u > v),
    ('>=', lambda u, v: u >= v),
    ('<', lambda u, v: u < v),
    ('<=', lambda u, v: u <= v),
])
def test_prefix_eq_and_range(cql, table_tu, op, fn):
    stmt = cql.prepare(f"SELECT tenant, user FROM {table_tu} WHERE tenant = ? AND user {op} ?")
    for v in [-1000, -2, 0, 1, 256, 70001]:
        assert keys(cql.execute(stmt, ['b', v])) == expected(lambda t, u: t == 'b' and fn(u, v))


def test_prefix_eq_and_two_sided_range(cql, table_tu):
    assert keys(cql.execute(f"SELECT tenant, user FROM {table_tu} WHERE tenant = 'c' AND user > -3 AND user <= 255")) == \
        expected(lambda t, u: t == 'c' and -3 < u <= 255)


def test_prefix_in(cql, table_tu):
    assert keys(cql.execute(f"SELECT tenant, user FROM {table_tu} WHERE tenant IN ('c', 'a') AND user >= 0")) == \
        expected(lambda t, u: t in ('a', 'c') and u >= 0)


def test_range_on_first_column(cql, table_tu):
    assert keys(cql.execute(f"SELECT tenant, user FROM {table_tu} WHERE tenant > 'b' AND tenant < 'c'")) == \
        expected(lambda t, u: 'b' < t < 'c')
    assert keys(cql.execute(f"SELECT tenant, user FROM {table_tu} WHERE tenant >= 'ba'")) == \
        expected(lambda t, u: t >= 'ba')


def test_range_needs_leading_column(cql, table_tu):
    # A restriction that skips the leading column is not a token range.
    with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
        cql.execute(f"SELECT * FROM {table_tu} WHERE user > 0")


def test_prefix_with_clustering_restriction_needs_filtering(cql, table_tu):
    with pytest.raises(InvalidRequest, match="ALLOW FILTERING"):
        cql.execute(f"SELECT * FROM {table_tu} WHERE tenant = 'b' AND ck = 1")
    rows = cql.execute(f"SELECT tenant, user FROM {table_tu} WHERE tenant = 'b' AND ck = 1 ALLOW FILTERING")
    assert keys(rows) == [(t, u) for (t, u) in expected(lambda t, u: t == 'b')][::2]


def test_modification_over_range_rejected(cql, table_tu):
    with pytest.raises(InvalidRequest):
        cql.execute(f"DELETE FROM {table_tu} WHERE tenant = 'b' AND user > 0")


def test_token_is_blob(cql, table_tu):
    # One row per clustering row; all carry the partition's token.
    rows = list(cql.execute(f"SELECT token(tenant, user) FROM {table_tu} WHERE tenant = 'a' AND user = 0"))
    assert len(rows) == 2
    # 'a' + terminator, then int 0 (0x80000000, zero bytes escaped) + terminator.
    expected = b'a\x00\x00' + b'\x80\x00\x01\x00\x01\x00\x01' + b'\x00\x00'
    assert all(r[0] == expected for r in rows)


def test_negative_int_partition_key_order(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY, v int") as table:
        values = [-2**31, -70000, -1, 0, 1, 70000, 2**31 - 1]
        for k in values:
            cql.execute(f"INSERT INTO {table} (k, v) VALUES ({k}, 0)")
        assert [r.k for r in cql.execute(f"SELECT k FROM {table}")] == values
        assert [r.k for r in cql.execute(f"SELECT k FROM {table} WHERE k > -1")] == [k for k in values if k > -1]
        assert [r.k for r in cql.execute(f"SELECT k FROM {table} WHERE k <= 0")] == [k for k in values if k <= 0]


@pytest.mark.parametrize("typ,vals", [
    ("varint", ["-123456789012345678901234567890", "-300", "-1", "0", "5", "1000", "123456789012345678901234567890"]),
    ("decimal", ["-1E+10", "-1.5", "-0.12", "-0.1", "0", "0.25", "1.00", "3", "1E+10"]),
    ("bigint", ["-9223372036854775808", "-1", "0", "1", "9223372036854775807"]),
    ("timestamp", ["'1969-12-31 23:59:59+0000'", "'1970-01-01 00:00:00+0000'", "'2024-05-01 12:00:00+0000'"]),
])
def test_range_on_exact_type(cql, test_keyspace, typ, vals):
    # vals are in ascending order; full scans and ranges follow it without ALLOW FILTERING.
    with new_test_table(cql, test_keyspace, f"k {typ} PRIMARY KEY, v int") as table:
        for i, v in enumerate(vals):
            cql.execute(f"INSERT INTO {table} (k, v) VALUES ({v}, {i})")
        assert [r.v for r in cql.execute(f"SELECT v FROM {table}")] == list(range(len(vals)))
        mid = len(vals) // 2
        assert [r.v for r in cql.execute(f"SELECT v FROM {table} WHERE k > {vals[mid]}")] == list(range(mid + 1, len(vals)))
        assert [r.v for r in cql.execute(f"SELECT v FROM {table} WHERE k <= {vals[mid]}")] == list(range(0, mid + 1))


@pytest.mark.parametrize("typ,vals", [
    ("double", ["-1.5", "0.5", "2.5"]),
    ("float", ["-1.5", "0.5", "2.5"]),
])
def test_range_on_inexact_type_needs_filtering(cql, test_keyspace, typ, vals):
    # These encodings do not sort exactly like CQL, so a range on them is
    # not a token range: it keeps requiring ALLOW FILTERING, and still works with it.
    with new_test_table(cql, test_keyspace, f"k {typ} PRIMARY KEY, v int") as table:
        for v in vals:
            cql.execute(f"INSERT INTO {table} (k, v) VALUES ({v}, 0)")
        with pytest.raises(InvalidRequest):
            cql.execute(f"SELECT k FROM {table} WHERE k > 0")
        assert len(list(cql.execute(f"SELECT k FROM {table} WHERE k > 0 ALLOW FILTERING"))) == 2


def test_alter_partition_key_type_rejected(cql, test_keyspace):
    # Tokens encode the partition key by its column types; changing a type
    # would change every token without re-sorting stored data.
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY, v int") as table:
        with pytest.raises(Exception, match="partition key column types"):
            cql.execute(f"ALTER TABLE {table} ALTER k TYPE blob")
