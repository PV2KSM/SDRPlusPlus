cmake_minimum_required(VERSION 3.13)
project(acars_decoder C CXX)

# Núcleo do decodificador ACARS + integração com a libacars (CPDLC, ADS-C, MIAM)
file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

add_subdirectory(libacars)
target_link_libraries(acars_decoder PRIVATE sdrpp_libacars)
target_include_directories(acars_decoder PRIVATE "src/")

# Teste offline (não é instalado): build-enxuta/decoder_modules/acars_decoder/test_acars
if (NOT WIN32 AND NOT ANDROID)
    add_executable(test_acars test/test_acars.cpp src/acars.cpp src/apps.cpp)
    target_include_directories(test_acars PRIVATE "src/")
    target_link_libraries(test_acars PRIVATE sdrpp_libacars)
    set_target_properties(test_acars PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif ()
