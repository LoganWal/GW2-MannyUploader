include(FetchContent)

FetchContent_Declare(
    manny_glaze_source
    URL https://github.com/stephenberry/glaze/archive/refs/tags/v8.0.0.tar.gz
    URL_HASH SHA256=569152f5ec43c510b2ec339476e2d0b78066068855e1a91594dbdfafcd7d248d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(glaze_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(manny_glaze_source)

FetchContent_Declare(
    manny_miniz_source
    URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(manny_miniz_source)

add_library(manny_miniz STATIC "${manny_miniz_source_SOURCE_DIR}/miniz.c")
target_include_directories(manny_miniz SYSTEM PUBLIC "${manny_miniz_source_SOURCE_DIR}")
target_compile_definitions(
    manny_miniz
    PUBLIC
        MINIZ_NO_DEFLATE_APIS
        MINIZ_NO_STDIO
        MINIZ_NO_TIME
        MINIZ_NO_ZLIB_APIS
)
set_target_properties(manny_miniz PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if(WIN32)
    # Keep both halves of the Nexus UI ABI on the exact commits used by the current official
    # Raidcore C++ addon template. The Nexus host and addon must agree on the ImGui ABI.
    FetchContent_Declare(
        manny_nexus_api_source
        URL https://github.com/RaidcoreGG/Nexus-API/archive/9b2c53df86c00db6495642bfcff2d0611bd957ef.tar.gz
        URL_HASH SHA256=ba7813547371fc52953fa3591c78a13acda9f95a71532d95a061df1511720941
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(manny_nexus_api_source)

    add_library(manny_nexus_api INTERFACE)
    target_include_directories(
        manny_nexus_api
        SYSTEM INTERFACE "${manny_nexus_api_source_SOURCE_DIR}"
    )

    FetchContent_Declare(
        manny_imgui_source
        URL https://github.com/RaidcoreGG/imgui/archive/58075c4414b985b352d10718b02a8c43f25efd7c.tar.gz
        URL_HASH SHA256=841321a17d00fd73256af6691dccd868ffb50dcff039d07e749c6f721365f53c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(manny_imgui_source)

    add_library(
        manny_imgui STATIC
        "${manny_imgui_source_SOURCE_DIR}/imgui.cpp"
        "${manny_imgui_source_SOURCE_DIR}/imgui_draw.cpp"
        "${manny_imgui_source_SOURCE_DIR}/imgui_tables.cpp"
        "${manny_imgui_source_SOURCE_DIR}/imgui_widgets.cpp"
    )
    target_include_directories(manny_imgui SYSTEM PUBLIC "${manny_imgui_source_SOURCE_DIR}")
    set_target_properties(
        manny_imgui
        PROPERTIES
            CXX_EXTENSIONS OFF
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
    )

    FetchContent_Declare(
        manny_curl_source
        URL https://curl.se/download/curl-8.21.0.tar.xz
        URL_HASH SHA256=aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    set(manny_parent_build_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
    set(CURL_USE_CMAKECONFIG OFF CACHE BOOL "" FORCE)
    set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_CA_NATIVE ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(USE_WIN32_IDN ON CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE STRING "" FORCE)
    set(CURL_ZSTD OFF CACHE STRING "" FORCE)
    set(CURL_ZLIB OFF CACHE STRING "" FORCE)
    set(CURL_DISABLE_ALTSVC ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_COOKIES ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DOH ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_HSTS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_HTTP_AUTH ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MIME ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_NETRC ON CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(PICKY_COMPILER OFF CACHE BOOL "" FORCE)
    set(CURL_STATIC_CRT ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(manny_curl_source)
    set(BUILD_TESTING "${manny_parent_build_testing}" CACHE BOOL "" FORCE)
endif()
