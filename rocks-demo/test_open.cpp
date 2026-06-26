#include <cstdio>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

int main() {
  printf("test: starting...\n"); fflush(stdout);
  rocksdb::Options o;
  o.create_if_missing = true;
  printf("test: opening db...\n"); fflush(stdout);
  rocksdb::DB* db = nullptr;
  auto s = rocksdb::DB::Open(o, "D:/tmp/test_open_db", &db);
  printf("test: status=%s\n", s.ToString().c_str()); fflush(stdout);
  if (db) { delete db; db = nullptr; }
  printf("test: done\n"); fflush(stdout);
  return s.ok() ? 0 : 1;
}
