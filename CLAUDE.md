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
- `params.h` — `param_codec_t<T>` customization point (text Bind params).
- `decode.h` — `value_codec_t<T>` customization point (binary wire value → T).
- `types.h` — codecs for richer Postgres types: `uuid_t`, `bytea_t`, `json_t`
  (json/jsonb; strips the leading `0x01` jsonb version byte), `numeric_t` (binary
  numeric → decimal string), plus `std::chrono::system_clock::time_point`
  (timestamp/timestamptz, µs since the 2000 epoch) and `std::chrono::sys_days`
  (date). Binary decoders + text-param encoders; `std::optional<T>` NULL handling
  is free via `decode_field`.
- `transaction.h` / `transaction.cpp` — `transaction_t` (move-only borrow of a
  `connection_t*`): `co_await conn->begin()`, then `co_await txn.commit()` /
  `rollback()`. Dropping an active transaction **poisons** the connection
  (`_broken`, so the pool evicts it and close aborts the open server transaction) —
  the safety net for the "a destructor can't `co_await`" problem.
- `pipeline.h` / `pipeline.cpp` — request **pipelining** (many query cycles in one
  round-trip). `conn->pipeline()` → a `pipeline_t` builder; `pipe.query<Row>` /
  `execute` queue a step and return a typed `pipe_slot_t<T>` read after
  `co_await pipe.run()`. Also a variadic one-shot `conn->pipeline(pquery<Row>(…),
  pexecute(…), …)` → a typed `std::tuple`. Mode flag: `independent` (Sync per step)
  or `atomic` (one trailing Sync = an implicit transaction). `run()` starts the
  batch write as an **eager `task_t`**, reads all responses concurrently, then
  joins the write — deadlock-free on TCP and TLS (vio has no `when_all`; this is
  the eager-task idiom). The per-query read loop is factored into
  `connection_t::read_query_result()` (shared by `exec_extended` and independent
  pipelines); `detail::append_extended_query` frames one cycle
  (`query_data_t` also moved to `detail/message.h`).
- `copy.h` / `copy.cpp` — bulk **COPY**. `conn->copy_in(sql)` → `copy_in_t`
  (`write`/`write_row` text-formatting with escaping + `\N`, `finish`, `fail`);
  `conn->copy_out(sql)` → `copy_out_t` (`read_chunk`/`read_all`). Simple-Query
  based; both borrow the connection and poison it if dropped mid-stream. A
  statement that does not enter COPY mode is drained to ReadyForQuery and returns a
  protocol error (never hangs).
- `named.h` / `named.cpp` — **named parameters**: `named_args_t` +
  `rewrite_named_params`, a SQL-aware `:name` → `$n` rewriter (skips `::` casts,
  `'…'`/`E'…'`(with backslash escapes)/`$tag$…$tag$` literals, `"idents"`, and `--`
  / `/* */` comments). `query`/`execute`/`query_one`/`query_value` gain
  `named_args_t` overloads (positional variadics SFINAE-guarded off a lone
  `named_args_t` via `is_named_args_call_v`).
- **Cancellation & timeouts** (on `connection.*`): `conn->cancel_handle()` →
  `cancel_handle_t` (captures backend pid/secret + params; `cancel(loop)` opens a
  fresh transport via the factored `establish_transport` and sends a CancelRequest).
  `connect_params_t::query_timeout` wraps `read_query_result` in a vio-cancellation
  watchdog (single-threaded loop ⇒ it can only fire while suspended in a read);
  on expiry the read is cancelled, the call returns `error_kind_t::timeout`, and the
  connection is poisoned. Every read/write entry point fast-fails when `_broken`.
- `row_binding.h` — `std::index_sequence` loop over structify metadata mapping
  result columns to struct fields; `result.h` `make_result_set<Row>` centralises
  `query_data_t` → `result_set_t<Row>`.
- `detail/message.h` / `.cpp` — frontend serializers + backend parsers (incl.
  `parse_notification`, `cancel_request_message`, COPY messages, and
  `parse_data_row_into` for scratch-reusing bulk decode).
- `detail/frame_reader.h` — length-prefixed frame reassembly. Uses a `_consumed`
  read cursor (amortised O(1)/frame, no per-frame front-erase) and reads straight
  into the buffer tail; `wire_writer_t` frames in-place (one alloc/message).
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
3. **TLS / sslmode** (done): added `ssl_client_upgrade` to vio (adopt a connected
   socket, TLS-handshake as a client), pushed + re-pinned. photon: SSLRequest
   negotiation, `tls_transport_t`, sslmode disable/allow/prefer/require/verify-ca/
   verify-full (peer_verify + CA from `sslrootcert`; SNI/hostname pinning per mode).
   Integration-tested against a real Postgres over TLS 1.3 (require encrypts;
   verify-full accepts the CA and rejects an untrusted server); ASan/TSan/UBSan
   clean; adversarial-review fixes (client-only DSN keywords no longer forwarded
   as GUCs; SNI sent for prefer/require/verify-full).
4. **API revisit** (done): rich structured server errors (`server_error_t` with all
   `ErrorResponse` fields) + sqlstate classification helpers (`is_unique_violation`,
   `sqlstate_class`, …); result ergonomics (range-for over `result_set_t`,
   `query_one<Row>`, `query_value<T>`); `on_notice` callback surfacing
   `NoticeResponse`; `connect()` returns `shared_ptr<connection_t>` (+ a `dsn`
   overload, `from_dsn`, `is_broken()`); array params (`param_codec_t<vector<T>>`
   → `ANY($1)`); server-side prepared statements (`prepare` →
   `prepared_statement_t`). Offline + live + ASan/TSan/UBSan clean; reviewed.
5. **Pooling + prism integration** (done): `pool_t` — a per-loop pool of
   `shared_ptr<connection_t>` (lazy to `max_size`, lock-free FIFO async wait when
   exhausted, RAII `lease_t`, `is_broken()` eviction), so concurrent handlers on
   one loop multiplex across connections. prism gained a **loop-aware
   `provide_per_thread`** factory (one-file change in `detail/thread_state.h`;
   vio pin realigned to `00ace20` + `SKIP_IF_TARGET vio/structify` so photon+prism
   share one vio target). `photon/prism.h` (behind `PHOTON_WITH_PRISM`):
   `photon::prism::provide(app, params)` registers a per-worker `pool_t`; handlers
   take `photon::prism::db` (`per_thread<pool_t>`) and `co_await db->query<Row>(…)`.
   Live pool concurrency/reuse + an in-process prism-integration test; ASan/TSan/
   UBSan clean on default and `PHOTON_WITH_PRISM=ON`; reviewed. Pool convenience
   methods take params **by value** (copied into the coroutine frame) since they
   `co_await acquire()` before using them.
6. **Breadth** (6a done): transactions (`begin`/`commit`/`rollback`, poison-on-drop),
   more type codecs (`uuid`, `timestamp`/`timestamptz`, `date`, `bytea`, `json`/`jsonb`,
   `numeric`), and LISTEN/NOTIFY (`on_notification` from the query read loops,
   `listen(channel)` with ident quoting, and a dedicated-listener `next_notification()`).
   Offline byte-vector codec tests + live round-trips/transaction/notify tests;
   ASan/TSan/UBSan clean on default and `PHOTON_WITH_PRISM=ON`; reviewed.
   **6b done**: request pipelining (builder + typed slots, variadic one-shot tuple,
   independent/atomic modes, eager-write/concurrent-read to avoid the large-transfer
   deadlock).
   **6c done**: COPY in/out, named parameters, CancelRequest, and query timeout
   (details in the architecture bullets above). Plus a memory-flow pass
   (frame-reader O(n²)→O(n) `_consumed` cursor, in-place `wire_writer_t` framing,
   scratch-reusing `collect()`) and an adversarial bug hunt (fixed: COPY/timeout
   hangs on misuse, E-string named-param parsing) backed by stress tests (100k-row
   streaming, 4 MB values, 50k-row COPY, 200 concurrent pooled queries). ASan/TSan/
   UBSan clean on default and `PHOTON_WITH_PRISM=ON`.
   Remaining breadth (future): true per-statement HTTP/2-style timeouts, pipelined
   prepared statements, a `pool_t::pipeline()` convenience, connection-level retry.

### Consuming photon + prism together

Both fetch vio/structify via cmake-dep; the `SKIP_IF_TARGET` guards make the
first project to add a shared target win. A downstream app must pin the **same
vio commit** in both (only one `vio` target can exist). prism has no install/export
config yet, so `PHOTON_USE_SYSTEM_PRISM=ON` (find_package) is unsupported — the
add_subdirectory (fetch) path is the way in.

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
