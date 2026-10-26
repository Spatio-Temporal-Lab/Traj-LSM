cmake_minimum_required(VERSION 3.20)
project(STLSMServer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
get_filename_component(STLSM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(PORTABLE ON CACHE BOOL "Portable CPU instructions")
set(FAIL_ON_WARNINGS OFF CACHE BOOL "Treat upstream warnings as errors")
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "Build shared library")
foreach(feature GFLAGS SNAPPY LZ4 ZSTD ZLIB BZ2 LIBURING BENCHMARK_TOOLS CORE_TOOLS TOOLS TRACE_TOOLS)
  set(WITH_${feature} OFF CACHE BOOL "Optional RocksDB feature")
endforeach()
add_subdirectory("${STLSM_ROOT}/rocksdb" "${CMAKE_BINARY_DIR}/rocksdb")
get_directory_property(STLSM_ROCKSDB_DEFINITIONS
  DIRECTORY "${STLSM_ROOT}/rocksdb" COMPILE_DEFINITIONS)
add_compile_definitions(${STLSM_ROCKSDB_DEFINITIONS})

set(STLSM_DEMO "${STLSM_ROOT}/rocks-demo")
foreach(program
    st_bucket_ingest_build st_compaction_throughput_bench
    st_meta_compact_existing st_level_stats st_table_props st_prune_stats
    st_mixed_workload_bench traj_db_audit st_design_regression)
  add_executable(${program} "${STLSM_DEMO}/${program}.cpp")
  target_include_directories(${program} PRIVATE "${STLSM_ROOT}/rocksdb" "${STLSM_DEMO}")
  target_link_libraries(${program} PRIVATE rocksdb)
endforeach()

add_executable(st_meta_read_bench
  "${STLSM_DEMO}/st_meta_read_bench.cpp"
  "${STLSM_DEMO}/st_meta_read_bench_batch.cpp"
  "${STLSM_DEMO}/db/manifest_ops.cpp")
target_include_directories(st_meta_read_bench PRIVATE "${STLSM_ROOT}/rocksdb")
target_compile_definitions(st_meta_read_bench PRIVATE
  ROCKS_DEMO_SKIP_ROCKSDB_MANIFEST_STATS_IMPL=1
  ROCKS_DEMO_SKIP_ROCKSDB_MANIFEST_LEVEL_STATS_IMPL=1)
target_link_libraries(st_meta_read_bench PRIVATE rocksdb)

add_custom_target(stlsm_tools DEPENDS
  st_bucket_ingest_build st_compaction_throughput_bench st_meta_read_bench
  st_meta_compact_existing st_level_stats st_table_props st_prune_stats
  st_mixed_workload_bench traj_db_audit st_design_regression)
