cmake_minimum_required(VERSION 3.24)
# The app version. Bump it (and CHANGELOG.md) when a feature set lands; see CLAUDE.md.
project(NodeLab VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NODELAB_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
# Portable CMake builds (e.g. WinLibs) ship without CA certificates; reuse Git for Windows' bundle.
set(_git_ca "C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt")
if(NOT CMAKE_TLS_CAINFO AND EXISTS "${_git_ca}")
    set(CMAKE_TLS_CAINFO "${_git_ca}")
endif()
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- dependencies
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8-docking
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

# SOURCE_SUBDIR _none: download only, don't add_subdirectory (no usable CMake in those repos).
FetchContent_Declare(tinyexpr
    GIT_REPOSITORY https://github.com/codeplea/tinyexpr.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

# LibRaw decodes camera RAW files. It ships no CMake build, so the library target is defined below
# from its Makefile.dist object list. Used under its CDDL 1.0 licence option (see README).
FetchContent_Declare(libraw
    GIT_REPOSITORY https://github.com/LibRaw/LibRaw.git
    GIT_TAG 0.21.3
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

# tinyexr (BSD-3) reads OpenEXR files: scanline and tiled, every lossless codec plus PXR24 and
# B44 (not DWA). Built against zlib-ng below instead of its bundled miniz.
FetchContent_Declare(tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG v1.0.12
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

# zlib-ng (in zlib-compatible mode) compresses PNG, TIFF and OpenEXR exports: stb's deflate is
# single-threaded and fixed-Huffman only, and zlib-ng deflates about 2-3x faster than zlib with
# SIMD chosen at run time. It keeps zlib's API, which the parallel PNG writer needs
# (deflateSetDictionary and sync flushes).
set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)  # zlib-ng's tests
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG 2.3.3
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(glfw json imgui stb tinyexpr tinyexr libraw zlib)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(tinyexpr STATIC ${tinyexpr_SOURCE_DIR}/tinyexpr.c)
target_include_directories(tinyexpr PUBLIC ${tinyexpr_SOURCE_DIR})

add_library(libraw STATIC
    ${libraw_SOURCE_DIR}/src/libraw_datastream.cpp
    ${libraw_SOURCE_DIR}/src/libraw_c_api.cpp
    ${libraw_SOURCE_DIR}/src/tables/cameralist.cpp
    ${libraw_SOURCE_DIR}/src/decoders/fuji_compressed.cpp
    ${libraw_SOURCE_DIR}/src/decoders/crx.cpp
    ${libraw_SOURCE_DIR}/src/decoders/fp_dng.cpp
    ${libraw_SOURCE_DIR}/src/decoders/decoders_libraw.cpp
    ${libraw_SOURCE_DIR}/src/decoders/unpack.cpp
    ${libraw_SOURCE_DIR}/src/decoders/unpack_thumb.cpp
    ${libraw_SOURCE_DIR}/src/integration/rawspeed_glue.cpp
    ${libraw_SOURCE_DIR}/src/integration/dngsdk_glue.cpp
    ${libraw_SOURCE_DIR}/src/tables/colorconst.cpp
    ${libraw_SOURCE_DIR}/src/utils/utils_libraw.cpp
    ${libraw_SOURCE_DIR}/src/utils/init_close_utils.cpp
    ${libraw_SOURCE_DIR}/src/utils/decoder_info.cpp
    ${libraw_SOURCE_DIR}/src/utils/open.cpp
    ${libraw_SOURCE_DIR}/src/utils/phaseone_processing.cpp
    ${libraw_SOURCE_DIR}/src/utils/thumb_utils.cpp
    ${libraw_SOURCE_DIR}/src/write/tiff_writer.cpp
    ${libraw_SOURCE_DIR}/src/preprocessing/subtract_black.cpp
    ${libraw_SOURCE_DIR}/src/postprocessing/postprocessing_utils.cpp
    ${libraw_SOURCE_DIR}/src/postprocessing/dcraw_process.cpp
    ${libraw_SOURCE_DIR}/src/preprocessing/raw2image.cpp
    ${libraw_SOURCE_DIR}/src/postprocessing/mem_image.cpp
    ${libraw_SOURCE_DIR}/src/x3f/x3f_utils_patched.cpp
    ${libraw_SOURCE_DIR}/src/x3f/x3f_parse_process.cpp
    ${libraw_SOURCE_DIR}/src/utils/read_utils.cpp
    ${libraw_SOURCE_DIR}/src/utils/curves.cpp
    ${libraw_SOURCE_DIR}/src/utils/utils_dcraw.cpp
    ${libraw_SOURCE_DIR}/src/tables/colordata.cpp
    ${libraw_SOURCE_DIR}/src/decoders/canon_600.cpp
    ${libraw_SOURCE_DIR}/src/decoders/decoders_dcraw.cpp
    ${libraw_SOURCE_DIR}/src/decoders/decoders_libraw_dcrdefs.cpp
    ${libraw_SOURCE_DIR}/src/decoders/generic.cpp
    ${libraw_SOURCE_DIR}/src/decoders/kodak_decoders.cpp
    ${libraw_SOURCE_DIR}/src/decoders/dng.cpp
    ${libraw_SOURCE_DIR}/src/decoders/smal.cpp
    ${libraw_SOURCE_DIR}/src/decoders/load_mfbacks.cpp
    ${libraw_SOURCE_DIR}/src/metadata/sony.cpp
    ${libraw_SOURCE_DIR}/src/metadata/nikon.cpp
    ${libraw_SOURCE_DIR}/src/metadata/samsung.cpp
    ${libraw_SOURCE_DIR}/src/metadata/cr3_parser.cpp
    ${libraw_SOURCE_DIR}/src/metadata/canon.cpp
    ${libraw_SOURCE_DIR}/src/metadata/epson.cpp
    ${libraw_SOURCE_DIR}/src/metadata/olympus.cpp
    ${libraw_SOURCE_DIR}/src/metadata/leica.cpp
    ${libraw_SOURCE_DIR}/src/metadata/fuji.cpp
    ${libraw_SOURCE_DIR}/src/metadata/adobepano.cpp
    ${libraw_SOURCE_DIR}/src/metadata/pentax.cpp
    ${libraw_SOURCE_DIR}/src/metadata/p1.cpp
    ${libraw_SOURCE_DIR}/src/metadata/makernotes.cpp
    ${libraw_SOURCE_DIR}/src/metadata/exif_gps.cpp
    ${libraw_SOURCE_DIR}/src/metadata/kodak.cpp
    ${libraw_SOURCE_DIR}/src/metadata/tiff.cpp
    ${libraw_SOURCE_DIR}/src/metadata/ciff.cpp
    ${libraw_SOURCE_DIR}/src/metadata/mediumformat.cpp
    ${libraw_SOURCE_DIR}/src/metadata/minolta.cpp
    ${libraw_SOURCE_DIR}/src/metadata/identify_tools.cpp
    ${libraw_SOURCE_DIR}/src/metadata/hasselblad_model.cpp
    ${libraw_SOURCE_DIR}/src/metadata/normalize_model.cpp
    ${libraw_SOURCE_DIR}/src/metadata/identify.cpp
    ${libraw_SOURCE_DIR}/src/metadata/misc_parsers.cpp
    ${libraw_SOURCE_DIR}/src/tables/wblists.cpp
    ${libraw_SOURCE_DIR}/src/postprocessing/postprocessing_aux.cpp
    ${libraw_SOURCE_DIR}/src/postprocessing/postprocessing_utils_dcrdefs.cpp
    ${libraw_SOURCE_DIR}/src/postprocessing/aspect_ratio.cpp
    ${libraw_SOURCE_DIR}/src/demosaic/misc_demosaic.cpp
    ${libraw_SOURCE_DIR}/src/demosaic/xtrans_demosaic.cpp
    ${libraw_SOURCE_DIR}/src/demosaic/ahd_demosaic.cpp
    ${libraw_SOURCE_DIR}/src/demosaic/dht_demosaic.cpp
    ${libraw_SOURCE_DIR}/src/demosaic/aahd_demosaic.cpp
    ${libraw_SOURCE_DIR}/src/demosaic/dcb_demosaic.cpp
    ${libraw_SOURCE_DIR}/src/write/file_write.cpp
    ${libraw_SOURCE_DIR}/src/preprocessing/ext_preprocess.cpp
    ${libraw_SOURCE_DIR}/src/write/apply_profile.cpp)
target_include_directories(libraw PUBLIC ${libraw_SOURCE_DIR})
target_compile_definitions(libraw PUBLIC LIBRAW_NODLL)
# Third-party code: its warnings aren't ours to fix.
target_compile_options(libraw PRIVATE -w)
# dcraw-era code relies on M_PI and friends, which strict -std=c++20 hides.
set_target_properties(libraw PROPERTIES CXX_EXTENSIONS ON)
target_compile_definitions(libraw PRIVATE _USE_MATH_DEFINES)
# Its demosaicers and decoders are OpenMP-parallel, which LibRaw only enables for MinGW when forced.
# PUBLIC because libraw_alloc.h's inline allocator differs with OpenMP: every file including
# LibRaw must see the same definition.
target_compile_definitions(libraw PUBLIC LIBRAW_FORCE_OPENMP)
target_compile_options(libraw PUBLIC -fopenmp)
target_link_options(libraw PUBLIC -fopenmp)
if(WIN32)
    target_link_libraries(libraw PUBLIC ws2_32)
endif()

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})
add_library(tinyexr INTERFACE)
target_include_directories(tinyexr INTERFACE ${tinyexr_SOURCE_DIR})

# ---------------------------------------------------------------- version info
# Regenerated on every build so the commit hash is current; the file only changes when the hash does.
set(NODELAB_VERSION_CPP ${CMAKE_BINARY_DIR}/generated/Version.cpp)
add_custom_target(nodelab_version
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/cmake/Version.cpp.in
        -DOUT=${NODELAB_VERSION_CPP}
        -DGIT_DIR=${CMAKE_SOURCE_DIR}
        -DVERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${NODELAB_VERSION_CPP}
    COMMENT "Stamping version ${PROJECT_VERSION}")

# GUIDE.md is compiled in so Help > Guide works without files next to the exe.
set(NODELAB_GUIDE_CPP ${CMAKE_BINARY_DIR}/generated/Guide.cpp)
add_custom_command(OUTPUT ${NODELAB_GUIDE_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/GUIDE.md -DOUT=${NODELAB_GUIDE_CPP} -DNAME=kGuideMarkdown
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/GUIDE.md ${CMAKE_SOURCE_DIR}/cmake/EmbedText.cmake
    COMMENT "Embedding GUIDE.md")

# Windows can't overwrite a running exe but can rename it; move the old one aside before linking
# so a single build/ folder works while NodeLab is open.
function(nodelab_allow_relink target)
    add_custom_command(TARGET ${target} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${target}> -P ${CMAKE_SOURCE_DIR}/cmake/MoveAsideExe.cmake)
endfunction()

# ---------------------------------------------------------------- core library (no GUI)
add_library(nodelab_core STATIC
    ${NODELAB_VERSION_CPP}
    ${NODELAB_GUIDE_CPP}
    src/core/Value.cpp
    src/gpu/GL.cpp
    src/gpu/Device.cpp
    src/gpu/PointOp.cpp
    src/gpu/Blur.cpp
    src/gpu/Reduce.cpp
    src/gpu/Display.cpp
    src/core/ColorManagement.cpp
    src/core/ColorScience.cpp
    src/core/Parallel.cpp
    src/core/Curve.cpp
    src/core/Ramp.cpp
    src/graph/Graph.cpp
    src/graph/Layout.cpp
    src/graph/Recipes.cpp
    src/graph/NodeMenu.cpp
    src/graph/NodeRegistry.cpp
    src/graph/Evaluator.cpp
    src/io/ImageIO.cpp
    src/io/ImageWrite.cpp
    src/io/Icc.cpp
    src/io/JpegDecode.cpp
    src/io/PngDecode.cpp
    src/io/TiffDecode.cpp
    src/io/ExrDecode.cpp
    src/io/RawDecode.cpp
    src/io/Exif.cpp
    src/io/ImageCache.cpp
    src/io/Library.cpp
    src/io/Presets.cpp
    src/io/ProjectFile.cpp
    src/io/Export.cpp
    src/io/LensProfiles.cpp
    src/nodes/io/IONodes.cpp
    src/nodes/color/ColorNodes.cpp
    src/nodes/color/DevelopNodes.cpp
    src/nodes/color/AutoTone.cpp
    src/nodes/math/MathNodes.cpp
    src/nodes/converter/ConverterNodes.cpp
    src/nodes/converter/Expression.cpp
    src/nodes/group/GroupNodes.cpp
    src/nodes/ImageOps.cpp
    src/nodes/filter/FilterNodes.cpp
    src/nodes/filter/Denoise.cpp
    src/nodes/filter/SpotRemoval.cpp
    src/nodes/transform/TransformNodes.cpp
    src/nodes/matte/MatteNodes.cpp
    src/nodes/matte/AutoMask.cpp
    src/ml/Http.cpp
    src/ml/Models.cpp
    src/ml/Onnx.cpp
    src/nodes/texture/TextureNodes.cpp
    src/nodes/utility/UtilityNodes.cpp)
target_include_directories(nodelab_core PUBLIC src)
# ONNX Runtime's C API header (MIT): the runtime itself is loaded at run time (src/ml/Onnx.cpp).
target_include_directories(nodelab_core PRIVATE third_party/onnxruntime)
add_dependencies(nodelab_core nodelab_version)
target_link_libraries(nodelab_core PUBLIC nlohmann_json::nlohmann_json stb tinyexpr tinyexr libraw zlib glfw OpenGL::GL)
if(WIN32)
    target_compile_definitions(nodelab_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    # winhttp: AI model downloads (src/ml/Http.cpp); shell32: the Recycle Bin (src/io/Library.cpp)
    target_link_libraries(nodelab_core PUBLIC winhttp shell32)
endif()

# ---------------------------------------------------------------- application
add_executable(NodeLab
    src/main.cpp
    src/ui/App.cpp
    src/ui/SystemStats.cpp
    src/ui/ImageView.cpp
    src/ui/DisplayWorker.cpp
    src/ui/NodeEditor.cpp
    src/ui/Inspector.cpp
    src/ui/GuideWindow.cpp
    src/ui/LibraryPanel.cpp
    src/ui/Theme.cpp
    src/ui/FileDialog.cpp
    src/ui/UiScript.cpp
    src/ui/ParamWidgets.cpp
    src/ui/SliderTrack.cpp
    src/ui/ViewerOverlay.cpp
    src/ui/NodeInspectors.cpp)
target_link_libraries(NodeLab PRIVATE nodelab_core imgui)
nodelab_allow_relink(NodeLab)
if(WIN32)
    enable_language(RC)
    configure_file(cmake/NodeLab.rc.in ${CMAKE_BINARY_DIR}/generated/NodeLab.rc @ONLY)
    target_sources(NodeLab PRIVATE ${CMAKE_BINARY_DIR}/generated/NodeLab.rc)
endif()
if(WIN32)
    target_link_libraries(NodeLab PRIVATE comdlg32 ole32 uuid)
endif()
if(MINGW)
    target_link_options(NodeLab PRIVATE -static -static-libgcc -static-libstdc++)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(NodeLab PRIVATE -mwindows)
    endif()
endif()

# ---------------------------------------------------------------- tests
if(NODELAB_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _none)
    FetchContent_MakeAvailable(doctest)

    enable_testing()
    add_executable(nodelab_tests tests/test_main.cpp tests/test_core.cpp tests/test_nodes.cpp tests/test_groups.cpp tests/test_nodes2.cpp tests/test_version.cpp tests/test_guide.cpp tests/test_layout.cpp tests/test_export.cpp tests/test_develop.cpp tests/test_ir_tools.cpp tests/test_expression_vm.cpp tests/test_parallel.cpp tests/test_colorpipe.cpp tests/test_develop_linear.cpp tests/test_raw_exif.cpp tests/test_image_write.cpp tests/test_roi.cpp tests/test_icc.cpp tests/test_gpu.cpp tests/test_denoise.cpp tests/test_masks.cpp tests/test_library.cpp tests/test_prefs.cpp tests/test_fuzz.cpp tests/test_jpeg_decode.cpp
    tests/test_png_decode.cpp tests/test_decode_fuzz.cpp tests/test_node_menu.cpp tests/test_retouch.cpp tests/test_presets.cpp tests/test_automask.cpp tests/test_grain.cpp tests/test_golden.cpp tests/test_tiff_exr.cpp tests/test_perspective.cpp tests/test_lens_profile.cpp src/ui/Theme.cpp)
    target_include_directories(nodelab_tests PRIVATE ${doctest_SOURCE_DIR})
    # Tests that load the shipped example projects find them here.
    target_compile_definitions(nodelab_tests PRIVATE NODELAB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(nodelab_tests PRIVATE nodelab_core imgui)
    nodelab_allow_relink(nodelab_tests)
    if(MINGW)
        target_link_options(nodelab_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME nodelab_tests COMMAND nodelab_tests)
endif()
