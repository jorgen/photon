set(public_headers
        photon/photon.h
        photon/error.h
        photon/params.h
        photon/decode.h
        photon/types.h
        photon/named.h
        photon/connect_params.h
        photon/row_binding.h
        photon/result.h
        photon/connection.h
        photon/transaction.h
        photon/pipeline.h
        photon/copy.h
        photon/pool.h
)
set(sources
        photon/version.cpp
        photon/connect_params.cpp
        photon/connection.cpp
        photon/named.cpp
        photon/transaction.cpp
        photon/pipeline.cpp
        photon/copy.cpp
        photon/pool.cpp
        photon/detail/message.cpp
        photon/detail/scram.cpp
        photon/detail/transport.cpp
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

if (PHOTON_WITH_PRISM)
    target_link_libraries(photon PUBLIC prism::prism)
endif ()

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
