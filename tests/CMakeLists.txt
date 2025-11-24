# Unit Tests

find_package(GTest CONFIG REQUIRED)

add_executable(EntityMediaTests
    unit/FrameRingBufferTests.cpp
    unit/BufferSystemTests.cpp
    test_Decoder.cpp
)

target_link_libraries(EntityMediaTests
    PRIVATE
        EntityMediaCore
        GTest::gtest
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(EntityMediaTests)
