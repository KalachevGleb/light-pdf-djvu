cmake_minimum_required(VERSION 3.24)
project(LightView VERSION 0.1.0 LANGUAGES C CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "LightView is a Windows application built with MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the exe does not need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/pdfium.cmake)
include(cmake/djvulibre.cmake)

add_executable(lightview WIN32
  src/main.cpp
  src/main_window.cpp
  src/canvas.cpp
  src/toolbar.cpp
  src/renderer.cpp
  src/document.cpp
  src/pdf_document.cpp
  src/djvu_document.cpp
  src/print.cpp
  src/settings.cpp
  src/theme.cpp
  src/util.cpp
  src/lightview.rc
  src/lightview.manifest
)
target_compile_definitions(lightview PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(lightview PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus)
target_link_libraries(lightview PRIVATE
  pdfium djvulibre comctl32 comdlg32 shell32 ole32 uxtheme dwmapi delayimp)
# pdfium.dll is only loaded when the first PDF is opened.
target_link_options(lightview PRIVATE /DELAYLOAD:pdfium.dll)

add_custom_command(TARGET lightview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:pdfium> $<TARGET_FILE_DIR:lightview>)

# Engine smoke test, built on demand: cmake --build --preset release --target lightview_smoke
add_executable(lightview_smoke EXCLUDE_FROM_ALL
  tests/smoke.cpp src/document.cpp src/pdf_document.cpp src/djvu_document.cpp src/print.cpp src/util.cpp)
target_include_directories(lightview_smoke PRIVATE src)
target_compile_definitions(lightview_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(lightview_smoke PRIVATE /utf-8 /W4 /permissive-)
target_link_libraries(lightview_smoke PRIVATE pdfium djvulibre comdlg32)
add_custom_command(TARGET lightview_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:pdfium> $<TARGET_FILE_DIR:lightview_smoke>)

# `cmake --install build/Release --prefix dist` produces a portable folder.
install(TARGETS lightview RUNTIME DESTINATION .)
install(FILES $<TARGET_FILE:pdfium> DESTINATION .)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES README.md DESTINATION . RENAME README.txt)
install(FILES "${pdfium_bin_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME PDFium-LICENSE.txt)
install(DIRECTORY "${pdfium_bin_SOURCE_DIR}/licenses/" DESTINATION licenses/pdfium-third-party)
install(FILES "${djvulibre_SOURCE_DIR}/COPYING" DESTINATION licenses RENAME DjVuLibre-COPYING.txt)
