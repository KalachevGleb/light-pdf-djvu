# DjVuLibre (libdjvu only) built as a static library from a pinned upstream snapshot.
# Upstream has no CMake build; the source list and defines mirror win32/djvulibre/libdjvulibre.vcxproj.

include(FetchContent)

FetchContent_Declare(djvulibre
  URL https://github.com/barak/djvulibre/archive/03e6abd7295c74f795ed70a7118294b25351e0f4.tar.gz
  URL_HASH SHA256=0c8a193b35da955aa7b34ae8d94a03375f8d7f6fbfc2c51aafeb3118aabc8d13
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(djvulibre)  # no CMakeLists.txt upstream: only downloads

set(_djvu_dir "${djvulibre_SOURCE_DIR}/libdjvu")
set(_djvu_sources
  Arrays atomic BSByteStream BSEncodeByteStream ByteStream DataPool ddjvuapi debug
  DjVmDir DjVmDir0 DjVmDoc DjVmNav DjVuAnno DjVuDocEditor DjVuDocument DjVuDumpHelper
  DjVuErrorList DjVuFile DjVuFileCache DjVuGlobal DjVuGlobalMemory DjVuImage DjVuInfo
  DjVuMessage DjVuMessageLite DjVuNavDir DjVuPalette DjVuPort DjVuText DjVuToPS
  GBitmap GContainer GException GIFFManager GMapAreas GOS GPixmap GRect GScaler
  GSmartPointer GString GThreads GUnicode GURL IFFByteStream IW44EncodeCodec IW44Image
  JB2EncodeCodec JB2Image JPEGDecoder miniexp MMRDecoder MMX UnicodeByteStream
  XMLParser XMLTags ZPCodec
)
list(TRANSFORM _djvu_sources PREPEND "${_djvu_dir}/")
list(TRANSFORM _djvu_sources APPEND ".cpp")

add_library(djvulibre STATIC ${_djvu_sources})
target_include_directories(djvulibre SYSTEM PUBLIC "${_djvu_dir}")
# Empty export macros = static linkage. JPEG-encoded DjVu chunks (very rare) are not supported:
# NEED_JPEG_DECODER is left undefined so no libjpeg dependency is needed.
target_compile_definitions(djvulibre
  PUBLIC "DDJVUAPI=" "MINILISPAPI=" "DJVUAPI="
  PRIVATE HAVE_NAMESPACES WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
)
target_compile_options(djvulibre PRIVATE /w /EHsc)
