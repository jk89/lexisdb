# Lexis

<table><tr>
<td valign="middle" width="160"><img src="lexis-icon.svg" width="160" height="196" alt="Lexis"></td>
<td valign="middle">

Lexis is a fork of ScyllaDB that replaces the Murmur3 partitioner with
byte-ordered partitioning, built on tablets. (ScyllaDB removed its
ByteOrderedPartitioner in 4.0.) The token of a partition is an
order-preserving encoding of its partition key, so partitions are stored and
distributed in key order, and prefix and range queries over partition keys are
served as range reads instead of scattering across the cluster.

</td>
</tr></table>

## The original constraint

ByteOrderedPartitioner was deprecated primarily because ordered partitioning causes severe imbalance when multiple tables have different key distributions. The token boundaries that produce good distribution for one table's key space are wrong for another's. With a cluster-global token ring this is a genuine problem and the objection was reasonable at the time.

The secondary objection, hotspots from sequential writes, reinforced this position.

## Why tablets eliminate the constraint

Tablets give each table its own independent token-range-to-node-and-core mapping. There is no shared token ring across tables. The multi-table distribution problem that was the principal objection to ByteOrderedPartitioner does not exist in a tablets architecture by construction.

The hotspot objection is also addressed. A hot tablet gets split and migrated dynamically by the tablet load balancer. With a sufficiently high-entropy leading key component, writes distribute naturally across the keyspace, and any residual concentration is handled by the load balancer without schema changes. This is the mechanism working as designed.

## What ordered partitioning restores

Prefix scans over contiguous key ranges become possible: queries that currently require secondary indexes become efficient range reads routed to a bounded set of nodes and cores. Partial partition key queries, which under Murmur3 require a full cluster scatter, can be served by contiguous token range lookups. The expressiveness of the query model improves substantially without any sacrifice in distribution or performance.

Secondary indexes and denormalized tables used to work around hash-distributed keys become unnecessary for range-ordered access patterns.

## Prior art

This is the model Google Bigtable has offered since 2004: lexicographic ordering with per-table tablet-based distribution and dynamic splitting. The open source distributed database ecosystem deprecated the one feature that would have given it equivalent query semantics, based on a constraint that modern tablet architectures have already eliminated.

## Why ScyllaDB

ScyllaDB is uniquely positioned to address this. The per-core shared-nothing architecture means a tablet split results in work moving to a specific core on a specific node, so the granularity of load balancing is finer than anything else available. Combining per-table ordered tablets with this architecture would produce something that does not currently exist in any production open source distributed database: Bigtable's query model on modern hardware.

## Approach

Re-enable ordered partitioning as the partitioner for all tables in ScyllaDB's tablet architecture. The tablet load balancer handles distribution and its splitting heuristics evolve iteratively on top of this foundation.

## Partition key order

Partitions are stored, distributed and returned in key order, and Lexis adds
`PARTITION ORDER BY` to choose that order: each partition key column can be
declared ASC or DESC, as `CLUSTERING ORDER BY` does for clustering columns.
The direction is fixed when the table is created, ASC is the default, and
directions can be mixed.

    CREATE TABLE user_orders (user_id text, order_id text, item text,
                              PRIMARY KEY ((user_id, order_id)))
        WITH PARTITION ORDER BY (user_id ASC, order_id DESC);

The token encodes each column in its direction: an ASC column as
order-preserving bytes, a DESC column as their complement, so its largest
value comes first. Every scalar type sorts as CQL compares it, in either
direction and with every range operator; text and blobs sort
lexicographically.

## Motivation

Records that belong to a parent are a common pattern: a user's orders, a
tenant's documents, a device's readings. Key them by the parent's id and then
their own, with both as the partition key, and every record is its own
partition. For example, `user_orders` keyed by `(user_id, order_id)`, with
each user's newest orders first:

    CREATE TABLE user_orders (user_id text, order_id text, item text,
                              PRIMARY KEY ((user_id, order_id)))
        WITH PARTITION ORDER BY (user_id ASC, order_id DESC);

    INSERT INTO user_orders (user_id, order_id, item) VALUES ('u7',  'o5',   'pen');
    INSERT INTO user_orders (user_id, order_id, item) VALUES ('u42', 'o250', 'mug');
    INSERT INTO user_orders (user_id, order_id, item) VALUES ('u1',  'o2',   'lamp');
    INSERT INTO user_orders (user_id, order_id, item) VALUES ('u42', 'o100', 'desk');
    INSERT INTO user_orders (user_id, order_id, item) VALUES ('u1',  'o1',   'book');
    INSERT INTO user_orders (user_id, order_id, item) VALUES ('u42', 'o101', 'chair');

     user_id | order_id | item
    ---------+----------+-------
     u1      | o2       | lamp
     u1      | o1       | book
     u42     | o250     | mug
     u42     | o101     | chair
     u42     | o100     | desk
     u7      | o5       | pen

In ScyllaDB the token is a hash of the whole key, so only a complete key can
be looked up efficiently. Listing a user's orders, paging through them, or
ranging over users all name just the start of the key, and ScyllaDB rejects them:

    Cannot execute this query as it might involve data filtering and thus may have unpredictable performance. If you want to execute this query despite the performance unpredictability, use ALLOW FILTERING

or, for a range on part of the key:

    Only EQ and IN relation are supported on the partition key (unless you use the token() function or allow filtering)

With ALLOW FILTERING they run as full scans of every node and return rows in
hash order. The usual workarounds are a secondary index, a second table keyed
for each access pattern, or moving order_id into a clustering column, which
puts all of a user's orders into one partition that cannot be split.

In Lexis the token is the key itself, encoded so that byte order is key order.
Keys that share a prefix are stored together, so each of these patterns
becomes a read of one contiguous range of tablets, in key order, without
ALLOW FILTERING.

**The whole table, in key order\*:**

    SELECT * FROM user_orders;

     u1  | o2   | lamp
     u1  | o1   | book
     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk
     u7  | o5   | pen

\* `'u42'` comes before `'u7'` due to lexicographic ordering. If you want
numeric order, make the column a numeric type such as `bigint`.

Name the start of a partition key. Matches share a token prefix, so they are
one contiguous range.

**All of one user's orders, newest first:**

    SELECT * FROM user_orders WHERE user_id = 'u42';

     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk

**A user's latest orders:**

    SELECT * FROM user_orders WHERE user_id = 'u42' LIMIT 2;

     u42 | o250 | mug
     u42 | o101 | chair

A range on contiguous partition key columns, beginning at the first column, is
a token range too.

**The next page, resuming after the last one seen\*:**

    SELECT * FROM user_orders WHERE user_id = 'u42'
        AND order_id < 'o101' LIMIT 2;

     u42 | o100 | desk

\* `order_id` is DESC, so the next page is the smaller ids and resumes with `<`.

**A window of a user's orders:**

    SELECT * FROM user_orders WHERE user_id = 'u42'
        AND order_id >= 'o100' AND order_id < 'o200';

     u42 | o101 | chair
     u42 | o100 | desk

**A user's orders up to a given one:**

    SELECT * FROM user_orders WHERE user_id = 'u42' AND order_id <= 'o101';

     u42 | o101 | chair
     u42 | o100 | desk

**A range of users\*:**

    SELECT * FROM user_orders WHERE user_id >= 'u1' AND user_id < 'u5';

     u1  | o2   | lamp
     u1  | o1   | book
     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk

\* `'u42'` falls inside `['u1', 'u5')` due to lexicographic ordering.

**Every user after a given one:**

    SELECT * FROM user_orders WHERE user_id > 'u1';

     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk
     u7  | o5   | pen

**Every user before a given one\*:**

    SELECT * FROM user_orders WHERE user_id < 'u7';

     u1  | o2   | lamp
     u1  | o1   | book
     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk

\* `'u42'` is included due to lexicographic ordering.

`IN` on a component gives one range per value.

**Several users at once:**

    SELECT * FROM user_orders WHERE user_id IN ('u1', 'u42');

     u1  | o2   | lamp
     u1  | o1   | book
     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk

**Several users at once, from a given order on\*:**

    u42 / *                ok: all of 'u42''s orders
    u42 / o100..o200       ok: a range at the end
    {u1, u42} / o100..*    ok: two users, each ending in a range
    u1..u5 / *             ok: a range of users, no order named
    * / o100               not ok: no user named, so the matches are
                           not next to each other

    SELECT * FROM user_orders WHERE user_id IN ('u1', 'u42')
        AND order_id > 'o1';

     u1  | o2   | lamp
     u42 | o250 | mug
     u42 | o101 | chair
     u42 | o100 | desk

\* `'o1'` is excluded, and all of `'u42'`'s orders are included since
`'o100' > 'o1'` lexicographically.

**Several users at once, within a window of orders\*:**

    SELECT * FROM user_orders WHERE user_id IN ('u1', 'u42')
        AND order_id >= 'o100' AND order_id < 'o200';

     u1  | o2   | lamp
     u42 | o101 | chair
     u42 | o100 | desk

\* `'o2'` falls inside `['o100', 'o200')` due to lexicographic ordering.

**Specific orders of one user:**

    SELECT * FROM user_orders WHERE user_id = 'u42'
        AND order_id IN ('o100', 'o250');

     u42 | o250 | mug
     u42 | o100 | desk

**Specific orders of several users:**

    SELECT * FROM user_orders WHERE user_id IN ('u1', 'u42')
        AND order_id IN ('o1', 'o100');

     u1  | o1   | book
     u42 | o100 | desk

Partition key order is the storage order, just as clustering key order already
was.

**The first rows of the whole table, in key order** (in ScyllaDB, LIMIT returns
whichever rows hash first):

    SELECT * FROM user_orders LIMIT 2;

     u1 | o2 | lamp
     u1 | o1 | book

**Highest first, and numbers in numeric order:** DESC on the leading column,
with a double key.

    CREATE TABLE scores (score double, player text,
                         PRIMARY KEY ((score, player)))
        WITH PARTITION ORDER BY (score DESC);

    INSERT INTO scores (score, player) VALUES (42.25, 'dee');
    INSERT INTO scores (score, player) VALUES (88,    'cy');
    INSERT INTO scores (score, player) VALUES (-3.5,  'eve');
    INSERT INTO scores (score, player) VALUES (97.5,  'ana');
    INSERT INTO scores (score, player) VALUES (88,    'bo');

     score | player
    -------+--------
     97.5  | ana
     88    | bo
     88    | cy
     42.25 | dee
     -3.5  | eve

    SELECT * FROM scores LIMIT 3;

     97.5 | ana
     88   | bo
     88   | cy

    SELECT * FROM scores WHERE score > 0 AND score < 90;

     88    | bo
     88    | cy
     42.25 | dee

    SELECT * FROM scores WHERE score < 50;

     42.25 | dee
     -3.5  | eve

    SELECT * FROM scores WHERE score = 88 AND player > 'bo';

     88 | cy

**token() is the key:** each component's bytes, then `00 00`. A DESC
component is the complement, ending in `ff ff`.

    SELECT token(user_id, order_id) FROM user_orders
        WHERE user_id = 'u42' AND order_id = 'o100';

     0x753432000090cecfcfffff

     753432 0000 90cecfcf ffff
     u42         o100

**The next page of the whole table, resuming after the last key seen:**

    SELECT * FROM user_orders
        WHERE token(user_id, order_id) > 0x7531000090ceffff LIMIT 2;

     u42 | o250 | mug
     u42 | o101 | chair

**Bind markers:** every query above can be prepared. The Lexis drivers
compute the same token range as the server and send the statement to the
replica and shard that hold it: a full key goes straight to its partition,
and a prefix, or a prefix ending in a range, goes to the tablet where its
range starts, which for most prefixes is the only one.

    q = session.prepare("SELECT * FROM user_orders WHERE user_id = ?"
                        " AND order_id < ? LIMIT 2")
    session.execute(q, ('u42', 'o101'))

     u42 | o100 | desk

**Queries that still need ALLOW FILTERING:**

    SELECT * FROM user_orders WHERE order_id = 'o100';

    Cannot execute this query as it might involve data filtering and thus may have unpredictable performance. If you want to execute this query despite the performance unpredictability, use ALLOW FILTERING

    SELECT * FROM user_orders WHERE user_id > 'u1' AND order_id = 'o5';

    Only EQ and IN relation are supported on the partition key (unless you use the token() function or ALLOW FILTERING)

With ALLOW FILTERING the first scans the whole table. The second reads only
the users after `'u1'` and filters them:

    SELECT * FROM user_orders WHERE user_id > 'u1' AND order_id = 'o5' ALLOW FILTERING;

     u7 | o5 | pen

## Components

| Repository | Docker image |
|---|---|
| [lexisdb](https://github.com/jk89/lexisdb) (this repository, the database) | `0x6a6f6e6e79/lexisdb` |
| [lexis-python-driver](https://github.com/jk89/lexis-python-driver) | |
| [lexis-rust-driver](https://github.com/jk89/lexis-rust-driver) | |
| [lexis-cqlsh](https://github.com/jk89/lexis-cqlsh) | `0x6a6f6e6e79/lexis-cqlsh` |
| [lexis-tools-java](https://github.com/jk89/lexis-tools-java) | |
| [lexis-jmx](https://github.com/jk89/lexis-jmx) | |

Run a single node:

    docker run --name lexisdb -d 0x6a6f6e6e79/lexisdb

## Building

Lexis builds with the standard ScyllaDB toolchain container. See
[guide-build-and-test.md](guide-build-and-test.md); upstream's build
documentation is in [HACKING.md](HACKING.md).

## Base and licence

Lexis is forked from `scylla-6.2.3` (commit `bff9ddde12`, 2025-01-19), the
last tagged open-source ScyllaDB release, which is licensed under the GNU
AGPL ([LICENSE.AGPL](LICENSE.AGPL)). From `branch-2025.1` onward, ScyllaDB is
under the source-available ScyllaDB Software License. Lexis is licensed
AGPL-3.0-or-later.

The upstream ScyllaDB README is kept unchanged in [README_SCYLLA.md](README_SCYLLA.md).
