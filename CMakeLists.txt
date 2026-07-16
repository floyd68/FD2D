add_library(FD2D STATIC)

target_sources(FD2D PRIVATE
    Application.cpp
    Backplate.cpp
    Button.cpp
    CheckBox.cpp
    ComboBox.cpp
    Core.cpp
    DockPanel.cpp
    DynamicPanel.cpp
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
)

target_compile_definitions(FD2D PRIVATE
    FD2D_STATIC
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(FD2D PRIVATE /permissive- /utf-8)
endif()
