# SPDX-License-Identifier: AGPL-3.0-or-later

#############################################################################
# Tests for Lexis prefix routing (LEXIS_PREFIX_ROUTING_V1): a prepared SELECT
# that reads a partition key prefix, a range on the next partition key column,
# IN lists or a token() range carries a routing plan in its PREPARED response,
# from which the driver computes the token the read starts at. Standard
# partition key indexes are only given for complete keys bound by `=`.
#
# These tests use the Lexis python driver, which negotiates the extension and
# exposes the plan's token as BoundStatement.lexis_routing_token.
#############################################################################

import struct

import pytest
from cassandra import lexis
from util import new_test_table

TENANTS = ['a', 'b', 'b\x00', 'ba', 'c']
USERS = [-1000, -1, 0, 1, 5, 70000]


@pytest.fixture(scope="module", params=['ASC', 'DESC'])
def table(cql, test_keyspace, request):
    with new_test_table(cql, test_keyspace, "tenant text, user int, v int, PRIMARY KEY ((tenant, user))",
                        f" WITH PARTITION ORDER BY (user {request.param})") as t:
        ins = cql.prepare(f"INSERT INTO {t} (tenant, user, v) VALUES (?, ?, 0)")
        for tenant in TENANTS:
            for user in USERS:
                cql.execute(ins, [tenant, user])
        yield t, request.param == 'DESC'


def tokens(cql, table):
    return {(r.tenant, r.user): r[2] for r in cql.execute(f"SELECT tenant, user, token(tenant, user) FROM {table}")}


def check_start(cql, table, where, args):
    """The plan's start token is a valid start for the read: no matching row
    sorts before it, and the first matching row is at or after it."""
    stmt = cql.prepare(f"SELECT tenant, user FROM {table} WHERE {where}")
    assert stmt.lexis_routing_plan is not None, where
    start = stmt.bind(args).lexis_routing_token
    assert start is not None, where
    toks = tokens(cql, table)
    matching = {(r.tenant, r.user) for r in cql.execute(stmt, args)}
    assert all(toks[k] >= start for k in matching), (where, args)
    return stmt, start, matching


def test_pk_indexes_only_for_complete_eq_keys(cql, table):
    t, _ = table
    assert cql.prepare(f"SELECT v FROM {t} WHERE tenant = ? AND user = ?").routing_key_indexes == [0, 1]
    for where in ["tenant = ?", "tenant = ? AND user > ?", "tenant = ? AND user > ? AND user < ?",
                  "tenant = ? AND user IN ?", "tenant = ? AND user IN (?, ?)", "tenant IN ? AND user = ?",
                  "token(tenant, user) > ?"]:
        stmt = cql.prepare(f"SELECT v FROM {t} WHERE {where}")
        assert not stmt.routing_key_indexes, where
        assert stmt.lexis_routing_plan is not None, where


def test_prefix_start(cql, table):
    t, _ = table
    _, start, matching = check_start(cql, t, "tenant = ?", ['b'])
    assert start == lexis.encode(['text'], [b'b'])
    assert matching == {('b', u) for u in USERS}


def test_complete_key_plan_token_is_token(cql, table):
    t, desc = table
    stmt = cql.prepare(f"SELECT token(tenant, user) FROM {t} WHERE tenant = ? AND user = ?")
    for tenant in TENANTS:
        for user in USERS:
            bound = stmt.bind([tenant, user])
            assert bound.lexis_routing_token == cql.execute(bound).one()[0]


@pytest.mark.parametrize("op", ['<', '<=', '>', '>='])
def test_range_start(cql, table, op):
    t, desc = table
    for v in [-1001, -1, 0, 5, 70001]:
        _, start, matching = check_start(cql, t, f"tenant = ? AND user {op} ?", ['b', v])
        # The exact start: an ASC lower bound or a DESC upper bound is the
        # low end, the other kind starts at the prefix.
        enc = lexis.encode(['text', lexis.Descending('int') if desc else 'int'], [b'b', struct.pack('>i', v)])
        low_end = (op in ('>', '>=')) != desc
        if not low_end:
            assert start == lexis.encode(['text'], [b'b'])
        elif op in ('>=', '<='):
            assert start == enc
        else:
            assert start == lexis.successor(enc)


def test_two_sided_range_start(cql, table):
    t, desc = table
    check_start(cql, t, "tenant = ? AND user > ? AND user <= ?", ['c', -1, 5])
    stmt = cql.prepare(f"SELECT tenant, user FROM {t} WHERE tenant = ? AND user > ? AND user < ?")
    # An empty range has nothing to route to.
    assert stmt.bind(['c', 5, -1]).lexis_routing_token is None


def test_in_starts_at_smallest(cql, table):
    t, desc = table
    _, start, matching = check_start(cql, t, "tenant IN ?", [['c', 'a', 'ba']])
    assert start == lexis.encode(['text'], [b'a'])
    _, start, _ = check_start(cql, t, "tenant = ? AND user IN (?, ?)", ['b', 1, 70000])
    smallest = min(lexis.encode(['text', lexis.Descending('int') if desc else 'int'], [b'b', struct.pack('>i', u)])
                   for u in (1, 70000))
    assert start == smallest
    check_start(cql, t, "tenant IN ('b', 'c') AND user > ?", [0])


def test_literal_values(cql, table):
    t, _ = table
    _, start, _ = check_start(cql, t, "tenant = 'ba' AND user >= ?", [0])
    stmt = cql.prepare(f"SELECT v FROM {t} WHERE tenant = 'ba'")
    assert stmt.bind([]).lexis_routing_token == lexis.encode(['text'], [b'ba'])


def test_token_range_start(cql, table):
    t, _ = table
    toks = sorted(tokens(cql, t).values())
    mid = toks[len(toks) // 2]
    stmt = cql.prepare(f"SELECT tenant FROM {t} WHERE token(tenant, user) >= ?")
    assert stmt.bind([mid]).lexis_routing_token == mid
    stmt = cql.prepare(f"SELECT tenant FROM {t} WHERE token(tenant, user) > ? AND token(tenant, user) < ?")
    assert stmt.bind([mid, toks[-1]]).lexis_routing_token == mid + b'\x00'
    stmt = cql.prepare(f"SELECT tenant FROM {t} WHERE token(tenant, user) < ?")
    assert stmt.bind([mid]).lexis_routing_token == b''


def test_no_plan_when_not_computable(cql, table):
    t, _ = table
    # A function call in the value: the driver could not evaluate it.
    stmt = cql.prepare(f"SELECT v FROM {t} WHERE tenant = blobAsText(?)")
    assert stmt.lexis_routing_plan is None
    # No partition key restriction at all.
    assert cql.prepare(f"SELECT v FROM {t}").lexis_routing_plan is None
