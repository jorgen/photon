# Examples are added as later phases land (connection demo, prism integration).

add_executable(tour tour.cpp)
target_link_libraries(tour PRIVATE photon::photon vio::vio structify::structify)

if (PHOTON_WITH_PRISM)
    add_executable(prism_service prism_service.cpp)
    target_link_libraries(prism_service PRIVATE photon::photon prism::prism vio::vio)
endif ()
