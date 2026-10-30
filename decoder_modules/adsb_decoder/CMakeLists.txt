cmake_minimum_required(VERSION 3.13)
project(adsb_decoder)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

# Embute a página do mapa (src/web/index.html) num header gerado
set(ADSB_WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html")
set(ADSB_WEB_HDR "${CMAKE_CURRENT_BINARY_DIR}/generated/adsb_web_page.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ADSB_WEB_SRC}")
file(READ "${ADSB_WEB_SRC}" ADSB_WEB_CONTENT)
file(WRITE "${ADSB_WEB_HDR}" "#pragma once\nstatic const char* ADSB_WEB_PAGE = R\"__ADSB_HTML__(${ADSB_WEB_CONTENT})__ADSB_HTML__\";\n")

target_include_directories(adsb_decoder PRIVATE "src/" "${CMAKE_CURRENT_BINARY_DIR}/generated")

if (WIN32)
    target_link_libraries(adsb_decoder PRIVATE shell32)
endif ()
