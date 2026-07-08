set(public_headers
        photon/photon.h
        photon/error.h
)
set(sources
        photon/version.cpp
)

add_library(photon ${public_headers} ${sources})
add_library(photon::photon ALIAS photon)

generate_export_header(photon
        BASE_NAME photon
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/photon/photon_export.h
)

target_compile_features(photon PUBLIC cxx_std_23)
target_compile_definitions(photon PRIVATE PHOTON_VERSION="${PROJECT_VERSION}")

target_link_libraries(photon PUBLIC vio::vio structify::structify)

target_include_directories(photon
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(photon PROPERTIES
        PUBLIC_HEADER "${public_headers}"
)

setWarningFlagsForTarget(photon)
