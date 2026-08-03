cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(anedya-camera-livestream-example-esp32-arduino)

# esp_peer pulls in sepfy__srtp, whose 2.3.0 sources have a uint32_t vs
# unsigned int* mismatch that GCC 14 promotes to an error.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=incompatible-pointer-types" APPEND)
