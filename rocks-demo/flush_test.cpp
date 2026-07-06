#include <rocksdb/db.h>
#include <iostream>
int main() {
    rocksdb::DB* db;
    rocksdb::Options o;
    o.create_if_missing = true;
    rocksdb::BlockBasedTableOptions bt;
    bt.experimental_spatio_temporal_meta_in_index_value = false;
    o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bt));
    o.write_buffer_size = 64ULL << 20;
    rocksdb::Status s = rocksdb::DB::Open(o, "D:/tmp/flush_test", &db);
    std::cout << "Open: " << s.ToString() << std::endl;
    if (!s.ok()) return 1;
    
    for (int i = 0; i < 100; i++) {
        db->Put(rocksdb::WriteOptions(), "key" + std::to_string(i), "value" + std::to_string(i));
    }
    std::cout << "Writes done" << std::endl;
    
    rocksdb::FlushOptions fo;
    fo.wait = true;
    s = db->Flush(fo);
    std::cout << "Flush: " << s.ToString() << std::endl;
    
    delete db;
    rocksdb::DestroyDB("D:/tmp/flush_test", rocksdb::Options());
    std::cout << "All done" << std::endl;
    return 0;
}
