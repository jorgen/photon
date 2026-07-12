# photon

[![CI](https://github.com/jorgen/photon/actions/workflows/ci.yml/badge.svg)](https://github.com/jorgen/photon/actions/workflows/ci.yml)

An async **PostgreSQL** client for **C++23**, built on
[vio](https://github.com/jorgen/vio) (async I/O: libuv + coroutines + TLS) and
[structify](https://github.com/jorgen/structify) (compile-time struct reflection).

photon speaks the PostgreSQL wire protocol directly over vio's sockets and
coroutines — no libpq — and binds result columns straight into a struct you
define. The light theme continues from vio and prism: a **photon** is the quantum
of light, one typed row dispersed from a query.

```cpp
#include <photon/photon.h>
#include <vio/run.h>

struct user_t
{
  std::int32_t id;
  std::string name;
  std::optional<std::int32_t> age;   // NULL -> empty optional
  STFY_OBJ(id, name, age);
};

VIO_MAIN(loop, argc, argv)
{
  auto conn = co_await photon::connection_t::connect(loop, "postgresql://user:pw@localhost/shop");
  if (!conn) { std::println(stderr, "connect: {}", conn->error().msg); co_return 1; }

  auto users = co_await (*conn)->query<user_t>(
    "SELECT id, name, age FROM users WHERE age > $1 ORDER BY id", 18);
  if (!users) { std::println(stderr, "query: {}", users.error().msg); co_return 1; }

  for (auto row : *users)          // each row is a result_t<user_t>
  {
    if (!row) break;
    std::println("{} {}", row->id, row->name);   // or: auto [id, name, age] = *row;
  }
  co_return 0;
}
```

- **Typed rows** — define a `Row` struct with `STFY_OBJ(...)`; photon decodes each
  result column into the matching field. `Row` is a plain aggregate, so C++
  structured bindings work.
- **Binary, extended protocol** — `Parse`/`Bind`/`Execute` with `$n` parameters
  (injection-safe) and binary result values decoded per type.
- **Async on vio** — every call is a `vio::task_t`, so it `co_await`s without
  blocking the event loop.
- **TLS / `sslmode`** — `disable`/`allow`/`prefer`/`require`/`verify-ca`/`verify-full`,
  negotiated over the same socket (SSLRequest), with cert + hostname verification.
- **SCRAM-SHA-256** and cleartext authentication.
- **Transactions** — `co_await conn->begin()` gives a move-only `transaction_t`;
  `co_await txn.commit()` / `rollback()`. Dropping it un-committed poisons the
  connection so its open transaction is discarded.
- **Pipelining** — batch many statements into one round-trip via a builder with
  typed result slots, or a variadic one-shot that returns a typed `std::tuple`;
  independent (per-statement) or atomic (all-or-nothing) modes.
- **COPY** — stream bulk data in (`copy_in`) and out (`copy_out`) far faster than
  row-by-row `INSERT`/`SELECT`.
- **Named parameters** — write `:name` placeholders and bind by name; the client
  rewrites them to `$n` (SQL-aware: casts, strings, and comments are left alone).
- **Cancellation & timeouts** — cancel an in-flight query from another coroutine
  (`cancel_handle`), or bound every query with `query_timeout`.
- **Rich types** — beyond the scalars, `uuid`, `timestamp`/`timestamptz`
  (`std::chrono::system_clock::time_point`), `date` (`std::chrono::sys_days`),
  `bytea`, `json`/`jsonb`, and `numeric` decode straight into ergonomic C++ types.
- **LISTEN / NOTIFY** — an async `on_notification` callback, or a dedicated
  `next_notification()` listener.
- **Rich errors** — server errors expose the full `ErrorResponse` (sqlstate, detail,
  hint, constraint, table, column, …) with classification helpers.
- **Lean & consistent** — errors flow through `result_t<T>`
  (`std::expected<T, error_t>`), mirroring vio and prism; built `-fno-exceptions -fno-rtti`.

## Connecting

```cpp
// From a DSN (postgresql:// URI: userinfo, host/port, /db, ?sslmode=…&application_name=…):
auto conn = co_await photon::connection_t::connect(loop, "postgresql://user:pw@db.internal:5432/shop?sslmode=require");

// Or from an explicit params struct:
photon::connect_params_t params;
params.host = "db.internal";
params.user = "user";
params.password = "pw";
params.database = "shop";
params.sslmode = photon::sslmode_t::verify_full;
params.sslrootcert = "/etc/ssl/db-ca.pem";
auto conn = co_await photon::connection_t::connect(loop, params);
```

`connect` returns `result_t<std::shared_ptr<connection_t>>` — a ready connection or
an error, never a half-open handle. Dropping the last `shared_ptr` closes it (a TLS
connection flushes `close_notify`); `co_await conn->close()` closes explicitly.
`conn->is_broken()` reports a connection poisoned by a transport failure.

## Queries

```cpp
// A set of typed rows:
auto rows = co_await conn->query<user_t>("SELECT id, name, age FROM users");
auto all  = rows->collect();            // result_t<std::vector<user_t>>

// A single row (or none):
auto one = co_await conn->query_one<user_t>("SELECT id, name, age FROM users WHERE id = $1", 42);
// one : result_t<std::optional<user_t>>

// A single scalar (handy for aggregates):
auto n = co_await conn->query_value<std::int64_t>("SELECT count(*) FROM users");
// n : result_t<std::optional<std::int64_t>>

// A statement with no rows returns a command result:
auto ins = co_await conn->execute("INSERT INTO users(id, name) VALUES ($1, $2)", 1, "ada");
// ins->rows_affected == 1
```

### Parameters and arrays

Parameters are positional (`$1`, `$2`, …), sent as text and parsed by the server per
the inferred type — integers, floating point, bool, strings, `std::optional<T>`
(→ NULL), and `std::vector<T>` (→ a Postgres array, e.g. for `= ANY($1)`):

```cpp
auto rows = co_await conn->query<user_t>(
  "SELECT id, name, age FROM users WHERE id = ANY($1)", std::vector<int>{1, 2, 3});
```

Teach photon a custom type by specialising `photon::param_codec_t<T>` (encode) and
`photon::value_codec_t<T>` (decode).

### Data types

Include `<photon/types.h>` (already pulled in by `<photon/photon.h>`) for codecs
beyond the built-in scalars. They bind as ordinary `Row` fields and as `$n`
parameters, and `std::optional<T>` handles NULL:

| Postgres                    | C++                                        |
| --------------------------- | ------------------------------------------ |
| `uuid`                      | `photon::uuid_t` (`.str()`, `::parse`)     |
| `timestamp` / `timestamptz` | `std::chrono::system_clock::time_point`    |
| `date`                      | `std::chrono::sys_days`                     |
| `bytea`                     | `photon::bytea_t { std::vector<std::byte> }` |
| `json` / `jsonb`            | `photon::json_t { std::string }`           |
| `numeric`                   | `photon::numeric_t { std::string }`        |

```cpp
struct event_t
{
  photon::uuid_t id;
  std::chrono::system_clock::time_point at;   // timestamptz, microsecond precision
  photon::numeric_t amount;                    // exact decimal, as a string
  STFY_OBJ(id, at, amount);
};
auto rows = co_await conn->query<event_t>("SELECT id, at, amount FROM events WHERE at > $1", since);
```

`numeric` decodes to its exact decimal string (`NaN`/`Infinity` preserved); `jsonb`
strips the wire version byte; `timestamptz` is a UTC instant at microsecond
precision.

### Transactions

```cpp
auto txn = co_await conn->begin();          // sends BEGIN
if (!txn) { /* handle */ }

auto moved = co_await conn->execute("UPDATE accounts SET balance = balance - $1 WHERE id = $2", 10, 1);
if (!moved) { co_await txn->rollback(); }
else        { co_await txn->commit(); }
```

`transaction_t` is a move-only **borrow** of the connection — keep the connection
(or its pool `lease`) alive for the transaction's lifetime. Because a destructor
cannot `co_await`, dropping a still-active transaction (no `commit`/`rollback`)
**poisons** the connection (`is_broken()`), so the pool evicts it and closing it
discards the open server transaction. Prefer an explicit `commit()` / `rollback()`.

### Prepared statements

```cpp
auto stmt = co_await conn->prepare("SELECT id, name, age FROM users WHERE id = $1");
for (int id : ids)
{
  auto row = co_await stmt->query_one<user_t>(id);   // parsed once, bound many times
}
```

## Pipelining

Running N statements one-at-a-time costs N network round-trips. **Pipelining** sends
the whole batch in one write and reads all the replies together — one round-trip.
Build it with `conn->pipeline()`; each `query`/`execute` queues a step and hands
back a typed slot you read after `run()`:

```cpp
auto pipe = conn->pipeline();
auto users = pipe.query<user_t>("SELECT id, name, age FROM users WHERE age > $1", 18);
auto n     = pipe.execute("UPDATE users SET seen = now() WHERE id = $1", 42);
co_await pipe.run();                 // one round-trip

auto rows = users.get();             // result_t<result_set_t<user_t>>
auto upd  = n.get();                 // result_t<command_result_t>
```

Or the one-shot form, which returns a typed `std::tuple` — one element per step:

```cpp
auto batch = co_await conn->pipeline(
  photon::pquery<user_t>("SELECT id, name, age FROM users WHERE age > $1", 18),
  photon::pexecute("UPDATE users SET seen = now() WHERE id = $1", 42));
auto &[users, n] = batch;            // users : result_t<result_set_t<user_t>>,  n : result_t<command_result_t>
```

By default steps are **independent** — each is its own implicit transaction, so one
failing statement does not stop the others (its slot just carries the error), and
`run()` succeeds as long as the connection held. Pass
`photon::pipeline_mode_t::atomic` for all-or-nothing: the batch runs as a single
implicit transaction, so **check `run()`'s result** — it returns the server error
if the transaction did not commit (a failing step *or* a commit-time failure such
as a deferred constraint or serialization error), and the whole batch is rolled
back:

```cpp
auto pipe = conn->pipeline(photon::pipeline_mode_t::atomic);
pipe.execute("INSERT INTO ledger(id, delta) VALUES ($1, $2)", 1, -100);
pipe.execute("INSERT INTO ledger(id, delta) VALUES ($1, $2)", 2,  100);
auto committed = co_await pipe.run();   // ok only if the whole batch committed
```

`run()` writes and reads concurrently, so a large batch (big request *and* big
responses) never deadlocks. In **independent** mode `run()` fails only on a
connection/protocol break; per-statement server errors live in each slot. Keep the
batch bounded — the whole request and all results are buffered in memory.

## Named parameters

Prefer names over positions? Write `:name` placeholders and bind with a
`named_args_t`; photon rewrites them to `$1, $2, …` client-side (repeats reuse one
position) and sends the ordinary extended-protocol query:

```cpp
photon::named_args_t args;
args.set("min_age", 18).set("name", std::string("ada"));
auto rows = co_await conn->query<user_t>(
  "SELECT id, name, age FROM users WHERE age >= :min_age AND name = :name", args);
```

The rewriter is SQL-aware: `::` casts, `'...'` / `E'...'` / `$tag$…$tag$` string
literals, `"quoted"` identifiers, and `--` / `/* */` comments are left untouched —
only real `:name` tokens are substituted. (A `:name` with no supplied value is a
400-style error, not a silent NULL.)

## COPY (bulk load & unload)

For bulk data, `COPY` is far faster than row-by-row statements. `copy_in` streams
rows to the server; `copy_out` streams them back:

```cpp
// Load: COPY ... FROM STDIN
auto in = co_await conn->copy_in("COPY events(id, label) FROM STDIN");
std::vector<std::optional<std::string>> row{std::optional<std::string>("1"),
                                            std::optional<std::string>("start")};
co_await in->write_row(row);                 // text format, escaping + \N for NULL handled
co_await in->write(std::string_view("2\tstop\n"));   // or write raw COPY text yourself
auto loaded = co_await in->finish();         // result_t<command_result_t>; rows_affected

// Unload: COPY ... TO STDOUT
auto out = co_await conn->copy_out("COPY events TO STDOUT");
auto text = co_await out->read_all();        // whole stream, or read_chunk() incrementally
```

Both borrow the connection for the duration; a `copy_in`/`copy_out` dropped mid
stream poisons the connection (its half-finished COPY is aborted), so drive it to
`finish()` / end-of-stream on the happy path. Pass CSV/binary options in the SQL
(`COPY … WITH (FORMAT csv)`) and format the bytes accordingly.

## Cancellation & timeouts

Cancel a query that is already running — from a *different* coroutine, since the
one that issued it is suspended — with a cancel handle (it opens a fresh
connection and sends the PostgreSQL CancelRequest):

```cpp
auto handle = conn->cancel_handle();          // cheap, copyable; capture before the query
auto running = conn->execute("CALL slow_report()");   // eager task, starts now
co_await handle.cancel(loop);                 // ... elsewhere / on a timer
auto result = co_await std::move(running);    // -> server error, sqlstate 57014
```

Or bound *every* query on a connection with a timeout — set
`connect_params_t::query_timeout` (0 = off, the default). On expiry the read is
cancelled, the call returns `error_kind_t::timeout`, and the connection is marked
broken (`is_broken()`), so a pool drops it. A broken connection fast-fails every
further call instead of hanging.

## Errors

Failures are `photon::error_t { error_kind_t kind; std::string msg;
std::optional<server_error_t> server; }`. When the server rejects a statement, the
full `ErrorResponse` is available and can be classified:

```cpp
auto r = co_await conn->execute("INSERT INTO users(id) VALUES ($1)", 1);
if (!r)
{
  if (photon::is_unique_violation(r.error()))
  {
    const auto &e = *r.error().server;
    std::println("conflict on {} ({}): {}", e.constraint, e.sqlstate, e.detail);
  }
}
```

Helpers include `sqlstate_class`, `is_unique_violation`, `is_foreign_key_violation`,
`is_not_null_violation`, `is_check_violation`, `is_integrity_constraint_violation`,
`is_undefined_table`, `is_undefined_column`, `is_serialization_failure`,
`is_deadlock_detected`, `is_connection_exception`, and more.

## Notices

```cpp
conn->on_notice([](const photon::server_error_t &notice) {
  std::println("[{}] {}", notice.severity, notice.message);
});
```

The callback runs synchronously on the event loop; it must not capture the
connection's own `shared_ptr` (that would form a reference cycle and leak the
connection) nor destroy the connection while it runs.

## LISTEN / NOTIFY

Subscribe with `listen` (the channel name is quoted as an identifier, so any name
is safe), then either register an async callback or block a dedicated connection on
`next_notification`:

```cpp
co_await conn->listen("jobs");

// async: notifications arriving during other queries are delivered here
conn->on_notification([](const photon::notification_t &n) {
  std::println("{} from pid {}: {}", n.channel, n.process_id, n.payload);
});

// or dedicate a connection to blocking for the next one
auto note = co_await conn->next_notification();   // result_t<notification_t>
if (note) std::println("{}: {}", note->channel, note->payload);
```

`NOTIFY` is a plain statement: `co_await conn->execute("NOTIFY jobs, 'ready'")`.
The async callback fires whenever a `query`/`execute` read loop encounters a
notification; `next_notification` is the single-purpose listener.

## Connection pool

A `connection_t` handles one query at a time, so to run queries concurrently on a
single event loop use a `pool_t` — a per-loop pool that grows lazily to `max_size`
and makes callers wait (async) when it is exhausted:

```cpp
photon::pool_t pool(loop, params, photon::pool_options_t{.max_size = 8});

// convenience: acquire → run → release, in one call
auto n = co_await pool.query_value<std::int64_t>("SELECT count(*) FROM users");

// or hold a lease across several statements (e.g. a transaction)
auto lease = co_await pool.acquire();
auto txn = co_await (*lease)->begin();
co_await (*lease)->execute("UPDATE accounts SET balance = balance - $1 WHERE id = $2", 10, 1);
co_await txn->commit();
// the connection returns to the pool when `lease` goes out of scope
```

A broken connection (`is_broken()`) is evicted and replaced on the next acquire.

## prism integration

Build with `-DPHOTON_WITH_PRISM=ON` and include `<photon/prism.h>` to back a
[prism](https://github.com/jorgen/prism) REST service with a per-worker pool. Each
prism worker thread gets its own `pool_t`; a handler takes `photon::prism::db` and
gets a `pool_t&`:

```cpp
#include <photon/prism.h>

vio::task_t<prism::response_t> count(photon::prism::db db)
{
  auto n = co_await db->query_value<std::int64_t>("SELECT count(*) FROM items");
  co_return prism::response_t::text(prism::status_t::ok, std::to_string(n->value_or(0)));
}

// during configure:
photon::prism::provide(app, params);   // registers one pool_t per worker
app.get("/count", count);
```

See [`examples/prism_service.cpp`](examples/prism_service.cpp). Both libraries fetch
vio/structify via cmake-dep; a downstream build must pin the **same vio commit** in
both (the `SKIP_IF_TARGET` guards then share a single vio target).

## Best practices

- **Never concatenate SQL.** Use `$n` (or `:name`) parameters — they are
  injection-safe and sent out-of-band. Bulk-load with `COPY`, not a loop of
  `INSERT`s.
- **One connection runs one statement at a time.** For concurrency on a single
  event loop, hand out connections from a `pool_t`; don't overlap calls on one
  `connection_t`.
- **Keep the owner alive across a borrow.** `transaction_t`, `pipeline_t`,
  `copy_in_t`/`copy_out_t` and a pool `lease` borrow the connection — hold them
  (and the connection/lease) until you `commit`/`run`/`finish`. Dropping one
  mid-operation poisons the connection so its half-done work is discarded.
- **Pick the right batching.** Independent pipelines cut round-trips for unrelated
  statements; use `atomic` mode (or wrap in `begin`/`commit`) when it must be
  all-or-nothing — and then **check `run()`'s result**, which reports a commit-time
  failure. Keep a pipeline bounded: the whole request and all results are buffered.
- **Prefer free-function coroutine handlers that take their dependencies by
  parameter** over capturing lambdas — a suspended coroutine lambda's captures are
  the classic dangling-`this` footgun. (This is why every handler here is a free
  function.)
- **Bound slow work.** Set `query_timeout`; expect a timed-out connection to be
  dropped (it fast-fails afterwards). Classify server errors with the `is_*`
  helpers and retry `is_serialization_failure` / `is_deadlock_detected`.
- **Materialise once.** `collect()` decodes a whole result set (reusing one scratch
  buffer); range-`for` decodes lazily per row — don't call `at(i)` in a loop if you
  need every row.
- On a compiler with the known `auto [a,b] = co_await …` structured-binding leak,
  bind the pipeline tuple to a named variable first: `auto r = co_await …;
  auto &[a, b] = r;`.

## Build

```cpp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Dependencies (**vio**, **structify**, **doctest**) are fetched and pinned
(URL + SHA256) at configure time via
[cmake-dep](https://github.com/jorgen/cmake-dep); no manual setup. Sanitizer presets
`asan`/`tsan`/`ubsan`/`msan` mirror the rest of the stack; CI runs the suite under
ASan, TSan and UBSan.

The unit tests are self-contained. The integration tests run against a real Postgres
and are gated by `PHOTON_PG_TEST_DSN` (they skip when it is unset):

```bash
docker run --rm -e POSTGRES_USER=photon -e POSTGRES_PASSWORD=photon \
  -e POSTGRES_DB=photon -e POSTGRES_INITDB_ARGS=--auth-host=scram-sha-256 \
  -p 55432:5432 postgres:16
PHOTON_PG_TEST_DSN=postgresql://photon:photon@127.0.0.1:55432/photon \
  ctest --test-dir build --output-on-failure
```

Set `PHOTON_PG_SSLROOTCERT` (to the server's CA/cert) to additionally run the TLS
integration tests.

## Status

Working: connection, SCRAM/cleartext auth, TLS (`sslmode`), typed `query`/`query_one`/
`query_value`/`execute`, `$n` + array + `:name` parameters, prepared statements,
structured errors, notices, a per-loop connection pool + prism integration,
transactions, request pipelining, `COPY` in/out, `CancelRequest`, query timeouts,
`LISTEN`/`NOTIFY`, and codecs for `uuid`/`timestamp(tz)`/`date`/`bytea`/`json(b)`/
`numeric`. Future work: per-stream HTTP/2-style timeouts and connection-level
retry helpers. See [`CLAUDE.md`](CLAUDE.md) for the architecture and roadmap.

## License

MIT — see [LICENSE](LICENSE).
