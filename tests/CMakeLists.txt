include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)

# Don't let GoogleTest's flags/warnings leak into our build.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(memalloc_tests
    smoke_test.cpp
    align_test.cpp
    arena_test.cpp
    slab_test.cpp
    tlsf_test.cpp
    tlsf_alloc_test.cpp)

target_link_libraries(memalloc_tests PRIVATE
    memalloc
    memalloc_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(memalloc_tests)
