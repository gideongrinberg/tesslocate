cmake_minimum_required(VERSION 3.31)
project(tesslocate)

set(CMAKE_CXX_STANDARD 20)
if(CMAKE_BUILD_TYPE STREQUAL "RELEASE")
    set(VCPKG_BUILD_TYPE release)
endif()

find_package(s2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(curl CONFIG REQUIRED)
find_package(OpenMP)

add_executable(tesslocate main.cpp external/csv.h
        external/cxxopts.h)
target_link_libraries(tesslocate PRIVATE s2::s2 nlohmann_json::nlohmann_json curl)

if(OpenMP_CXX_FOUND)
    target_link_libraries(tesslocate PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(tesslocate PRIVATE USE_OPENMP)
else()
    message(WARNING "OpenMP not found. Proceeding without it.")
endif()