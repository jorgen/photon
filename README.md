# photon

An async **PostgreSQL** client for **C++23**, built on
[vio](https://github.com/jorgen/vio) (async I/O: libuv + coroutines + TLS) and
[structify](https://github.com/jorgen/structify) (compile-time struct reflection).

photon speaks the PostgreSQL wire protocol directly over vio's sockets and
coroutines — no libpq — and binds result columns straight into a struct you
define. The light theme continues from vio and prism: a **photon** is the quantum
of light, one typed row dispersed from a query.

```cpp
struct user_t { int id; std::string name; STFY_OBJ(id, name); };

photon::connection_t conn(loop, params);
if (auto ok = co_await conn.connect(); !ok) { /* ok.error() */ }

auto users = co_await conn.query<user_t>(
    "SELECT id, name FROM users WHERE age > $1", 18);
for (const user_t &u : *users)
{
    // u.id, u.name  — or structured bindings: auto [id, name] = u;
}
```

- **Typed rows** — define a `Row` struct with `STFY_OBJ(...)`; photon decodes each
  result column into the matching field. `Row` is a plain aggregate, so C++
  structured bindings work.
- **Binary, extended protocol** — `Parse`/`Bind`/`Execute` with `$n` parameters
  (injection-safe) and binary result values decoded per type.
- **Async on vio** — every call is a `vio::task_t`, so it `co_await`s without
  blocking the event loop; timeouts and cancellation via `vio::cancellation_t`.
- **Lean & consistent** — errors flow through `result_t<T>`
  (`std::expected<T, error_t>`), mirroring vio and prism; built
  `-fno-exceptions -fno-rtti`.

Status: **early development.** See [`CLAUDE.md`](CLAUDE.md) for the architecture
and phased roadmap. The connection + typed-query API lands in the vertical-slice
milestone.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Dependencies (**vio**, **structify**, **doctest**) are fetched and pinned
(URL + SHA256) at configure time via
[cmake-dep](https://github.com/jorgen/cmake-dep); no manual setup.

Integration tests run against a real Postgres and are gated by the
`PHOTON_PG_TEST_DSN` environment variable (they skip when it is unset):

```bash
docker run --rm -e POSTGRES_USER=photon -e POSTGRES_PASSWORD=photon \
  -e POSTGRES_DB=photon -p 5432:5432 postgres:16
PHOTON_PG_TEST_DSN=postgresql://photon:photon@127.0.0.1:5432/photon \
  ctest --test-dir build --output-on-failure
```

## License

MIT — see [LICENSE](LICENSE).
