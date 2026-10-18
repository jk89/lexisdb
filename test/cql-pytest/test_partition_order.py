# SPDX-License-Identifier: AGPL-3.0-or-later

#############################################################################
# Tests for PARTITION ORDER BY (Lexis): each partition key column can be
# declared ASC or DESC at CREATE TABLE. A DESC column's token component is the
# bitwise complement of its ascending encoding, so full scans return
# partitions in the declared order, and ranges on the column are still token
# ranges, needing no ALLOW FILTERING.
#
# The oracle for every type is a clustering column with the same CLUSTERING
# ORDER: the server sorts and slices clustering rows with the CQL comparator,
# so a partition key query must return exactly what the same query on the
# clustering column returns.
#
# The design is from kaebase (cluster_key.rs Direction, operators.rs
# invert_operator):
# https://github.com/kaepek/kaebase/blob/FEAT/scaling/v2/crates/common/src/encoding/cluster_key.rs
#############################################################################

import datetime
import decimal
import time
import uuid

import pytest
from cassandra.protocol import InvalidRequest, SyntaxException
from cassandra.util import Date, Time
from util import new_test_table, new_materialized_view


def v1_uuid(t, clock_seq_and_node):
    """A version 1 UUID with 60-bit timestamp t."""
    return uuid.UUID(fields=(t & 0xffffffff, (t >> 32) & 0xffff, 0x1000 | ((t >> 48) & 0x0fff),
                             clock_seq_and_node >> 56, (clock_seq_and_node >> 48) & 0xff,
                             clock_seq_and_node & 0xffffffffffff))


# Time order differs from byte order (time_low is serialized first), and the
# clock sequence and node bytes cover both signs.
V1_UUIDS = [v1_uuid(t, n) for t in (1, 0xffffffff, 0x100000000, 0x123456789abcdef)
            for n in (0x0080008000800080, 0x80ff00ff80ff00ff, 0xffffffffffffffff)]

TYPES = {
    'text': ['a', 'a\x00', 'a\x00b', 'ab', 'b', 'xy', 'xyz', 'z', 'é'],
    'blob': [b'\x00', b'\x00\x00', b'\x00\x01', b'\x01', b'\x7f', b'\xfe\xff', b'\xff', b'\xff\xff'],
    'boolean': [False, True],
    'tinyint': [-128, -1, 0, 1, 127],
    'smallint': [-32768, -256, -1, 0, 1, 255, 32767],
    'int': [-2**31, -70000, -1, 0, 1, 255, 256, 2**31 - 1],
    'bigint': [-2**63, -2**40, -1, 0, 1, 2**40, 2**63 - 1],
    'varint': [-10**30, -300, -1, 0, 5, 1000, 10**30],
    'decimal': [decimal.Decimal(s) for s in ('-1E+10', '-1.5', '-0.12', '-0.1', '0', '0.25', '1.00', '3', '1E+10')],
    'double': [float('-inf'), -1e300, -1.5, -0.0, 0.0, 5e-324, 0.5, 1e300, float('inf'), float('nan')],
    'float': [float('-inf'), -1.5, -0.0, 0.0, 0.5, 3.5, float('inf'), float('nan')],
    'timestamp': [datetime.datetime(1969, 12, 31, 23, 59, 59), datetime.datetime(1970, 1, 1),
                  datetime.datetime(2024, 5, 1, 12, 0, 0)],
    'date': [Date(-1000), Date(0), Date(1), Date(19000)],
    'time': [Time(0), Time(1), Time(10**9), Time(86399999999999)],
    'uuid': V1_UUIDS + [uuid.UUID('00000000-0000-4000-8000-000000000000'), uuid.UUID('ffffffff-ffff-4fff-bfff-ffffffffffff'),
                        uuid.UUID('12345678-90ab-4cde-8000-000000000001'), uuid.UUID('00000000-0000-0000-0000-000000000000'),
                        uuid.UUID('ffffffff-ffff-2fff-ffff-ffffffffffff')],
    'timeuuid': V1_UUIDS,
    'inet': ['0.0.0.1', '1.2.3.4', '10.0.0.1', '255.255.255.255', '::', '::1', '::ffff:1.2.3.4', '2001:db8::1',
             'fe80::1', 'ffff::'],
}

OPS = ['<', '<=', '>', '>=']


def key(v):
    # NaN != NaN in Python; repr tells NaNs, -0.0 and 0.0 apart.
    return repr(v)


def keys(rows):
    return [key(r[0]) for r in rows]


@pytest.fixture(scope="module", params=sorted(TYPES))
def typ(request):
    return request.param


@pytest.fixture(scope="module", params=['ASC', 'DESC'])
def direction(request):
    return request.param


@pytest.fixture(scope="module")
def tables(cql, test_keyspace, typ, direction):
    # (data table keyed by k, oracle table clustered by c), same values.
    with new_test_table(cql, test_keyspace, f"k {typ} PRIMARY KEY",
                        f" WITH PARTITION ORDER BY (k {direction})") as t, \
         new_test_table(cql, test_keyspace, f"p int, c {typ}, PRIMARY KEY (p, c)",
                        f" WITH CLUSTERING ORDER BY (c {direction})") as o:
        ins_t = cql.prepare(f"INSERT INTO {t} (k) VALUES (?)")
        ins_o = cql.prepare(f"INSERT INTO {o} (p, c) VALUES (0, ?)")
        for v in TYPES[typ]:
            cql.execute(ins_t, [v])
            cql.execute(ins_o, [v])
        yield t, o


def test_full_scan_in_declared_order(cql, tables, typ, direction):
    t, o = tables
    got = keys(cql.execute(f"SELECT k FROM {t}"))
    want = keys(cql.execute(f"SELECT c FROM {o} WHERE p = 0"))
    assert got == want
    assert len(got) == len(TYPES[typ])


def test_every_operator(cql, tables, typ):
    # Without ALLOW FILTERING: each one is a token range.
    t, o = tables
    for op in OPS:
        q_t = cql.prepare(f"SELECT k FROM {t} WHERE k {op} ?")
        q_o = cql.prepare(f"SELECT c FROM {o} WHERE p = 0 AND c {op} ?")
        for v in TYPES[typ]:
            assert keys(cql.execute(q_t, [v])) == keys(cql.execute(q_o, [v])), (op, v)


def test_two_sided_ranges(cql, tables, typ):
    t, o = tables
    vals = TYPES[typ]
    for lo_op, hi_op in (('>', '<'), ('>=', '<='), ('>', '<='), ('>=', '<')):
        q_t = cql.prepare(f"SELECT k FROM {t} WHERE k {lo_op} ? AND k {hi_op} ?")
        q_o = cql.prepare(f"SELECT c FROM {o} WHERE p = 0 AND c {lo_op} ? AND c {hi_op} ?")
        for i in range(len(vals)):
            for j in range(i, len(vals)):
                args = [vals[i], vals[j]]
                assert keys(cql.execute(q_t, args)) == keys(cql.execute(q_o, args)), (lo_op, hi_op, args)


def test_eq_and_in(cql, tables, typ):
    t, o = tables
    vals = TYPES[typ]
    q_t = cql.prepare(f"SELECT k FROM {t} WHERE k = ?")
    q_o = cql.prepare(f"SELECT c FROM {o} WHERE p = 0 AND c = ?")
    for v in vals:
        assert keys(cql.execute(q_t, [v])) == keys(cql.execute(q_o, [v])), v
    q_t = cql.prepare(f"SELECT k FROM {t} WHERE k IN ?")
    q_o = cql.prepare(f"SELECT c FROM {o} WHERE p = 0 AND c IN ?")
    assert keys(cql.execute(q_t, [vals[::2]])) == keys(cql.execute(q_o, [vals[::2]]))


@pytest.fixture(scope="module")
def table_ab(cql, test_keyspace):
    # (a ASC, b DESC), with a clustering oracle (a, b DESC).
    with new_test_table(cql, test_keyspace, "a text, b int, v int, PRIMARY KEY ((a, b))",
                        " WITH PARTITION ORDER BY (a ASC, b DESC)") as t, \
         new_test_table(cql, test_keyspace, "p int, a text, b int, PRIMARY KEY (p, a, b)",
                        " WITH CLUSTERING ORDER BY (a ASC, b DESC)") as o:
        ins_t = cql.prepare(f"INSERT INTO {t} (a, b, v) VALUES (?, ?, 0)")
        ins_o = cql.prepare(f"INSERT INTO {o} (p, a, b) VALUES (0, ?, ?)")
        for a in ['x', 'x\x00', 'xa', 'y', 'z']:
            for b in [-1000, -1, 0, 1, 7, 70000]:
                cql.execute(ins_t, [a, b])
                cql.execute(ins_o, [a, b])
        yield t, o


def ab(rows):
    return [(r.a, r.b) for r in rows]


def test_mixed_directions_full_scan(cql, table_ab):
    t, o = table_ab
    got = ab(cql.execute(f"SELECT a, b FROM {t}"))
    assert got == ab(cql.execute(f"SELECT a, b FROM {o} WHERE p = 0"))
    assert got[:3] == [('x', 70000), ('x', 7), ('x', 1)]


def test_desc_range_after_eq_prefix(cql, table_ab):
    t, o = table_ab
    for op in OPS:
        for v in [-1001, -1000, 0, 7, 70001]:
            got = ab(cql.execute(f"SELECT a, b FROM {t} WHERE a = 'x' AND b {op} {v}"))
            want = ab(cql.execute(f"SELECT a, b FROM {o} WHERE p = 0 AND a = 'x' AND b {op} {v}"))
            assert got == want, (op, v)
    got = ab(cql.execute(f"SELECT a, b FROM {t} WHERE a = 'y' AND b > -1 AND b <= 7"))
    assert got == [('y', 7), ('y', 1), ('y', 0)]


def test_in_with_desc_range(cql, table_ab):
    t, o = table_ab
    got = ab(cql.execute(f"SELECT a, b FROM {t} WHERE a IN ('z', 'x') AND b >= 0"))
    assert got == ab(cql.execute(f"SELECT a, b FROM {o} WHERE p = 0 AND a IN ('z', 'x') AND b >= 0"))
    assert got[0] == ('x', 70000)


def test_limit_returns_largest_first(cql, table_ab):
    t, _ = table_ab
    assert ab(cql.execute(f"SELECT a, b FROM {t} WHERE a = 'z' LIMIT 2")) == [('z', 70000), ('z', 7)]


def test_desc_first_component_range(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "a text, b int, PRIMARY KEY ((a, b))",
                        " WITH PARTITION ORDER BY (a DESC)") as t:
        for a in ['a', 'b', 'ba', 'c']:
            for b in [1, 2]:
                cql.execute(f"INSERT INTO {t} (a, b) VALUES ('{a}', {b})")
        assert ab(cql.execute(f"SELECT a, b FROM {t}")) == \
            [('c', 1), ('c', 2), ('ba', 1), ('ba', 2), ('b', 1), ('b', 2), ('a', 1), ('a', 2)]
        assert ab(cql.execute(f"SELECT a, b FROM {t} WHERE a < 'ba'")) == [('b', 1), ('b', 2), ('a', 1), ('a', 2)]
        assert ab(cql.execute(f"SELECT a, b FROM {t} WHERE a >= 'b' AND a < 'c'")) == \
            [('ba', 1), ('ba', 2), ('b', 1), ('b', 2)]


def test_token_is_complement_of_ascending(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY", " WITH PARTITION ORDER BY (k DESC)") as d, \
         new_test_table(cql, test_keyspace, "k int PRIMARY KEY") as a:
        cql.execute(f"INSERT INTO {d} (k) VALUES (5)")
        cql.execute(f"INSERT INTO {a} (k) VALUES (5)")
        td = cql.execute(f"SELECT token(k) FROM {d}").one()[0]
        ta = cql.execute(f"SELECT token(k) FROM {a}").one()[0]
        assert td == bytes(b ^ 0xff for b in ta)
        assert ta == b'\x80\x00\x01\x00\x01\x05\x00\x00'


def test_no_partition_order_is_ascending(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY", " WITH PARTITION ORDER BY (k ASC)") as t:
        for k in [3, -1, 2]:
            cql.execute(f"INSERT INTO {t} (k) VALUES ({k})")
        assert [r.k for r in cql.execute(f"SELECT k FROM {t}")] == [-1, 2, 3]
        assert 'PARTITION ORDER BY' not in cql.execute(f"DESC TABLE {t}").one().create_statement


def test_describe_and_schema(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "a text, b int, c int, PRIMARY KEY ((a, b), c)",
                        " WITH PARTITION ORDER BY (b DESC) AND CLUSTERING ORDER BY (c DESC)") as t:
        stmt = cql.execute(f"DESC TABLE {t}").one().create_statement
        assert 'PARTITION ORDER BY (a ASC, b DESC)' in stmt
        assert 'CLUSTERING ORDER BY (c DESC)' in stmt
        ks, name = t.split('.')
        orders = {r.column_name: r.clustering_order.lower() for r in cql.execute(
            f"SELECT column_name, clustering_order FROM system_schema.columns WHERE keyspace_name = '{ks}' AND table_name = '{name}'")}
        assert orders == {'a': 'none', 'b': 'desc', 'c': 'desc'}
        # The description recreates the same table.
        cql.execute(f"DROP TABLE {t}")
        cql.execute(stmt)
        cql.execute(f"INSERT INTO {t} (a, b, c) VALUES ('x', 1, 0)")
        cql.execute(f"INSERT INTO {t} (a, b, c) VALUES ('x', 2, 0)")
        assert [r.b for r in cql.execute(f"SELECT b FROM {t}")] == [2, 1]


def test_cannot_be_altered(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY, v int", " WITH PARTITION ORDER BY (k DESC)") as t:
        with pytest.raises(SyntaxException):
            cql.execute(f"ALTER TABLE {t} WITH PARTITION ORDER BY (k ASC)")
        with pytest.raises(Exception, match="partition key column types"):
            cql.execute(f"ALTER TABLE {t} ALTER k TYPE blob")
        cql.execute(f"ALTER TABLE {t} WITH comment = 'x'")
        for k in [1, 3, 2]:
            cql.execute(f"INSERT INTO {t} (k, v) VALUES ({k}, 0)")
        assert [r.k for r in cql.execute(f"SELECT k FROM {t}")] == [3, 2, 1]


def test_validation(cql, test_keyspace):
    for schema, extra, match in [
            ("k int, c int, PRIMARY KEY (k, c)", "PARTITION ORDER BY (c DESC)", "not a partition key column"),
            ("k int, v int, PRIMARY KEY (k)", "PARTITION ORDER BY (v DESC)", "not a partition key column"),
            ("k int, v int, PRIMARY KEY (k)", "PARTITION ORDER BY (nope DESC)", "not a partition key column"),
            ("a int, b int, PRIMARY KEY ((a, b))", "PARTITION ORDER BY (a DESC, a ASC)", "more than once")]:
        with pytest.raises(InvalidRequest, match=match):
            with new_test_table(cql, test_keyspace, schema, " WITH " + extra):
                pass


def eventually_equal(fun, want, timeout_s=10):
    deadline = time.time() + timeout_s
    while True:
        got = fun()
        if got == want or time.time() > deadline:
            return got
        time.sleep(0.1)


def test_materialized_view(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY, v int", " WITH PARTITION ORDER BY (k DESC)") as t:
        # A view takes its own directions: ASC unless it declares otherwise.
        with new_materialized_view(cql, t, "*", "v, k", "v IS NOT NULL AND k IS NOT NULL",
                                   "WITH PARTITION ORDER BY (v DESC)") as mv, \
             new_materialized_view(cql, t, "*", "v, k", "v IS NOT NULL AND k IS NOT NULL") as mv_asc:
            for k in range(5):
                cql.execute(f"INSERT INTO {t} (k, v) VALUES ({k}, {10 * k})")
            assert 'PARTITION ORDER BY (v DESC)' in cql.execute(f"DESC MATERIALIZED VIEW {mv}").one().create_statement
            assert eventually_equal(lambda: [r.v for r in cql.execute(f"SELECT v FROM {mv}")],
                                    [40, 30, 20, 10, 0]) == [40, 30, 20, 10, 0]
            assert [r.v for r in cql.execute(f"SELECT v FROM {mv} WHERE v < 25")] == [20, 10, 0]
            assert eventually_equal(lambda: [r.v for r in cql.execute(f"SELECT v FROM {mv_asc}")],
                                    [0, 10, 20, 30, 40]) == [0, 10, 20, 30, 40]


def test_secondary_index_on_desc_key(cql, test_keyspace):
    with new_test_table(cql, test_keyspace, "a int, b int, v int, PRIMARY KEY ((a, b))",
                        " WITH PARTITION ORDER BY (a DESC)") as t:
        cql.execute(f"CREATE INDEX ON {t} (b)")
        for a in range(4):
            cql.execute(f"INSERT INTO {t} (a, b, v) VALUES ({a}, 1, 0)")
        assert sorted(r.a for r in cql.execute(f"SELECT a FROM {t} WHERE b = 1")) == [0, 1, 2, 3]


def test_cdc_and_lwt_on_desc_key(cql, test_keyspace_vnodes):
    # CDC needs a vnode keyspace (no CDC with tablets, ScyllaDB issue #16317).
    test_keyspace = test_keyspace_vnodes
    with new_test_table(cql, test_keyspace, "k int PRIMARY KEY, v int",
                        " WITH PARTITION ORDER BY (k DESC) AND cdc = {'enabled': true}") as t:
        assert cql.execute(f"INSERT INTO {t} (k, v) VALUES (1, 10) IF NOT EXISTS").one()[0]
        assert not cql.execute(f"INSERT INTO {t} (k, v) VALUES (1, 20) IF NOT EXISTS").one()[0]
        assert cql.execute(f"UPDATE {t} SET v = 30 WHERE k = 1 IF v = 10").one()[0]
        cql.execute(f"INSERT INTO {t} (k, v) VALUES (2, 0)")
        assert [(r.k, r.v) for r in cql.execute(f"SELECT k, v FROM {t}")] == [(2, 0), (1, 30)]
        log = sorted((r.k, r.v) for r in cql.execute(f"SELECT k, v FROM {t}_scylla_cdc_log"))
        assert (1, 10) in log and (1, 30) in log and (2, 0) in log


@pytest.mark.parametrize("order", ['ASC', 'DESC'])
def test_distinct_nans_share_a_token(cql, test_keyspace, order):
    # Every NaN encodes as the canonical NaN, so two NaNs with different bits
    # are separate partitions with one token: the "same token, different key"
    # path.
    nan1, nan2 = '0x7ff8000000000000', '0x7ff8000000000001'
    with new_test_table(cql, test_keyspace, "k double PRIMARY KEY, v int",
                        f" WITH PARTITION ORDER BY (k {order})") as t:
        cql.execute(f"INSERT INTO {t} (k, v) VALUES (blobAsDouble({nan1}), 1)")
        cql.execute(f"INSERT INTO {t} (k, v) VALUES (blobAsDouble({nan2}), 2)")
        cql.execute(f"INSERT INTO {t} (k, v) VALUES (1.5, 3)")
        rows = {(r[0].hex(), r[1]) for r in cql.execute(f"SELECT doubleAsBlob(k), v FROM {t}")}
        assert rows == {(nan1[2:], 1), (nan2[2:], 2), ('3ff8000000000000', 3)}
        tokens = [cql.execute(f"SELECT token(k) FROM {t} WHERE k = blobAsDouble({n})").one()[0] for n in (nan1, nan2)]
        assert tokens[0] == tokens[1]
        for n, v in ((nan1, 1), (nan2, 2)):
            assert [r.v for r in cql.execute(f"SELECT v FROM {t} WHERE k = blobAsDouble({n})")] == [v]
        cql.execute(f"DELETE FROM {t} WHERE k = blobAsDouble({nan1})")
        assert [r.v for r in cql.execute(f"SELECT v FROM {t} WHERE k = blobAsDouble({nan1})")] == []
        assert [r.v for r in cql.execute(f"SELECT v FROM {t} WHERE k = blobAsDouble({nan2})")] == [2]
        rows = {(r[0].hex(), r[1]) for r in cql.execute(f"SELECT doubleAsBlob(k), v FROM {t}")}
        assert rows == {(nan2[2:], 2), ('3ff8000000000000', 3)}
