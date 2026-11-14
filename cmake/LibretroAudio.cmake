include(FetchContent)
set(LIBRETRO_AUDIO_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(libretro_build_audio)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()

    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_TESTING OFF)
    set(BUILD_PROGRAMS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(INSTALL_DOCS OFF)
    set(INSTALL_MANPAGES OFF)
    set(BUILD_CXXLIBS OFF)
    set(BUILD_LIBOUT123 OFF)
    set(ENABLE_CPACK OFF)
    set(ENABLE_PACKAGE_CONFIG OFF)
    set(ENABLE_EXTERNAL_LIBS ON)

    # MP3 decoding uses ZMusic's mpg123 decoder.
    set(ENABLE_MPEG OFF)
    set(CMAKE_DISABLE_FIND_PACKAGE_mp3lame TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_mpg123 TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_Speex TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_SQLite3 TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_ALSA TRUE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(BUILD_FRAMEWORK OFF)
    set(OPUS_BUILD_FRAMEWORK OFF)

    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)

    # libsndfile otherwise switches to installed packages under vcpkg.
    set(VCPKG_TOOLCHAIN OFF)
    list(PREPEND CMAKE_MODULE_PATH "${LIBRETRO_AUDIO_CMAKE_DIR}/libretro-audio")

    FetchContent_Declare(libretro_ogg
        URL https://codeload.github.com/xiph/ogg/tar.gz/refs/tags/v1.3.5
        URL_HASH SHA256=f6f1b04cfa4e98b70ffe775d5e302d9c6b98541f05159af6de2d6617817ed7d6)
    FetchContent_Declare(libretro_vorbis
        URL https://codeload.github.com/xiph/vorbis/tar.gz/refs/tags/v1.3.7
        URL_HASH SHA256=270c76933d0934e42c5ee0a54a36280e2d87af1de3cc3e584806357e237afd13)
    FetchContent_Declare(libretro_flac
        URL https://codeload.github.com/xiph/flac/tar.gz/refs/tags/1.4.3
        URL_HASH SHA256=0a4bb82a30609b606650d538a804a7b40205366ce8fc98871b0ecf3fbb0611ee)
    FetchContent_Declare(libretro_opus
        URL https://codeload.github.com/xiph/opus/tar.gz/refs/tags/v1.5.2
        URL_HASH SHA256=9480e329e989f70d69886ded470c7f8cfe6c0667cc4196d4837ac9e668fb7404)
    FetchContent_Declare(libretro_sndfile
        URL https://codeload.github.com/libsndfile/libsndfile/tar.gz/refs/tags/1.2.2
        URL_HASH SHA256=ffe12ef8add3eaca876f04087734e6e8e029350082f3251f565fa9da55b52121)
    FetchContent_Declare(libretro_mpg123
        URL https://www.mpg123.de/download/mpg123-1.32.9.tar.bz2
        URL_HASH SHA256=03b61e4004e960bacf2acdada03ed94d376e6aab27a601447bd4908d8407b291)

    FetchContent_MakeAvailable(libretro_ogg libretro_vorbis libretro_flac libretro_opus)

    file(GLOB opus_headers "${libretro_opus_SOURCE_DIR}/include/*.h")

    foreach(header IN LISTS opus_headers)
        get_filename_component(name "${header}" NAME)
        configure_file("${header}" "${libretro_opus_BINARY_DIR}/include/opus/${name}" COPYONLY)
    endforeach()

    target_include_directories(opus INTERFACE
        "$<BUILD_INTERFACE:${libretro_opus_BINARY_DIR}/include>")

    FetchContent_MakeAvailable(libretro_sndfile libretro_mpg123)

    # mpg123 1.32.9 only assumes an ARM64 FPU on Apple platforms
    set(mpg123_cmake "${libretro_mpg123_SOURCE_DIR}/ports/cmake/src/CMakeLists.txt")
    file(READ "${mpg123_cmake}" mpg123_cmake_original)
    string(REPLACE "if(WIN32 OR (ARCH_IS_ARM64 AND APPLE))"
        "if(WIN32 OR ARCH_IS_ARM64)"
        mpg123_cmake_patched "${mpg123_cmake_original}")
    if(NOT mpg123_cmake_patched STREQUAL mpg123_cmake_original)
        file(WRITE "${mpg123_cmake}" "${mpg123_cmake_patched}")
    endif()

    add_subdirectory("${libretro_mpg123_SOURCE_DIR}/ports/cmake" "${libretro_mpg123_BINARY_DIR}")

    # This mpg123 release exports the generated-header directory only.
    target_include_directories(libmpg123 INTERFACE
        "$<BUILD_INTERFACE:${libretro_mpg123_SOURCE_DIR}/src/include>")

    foreach(codec ogg vorbis vorbisenc FLAC opus sndfile libmpg123)
        get_target_property(codec_type ${codec} TYPE)
        if(NOT codec_type STREQUAL "STATIC_LIBRARY")
            message(FATAL_ERROR "The libretro audio codec ${codec} must be static")
        endif()
    endforeach()

    add_library(libretro_audio INTERFACE)
    target_link_libraries(libretro_audio INTERFACE SndFile::sndfile libmpg123)
endfunction()
