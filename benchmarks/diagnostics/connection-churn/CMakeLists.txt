cmake_minimum_required(VERSION 3.21)
project(fulla-connection-probe LANGUAGES CXX)

# Standalone upstream reproducer: deliberately does not link Fulla or its DBs.
find_package(Drogon CONFIG REQUIRED)
add_executable(fulla-connection-probe server.cc)
target_compile_features(fulla-connection-probe PRIVATE cxx_std_17)
target_link_libraries(fulla-connection-probe PRIVATE Drogon::Drogon)
