cmake_minimum_required(VERSION 3.13)
project(aprs_decoder)

# APRS (AFSK 1200 / AX.25): estações no painel e no mapa do ADS-B
file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(aprs_decoder PRIVATE "src/")

if (WIN32)
    target_link_libraries(aprs_decoder PRIVATE shell32)
endif ()
