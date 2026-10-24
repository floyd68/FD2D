# FD2D unit tests (enabled with -DFD2D_BUILD_TESTS=ON when configuring the
# FICture2 tree; FD2D has no standalone CMake project). Non-GUI tests only.

add_executable(FD2DTests
    TestHarness.h
    TestMain.cpp
    AsyncRedrawSignalTests.cpp
)

target_include_directories(FD2DTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(FD2DTests PRIVATE
    FD2D_STATIC
    UNICODE
    _UNICODE
)

target_link_libraries(FD2DTests PRIVATE
    FD2D
)

if(MSVC)
    target_compile_options(FD2DTests PRIVATE /permissive- /utf-8)
endif()

add_test(NAME FD2D.AsyncRedrawSignal COMMAND FD2DTests AsyncRedrawSignal_)
# Safety net only: every wait inside the tests is bounded.
set_tests_properties(FD2D.AsyncRedrawSignal PROPERTIES TIMEOUT 120)
