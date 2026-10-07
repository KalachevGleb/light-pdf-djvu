# PDFium: prebuilt Windows x64 binaries from github.com/bblanchon/pdfium-binaries (no V8, no XFA).
# Provides the imported target `pdfium` (pdfium.dll + import library + headers).

include(FetchContent)

set(LV_PDFIUM_VERSION "8086")
FetchContent_Declare(pdfium_bin
  URL https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${LV_PDFIUM_VERSION}/pdfium-win-x64.tgz
  URL_HASH SHA256=1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(pdfium_bin)

find_package(PDFium REQUIRED CONFIG PATHS "${pdfium_bin_SOURCE_DIR}" NO_DEFAULT_PATH)
