# FD2D unit tests (enabled with -DFD2D_BUILD_TESTS=ON when configuring the
# FICture2 tree; FD2D has no standalone CMake project). Non-GUI tests only.

add_executable(FD2DTests
    TestHarness.h
    TestMain.cpp
    BackplateTestAccess.h
    AsyncRedrawDispatchTests.cpp
    AsyncRedrawSignalTests.cpp
    ImageTransformTests.cpp
    ImageCheckerboardRenderTests.cpp
    BackplateHoverToastTests.cpp
    ComboBoxTests.cpp
    ScrollViewTests.cpp
    GraphicsGenerationTests.cpp
    RenderStatsTests.cpp
    WindowlessTestSupport.h
    WindowlessRenderTests.cpp
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
    d2d1
    dwrite
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    ole32
    shell32
)

if(MSVC)
    target_compile_options(FD2DTests PRIVATE /permissive- /utf-8)
endif()

# Manual two-backplate host (needs a desktop; not registered with CTest).
add_executable(FD2DAsyncRedrawHost AsyncRedrawHost.cpp)
target_compile_definitions(FD2DAsyncRedrawHost PRIVATE FD2D_STATIC UNICODE _UNICODE)
target_link_libraries(FD2DAsyncRedrawHost PRIVATE
    FD2D d2d1 dwrite d3d11 dxgi d3dcompiler windowscodecs ole32 shell32
)
if(MSVC)
    target_compile_options(FD2DAsyncRedrawHost PRIVATE /permissive- /utf-8)
endif()

add_test(NAME FD2D.AsyncRedrawSignal COMMAND FD2DTests AsyncRedrawSignal_)
add_test(NAME FD2D.AsyncRedrawDispatch COMMAND FD2DTests AsyncRedrawDispatch_)
add_test(NAME FD2D.ImageTransform COMMAND FD2DTests ImageTransform_)
add_test(NAME FD2D.GraphicsGeneration COMMAND FD2DTests GraphicsGeneration_)
add_test(NAME FD2D.ScrollView COMMAND FD2DTests ScrollViewShift_)
add_test(NAME FD2D.WindowlessRender COMMAND FD2DTests WindowlessRender_)
add_test(NAME FD2D.ImageCheckerboard COMMAND FD2DTests ImageCheckerboard_)
add_test(NAME FD2D.BackplateHoverToast COMMAND FD2DTests BackplateHoverToast_)
add_test(NAME FD2D.ComboBox COMMAND FD2DTests ComboBox_)
add_test(NAME FD2D.RenderStats COMMAND FD2DTests RenderStats_)
# Safety net only: every wait inside the tests is bounded.
set_tests_properties(FD2D.AsyncRedrawSignal FD2D.AsyncRedrawDispatch FD2D.ImageTransform FD2D.GraphicsGeneration FD2D.ScrollView FD2D.WindowlessRender FD2D.ImageCheckerboard FD2D.BackplateHoverToast FD2D.ComboBox FD2D.RenderStats PROPERTIES TIMEOUT 120)
