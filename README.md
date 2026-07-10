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

### Prepared statements

```cpp
auto stmt = co_await conn->prepare("SELECT id, name, age FROM users WHERE id = $1");
for (int id : ids)
{
  auto row = co_await stmt->query_one<user_t>(id);   // parsed once, bound many times
}
```

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
`query_value`/`execute`, `$n` + array parameters, prepared statements, structured
errors, and notices. Future work: connection pooling + a prism integration header,
transactions, `COPY`, `LISTEN`/`NOTIFY`, pipelining, and more type codecs (numeric,
uuid, timestamp, json, …). See [`CLAUDE.md`](CLAUDE.md) for the architecture and
roadmap.

## License

MIT — see [LICENSE](LICENSE).
