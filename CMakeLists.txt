add_library(FD2D STATIC)

target_sources(FD2D PRIVATE
    Application.cpp
    Backplate.cpp
    Button.cpp
    CheckBox.cpp
    ComboBox.cpp
    Core.cpp
    DockPanel.cpp
    FD2DLog.cpp
    GridPanel.cpp
    Image.cpp
    OverlayPanel.cpp
    Panel.cpp
    ScrollView.cpp
    Slider.cpp
    Spinner.cpp
    SplitPanel.cpp
    Splitter.cpp
    StackPanel.cpp
    Text.cpp
    Util.cpp
    Wnd.cpp
)

target_include_directories(FD2D
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        # Required by Backplate.cpp/Application.cpp/etc. which include
        # ../CommonUtil.h (a small header-only utility, not a build dependency -
        # any consumer must vendor a copy one level above FD2D; see nifskope's
        # liteviewer/third_party/CommonUtil.h for an example). Logging no longer
        # needs this: see FD2DLog.h, which replaced the old "../AppLog.h" +
        # "../external/spdlog/include" dependency with a self-contained,
        # dependency-free implementation.
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_definitions(FD2D PRIVATE
    FD2D_STATIC
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(FD2D PRIVATE /permissive- /utf-8)
endif()
