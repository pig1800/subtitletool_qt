cmake_minimum_required(VERSION 3.20)
project(subtitletool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia)
find_package(xlnt CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavformat libavcodec libswresample libavutil)


add_executable(subtitletool WIN32
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/models/SubtitleRow.h
    src/models/SubtitleRow.cpp
    src/models/SubtitleModel.h
    src/models/SubtitleModel.cpp
    src/models/FileTab.h
    src/models/FileTab.cpp
    src/models/AudioData.h
    src/models/AudioData.cpp
    src/helpers/CharCountHelper.h
    src/helpers/ExcelHelper.h
    src/helpers/ExcelHelper.cpp
    src/helpers/LexicographicComparer.h
    src/dialogs/FindReplaceDialog.h
    src/dialogs/FindReplaceDialog.cpp
    src/dialogs/ShiftTimeDialog.h
    src/dialogs/ShiftTimeDialog.cpp
    src/dialogs/ProjectSettingsDialog.h
    src/dialogs/ProjectSettingsDialog.cpp
    src/dialogs/RuleCheckDialog.h
    src/dialogs/RuleCheckDialog.cpp
    src/widgets/WaveformWidget.h
    src/widgets/WaveformWidget.cpp
)

target_include_directories(subtitletool PRIVATE src)

# Enable AVX for SIMD audio processing
if(MSVC)
    target_compile_options(subtitletool PRIVATE /arch:AVX2)
else()
    target_compile_options(subtitletool PRIVATE -mavx2 -mfma)
endif()

target_link_libraries(subtitletool PRIVATE
    Qt6::Widgets
    Qt6::Multimedia
    xlnt::xlnt
    PkgConfig::LIBAV
)
