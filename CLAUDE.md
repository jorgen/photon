# photon

An async **PostgreSQL** client library for C++23, built on **vio** (async I/O:
libuv + coroutines + LibreSSL TLS) and **structify** (header-only compile-time
struct reflection). It speaks the Postgres wire protocol directly over vio's
sockets and coroutines — no libpq. Name theme: libuv = ultraviolet, vio = violet
io, prism disperses a request stream into routes, and **photon** is the quantum of
light — one typed row dispersed from a query.

## Build

```bash
cmake --preset debug
cmake --build cmake-build-debug
ctest --preset debug                       # or ./cmake-build-debug/tests/photon_tests
```

The default preset pins ninja to a homebrew path (macOS). On Linux configure
manually:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

Sanitizer presets: `asan`, `tsan`, `ubsan`, `msan` (see `CMakePresets.json`).

## Dependencies

Fetched at configure time via [cmake-dep](https://github.com/jorgen/cmake-dep),
pinned in `CMake/3rdPartyPackages.cmake` (`CmDepFetchPackage(name version url
SHA256=…)`) and added in `CMake/Build3rdParty.cmake` (`CmDepAddPackage`):

- **vio** — built static; its `src/` is re-exposed as a PUBLIC include (vio marks
  headers PRIVATE upstream). We own vio (`github.com/jorgen/vio`) and extend it
  when the client needs a primitive it lacks (e.g. the plaintext→TLS socket
  upgrade for `sslmode`). Linked `vio::vio`.
- **structify** — INTERFACE target `structify::structify`; drives row→struct
  binding via its compile-time field reflection.
- **doctest** — testing.

Per-dependency toggles/overrides are auto-declared by cmake-dep with the `PHOTON_`
prefix (from `PROJECT_NAME`): `PHOTON_USE_SYSTEM_<DEP>` and
`PHOTON_<DEP>_{VERSION,URL,SHA256}`. To bump a dep: change the git ref in the URL
**and** recompute the SHA256 (`curl -sL <url> | shasum -a 256`).

## Conventions

- **C++23**, built `-fno-exceptions -fno-rtti` (MSVC `/EHs-c- /GR-
  -D_HAS_EXCEPTIONS=0`). `Build3rdParty.cmake` strips those around the third-party
  `add_subdirectory`s that need exceptions, then restores them.
- vio's `task_t` coroutine calls `std::terminate()` on unhandled exception (no
  throw), so it is compatible with `-fno-exceptions`.
- Naming (enforced by `.clang-tidy`): types/enums/aliases are `lower_case` with a
  `_t` suffix; functions/variables are `lower_case`; private members `_`-prefixed.
- Errors use `photon::result_t<T>` = `std::expected<T, photon::error_t>`, mirroring
  vio's `std::expected<T, vio::error_t>`. `error_t` carries an `error_kind_t`, an
  optional 5-char `sqlstate` (server errors), and a message.
- **No comments in code.** Let clear naming speak — library, tests, and examples.

## Git

- **No Claude/Anthropic attribution** in commits, PR bodies, or generated content.

## Architecture (target design)

Everything above the socket is transport-agnostic and templated on a `transport_t`
(plain TCP now, TLS later), the same pattern as prism's
`serve_connection_impl<Transport>`.

- `error.h` — `error_kind_t`, `error_t`, `result_t<T>`, `fail()`/`fail_server()`.
- `photon.h` — umbrella header + `version()`.
- `connect_params_t.h` — host/port/database/user/password/sslmode/timeouts.
- `connection.h` / `.cpp` — `connection_t` bound to `(vio::event_loop_t&,
  connect_params_t)`: `co_await connect()` (DNS → TCP → optional TLS upgrade →
  startup → auth → ready), `query<Row>(sql, params...)`, `execute(sql,
  params...)`, transactions, close.
- `result.h` — `result_set_t<Row>` (owns the DataRow buffers + a field→column map;
  `at(i)`/iteration materialise a typed `Row`), `command_result_t` (tag +
  rows_affected).
- `params.h` — `param_codec_t<T>` customization point (binary Bind params).
- `decode.h` — `value_codec_t<T>` customization point (binary wire value → T).
- `row_binding.h` — `std::index_sequence` loop over structify metadata mapping
  result columns to struct fields.
- `detail/message.h` / `.cpp` — frontend serializers + backend parsers.
- `detail/frame_reader.h` — length-prefixed frame reassembly, timeout-bounded.
- `detail/transport.h` — `tcp_transport_t` (+ `tls_transport_t` later).
- `detail/scram.h` / `.cpp` — SCRAM-SHA-256 (+ md5/cleartext) auth.
- `pool.h` / `.cpp` — per-loop connection pool (later phase).
- `prism.h` — optional prism integration (behind `PHOTON_WITH_PRISM`): registers a
  per-worker pool through prism's `provide_per_thread<T>` and exposes it to
  handlers as `per_thread<pool_t>`.

### Typed query flow

`co_await conn.query<user_t>("SELECT id, name FROM users WHERE age > $1", 18)`
sends Parse/Bind/Execute/Sync (binary result format, `$1` bound via
`param_codec_t<int>`), builds a column→field map from the `RowDescription` once,
and returns a `result_set_t<user_t>` that decodes each column into the matching
struct field via `value_codec_t<field_type>` (NULL → empty `std::optional`; NULL
into a non-optional → decode error). `Row` is a plain aggregate with `STFY_OBJ`,
so C++ structured bindings work: `auto [id, name] = set.at(0);`.

### structify reflection primitives used

`STFY::Internal::StructifyBaseDummy<Row,Row>::stfy_static_meta_data_info()` returns
a `Tuple` with `::size` and `.get<I>()` → `MI` exposing `.names.get<0>().data`
(field name), `::type` (field type), and `.member` (pointer-to-member). structify's
`TypeHandler` is JSON-tokenizer-bound, so photon has its own wire codec and NULL
policy; only the reflection is reused.

## Delivery phases

0. **Scaffold** (done): repo, CMake/cmake-dep, presets, CI, `error.h`, `version()`.
1. **Wire codec** (done): messages, framing, param/value binary codecs, SCRAM
   (`vio::crypto` extended + re-pinned).
2. **Connection + typed SELECT** (done): plaintext `connection_t::connect` (DNS →
   TCP → startup → SCRAM/cleartext auth → ready), extended-protocol
   `query<Row>`/`execute` with text params + binary results, `result_set_t<Row>`
   bound via structify. Integration-tested against a real Postgres over SCRAM;
   ASan/TSan/UBSan clean; adversarial-review findings fixed. The vertical slice.
3. **TLS / sslmode**: add `ssl_client_upgrade` to vio, then the TLS transport.
4. **Pooling + prism integration**: `pool_t` + a loop-aware `provide_per_thread`
   overload in prism + `photon/prism.h`.
5. **Breadth**: prepared statements, transactions, more type codecs (numeric, uuid,
   timestamp, json, arrays), COPY, LISTEN/NOTIFY, pipelining, CancelRequest.

## Testing

- **Unit (offline)**: doctest (`DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`)
  — message round-trips, SCRAM RFC vectors, codec encode/decode.
- **Integration**: against a real Postgres, gated by the `PHOTON_PG_TEST_DSN`
  environment variable (skips cleanly when unset). CI runs a `postgres:16` service
  container. Locally: `docker run -e POSTGRES_PASSWORD=photon -e POSTGRES_USER=photon
  -e POSTGRES_DB=photon -p 5432:5432 postgres:16`, then
  `PHOTON_PG_TEST_DSN=postgresql://photon:photon@127.0.0.1:5432/photon ctest`.
- **Sanitizers**: ASan + TSan are the key gates.

## Layout

- `src/photon/` — the library (headers + `*.cpp`).
- `tests/` — doctest.
- `examples/` — added from Phase 4 (connection demo, prism integration).
