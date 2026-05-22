add_executable(merlion_tsdb_tests
    smoke_test.cpp
    encoding/varint_test.cpp
    encoding/bstream_test.cpp
)
target_link_libraries(merlion_tsdb_tests PRIVATE
    merlion_tsdb_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(merlion_tsdb_tests)
