include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For older CMake or if URL fails, we might need alternatives, but this is standard.
# However, to be safe and avoid network, I might check if GTest is installed.
# But FetchContent is standard for modern CMake.
# I'll stick to FetchContent.

FetchContent_MakeAvailable(googletest)

add_executable(UnitTests TestOrderBook.cpp)
target_link_libraries(UnitTests PRIVATE OrderMatcher gtest_main)

include(GoogleTest)
gtest_discover_tests(UnitTests)
