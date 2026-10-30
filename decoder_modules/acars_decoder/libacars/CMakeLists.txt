# libacars 2.2.1 (MIT, https://github.com/szpajder/libacars) compilada como
# biblioteca estática dentro do módulo ACARS. Sem zlib/libxml2/jansson.
include(CheckSymbolExists)
include(CheckPrototypeDefinition)
include(CheckIncludeFile)
include(TestBigEndian)

set(LA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libacars")
include("${LA_DIR}/asn1/CMakeLists.inc")

TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
CHECK_SYMBOL_EXISTS(strsep string.h HAVE_STRSEP)
set(CMAKE_REQUIRED_DEFINITIONS_SAVE ${CMAKE_REQUIRED_DEFINITIONS})
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} "-D_GNU_SOURCE")
CHECK_SYMBOL_EXISTS(memmem string.h HAVE_MEMMEM)
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS_SAVE})
check_prototype_definition(lfind
    "void *lfind(const void *key, const void *base, size_t *nmemb, size_t size, int(*compar)(const void *, const void *))"
    "NULL" "stddef.h;search.h" LFIND_NMEMB_SIZE_SIZE_T)
check_prototype_definition(lfind
    "void *lfind(const void *key, const void *base, unsigned int *nmemb, unsigned int size, int(*compar)(const void *, const void *))"
    "NULL" "stddef.h;search.h" LFIND_NMEMB_SIZE_UINT)
if (NOT LFIND_NMEMB_SIZE_SIZE_T AND NOT LFIND_NMEMB_SIZE_UINT)
    set(LFIND_NMEMB_SIZE_SIZE_T ON)
endif ()
CHECK_INCLUDE_FILE("sys/time.h" HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILE("unistd.h" HAVE_UNISTD_H)

set(LA_GEN "${CMAKE_CURRENT_BINARY_DIR}/la_generated")
configure_file("${LA_DIR}/config.h.in" "${LA_GEN}/config.h" @ONLY)
file(WRITE "${LA_GEN}/version.c" "char const * const LA_VERSION=\"2.2.1\";\n")

set(LA_CORE_SOURCES
    acars.c adsc.c arinc.c asn1-format-common.c asn1-format-cpdlc-text.c asn1-format-cpdlc-json.c
    asn1-util.c bitstream.c configuration.c cpdlc.c crc.c dict.c hash.c json.c libacars.c list.c
    media-adv.c miam.c miam-core.c ohma.c reassembly.c util.c vstring.c)
list(TRANSFORM LA_CORE_SOURCES PREPEND "${LA_DIR}/")
list(TRANSFORM ASN_MODULE_SOURCES PREPEND "${LA_DIR}/asn1/")

add_library(sdrpp_libacars_asn1 OBJECT ${ASN_MODULE_SOURCES})
add_library(sdrpp_libacars STATIC ${LA_CORE_SOURCES} "${LA_GEN}/version.c" $<TARGET_OBJECTS:sdrpp_libacars_asn1>)

foreach (T sdrpp_libacars_asn1 sdrpp_libacars)
    set_property(TARGET ${T} PROPERTY POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET ${T} PROPERTY C_STANDARD 11)
    # PUBLIC só a raiz (para <libacars/...>); o config.h gerado fica PRIVATE para não
    # colidir com o config.h do SDR++
    target_include_directories(${T} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${T} PRIVATE "${LA_GEN}" "${LA_DIR}" "${LA_DIR}/asn1")
    if (MSVC)
        target_compile_definitions(${T} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
        target_compile_options(${T} PRIVATE /w)
    else ()
        target_compile_options(${T} PRIVATE -w)
    endif ()
endforeach ()
# strdup, strtok_r, strnlen, strsep (mesmas definições do projeto original)
target_compile_definitions(sdrpp_libacars PRIVATE _XOPEN_SOURCE=600 _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
target_compile_definitions(sdrpp_libacars_asn1 PRIVATE _DEFAULT_SOURCE)
if (APPLE)
    target_compile_definitions(sdrpp_libacars PRIVATE _DARWIN_C_SOURCE)
endif ()
if (NOT MSVC AND NOT APPLE)
    target_link_libraries(sdrpp_libacars PUBLIC m)
endif ()
