cmake_minimum_required(VERSION 3.16)

# Eigenstaendiges Host-Tool (nicht Teil des SKSE-Plugins): wertet Sampling-Daten aus .tracy-Dateien aus.
# Aufbau analog zu extern/tracy/csvexport.
set(TRACY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../extern/tracy)
set(NO_STATISTICS OFF)

include(${TRACY_ROOT}/cmake/version.cmake)

set(CMAKE_CXX_STANDARD 20)

project(
    tracy-sampling-export
    LANGUAGES C CXX
    VERSION ${TRACY_VERSION_STRING}
)

include(${TRACY_ROOT}/cmake/config.cmake)
include(${TRACY_ROOT}/cmake/vendor.cmake)
include(${TRACY_ROOT}/cmake/server.cmake)

add_executable(${PROJECT_NAME} sampling-export.cpp ${COMMON_FILES} ${SERVER_FILES})
target_link_libraries(${PROJECT_NAME} PRIVATE TracyServer)
