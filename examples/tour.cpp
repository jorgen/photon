#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <structify/structify.h>

#include <photon/photon.h>

#include <vio/run.h>

namespace
{
struct book_t
{
  std::int32_t id;
  std::string title;
  photon::numeric_t price;
  STFY_OBJ(id, title, price);
};

struct count_row_t
{
  std::int64_t n;
  STFY_OBJ(n);
};

const char *dsn_from(int argc, char **argv)
{
  if (argc > 1)
  {
    return argv[1];
  }
  const char *env = std::getenv("PHOTON_PG_TEST_DSN");
  return env != nullptr ? env : "postgresql://photon:photon@127.0.0.1:5432/photon";
}

vio::task_t<photon::result_t<void>> load_books(const std::shared_ptr<photon::connection_t> &conn)
{
  auto in = co_await conn->copy_in("COPY books(title, price) FROM STDIN");
  if (!in.has_value())
  {
    co_return std::unexpected(in.error());
  }
  const char *rows[] = {"The Go Programming Language\t34.99", "Effective Modern C++\t44.50", "Designing Data-Intensive Applications\t39.99"};
  for (const char *row : rows)
  {
    auto wrote = co_await in->write(std::string(row) + "\n");
    if (!wrote.has_value())
    {
      co_return std::unexpected(wrote.error());
    }
  }
  auto done = co_await in->finish();
  if (!done.has_value())
  {
    co_return std::unexpected(done.error());
  }
  std::printf("COPY loaded %lld books\n", static_cast<long long>(done->rows_affected));
  co_return photon::result_t<void>{};
}
} // namespace

VIO_MAIN(loop, argc, argv)
{
  auto connection = co_await photon::connection_t::connect(loop, dsn_from(argc, argv));
  if (!connection.has_value())
  {
    std::fprintf(stderr, "connect: %s\n", connection.error().msg.c_str());
    co_return 1;
  }
  auto &conn = *connection;

  (void)co_await conn->execute("DROP TABLE IF EXISTS books");
  auto created = co_await conn->execute("CREATE TABLE books(id serial PRIMARY KEY, title text NOT NULL, price numeric(8,2))");
  if (!created.has_value())
  {
    std::fprintf(stderr, "create: %s\n", created.error().msg.c_str());
    co_return 1;
  }

  auto loaded = co_await load_books(conn);
  if (!loaded.has_value())
  {
    std::fprintf(stderr, "load: %s\n", loaded.error().msg.c_str());
    co_return 1;
  }

  auto books = co_await conn->query<book_t>("SELECT id, title, price FROM books ORDER BY price DESC");
  if (!books.has_value())
  {
    std::fprintf(stderr, "query: %s\n", books.error().msg.c_str());
    co_return 1;
  }
  for (auto row : *books)
  {
    if (!row.has_value())
    {
      break;
    }
    std::printf("  #%d  %-40s $%s\n", row->id, row->title.c_str(), row->price.value.c_str());
  }

  photon::named_args_t args;
  args.set("ceiling", photon::numeric_t{"40.00"});
  auto affordable = co_await conn->query_value<std::int64_t>("SELECT count(*) FROM books WHERE price <= :ceiling", args);
  if (affordable.has_value() && affordable->has_value())
  {
    std::printf("books at or under $40: %lld\n", static_cast<long long>(**affordable));
  }

  auto txn = co_await conn->begin();
  if (txn.has_value())
  {
    (void)co_await conn->execute("UPDATE books SET price = price * 0.9 WHERE price > $1", photon::numeric_t{"40"});
    (void)co_await txn->commit();
    std::printf("applied a 10%% discount to the pricey books\n");
  }

  auto stats = co_await conn->pipeline(photon::pquery<count_row_t>("SELECT count(*) AS n FROM books"), photon::pquery<count_row_t>("SELECT count(*) AS n FROM books WHERE price < 40"));
  auto &[total, cheap] = stats;
  auto total_row = total.has_value() ? total->one() : photon::result_t<std::optional<count_row_t>>{std::nullopt};
  auto cheap_row = cheap.has_value() ? cheap->one() : photon::result_t<std::optional<count_row_t>>{std::nullopt};
  if (total_row.has_value() && total_row->has_value() && cheap_row.has_value() && cheap_row->has_value())
  {
    std::printf("pipeline (one round-trip): %lld books, %lld under $40\n", static_cast<long long>((*total_row)->n), static_cast<long long>((*cheap_row)->n));
  }

  auto missing = co_await conn->query<book_t>("SELECT * FROM nope");
  if (!missing.has_value() && photon::is_undefined_table(missing.error()))
  {
    std::printf("expected error: %s (%.*s)\n", missing.error().msg.c_str(), static_cast<int>(missing.error().sqlstate().size()), missing.error().sqlstate().data());
  }

  (void)co_await conn->execute("DROP TABLE books");
  co_await conn->close();
  co_return 0;
}
