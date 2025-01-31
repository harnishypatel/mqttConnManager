#   Copyright 2023
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

cmake_minimum_required(VERSION 2.8.7)

project(mqttconmgr)

include(ExternalProject)

add_definitions(-std=c99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -g -Werror -Wall -Wno-sizeof-pointer-memaccess -Wno-format-truncation -Wno-unused-parameter -D_GNU_SOURCE=1")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c99 -g -Werror -Wall -Wno-sizeof-pointer-memaccess -Wno-format-truncation -Wno-unused-parameter -D_GNU_SOURCE=1")

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
set(CMAKE_MACOSX_RPATH 1)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -undefined dynamic_lookup")
endif()

set(INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/_install)
set(PREFIX_DIR ${CMAKE_CURRENT_BINARY_DIR}/_prefix)
set(INCLUDE_DIR ${INSTALL_DIR}/include)
set(LIBRARY_DIR ${INSTALL_DIR}/lib)
set(LIBRARY_DIR64 ${INSTALL_DIR}/lib64)
set(COMMON_LIBRARY_DIR ${INSTALL_DIR}/lib/${CMAKE_LIBRARY_ARCHITECTURE})

include_directories(${INCLUDE_DIR}
                    ${INCLUDE_DIR}/mosquitto
                    ${INCLUDE_DIR}/rbus
		    ${INCLUDE_DIR}/rtmessage
                    )


if (NOT BUILD_YOCTO)

# mosquitto external dependency
#-------------------------------------------------------------------------------
ExternalProject_Add(mosquitto
    PREFIX ${CMAKE_CURRENT_BINARY_DIR}/_prefix/mosquitto
    GIT_REPOSITORY https://github.com/eclipse/mosquitto.git
    GIT_TAG "master"
    CMAKE_ARGS += -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR} -DWITH_CJSON=no -DWITH_BROKER=ON
)
add_library(libmosquitto STATIC SHARED IMPORTED)
add_dependencies(libmosquitto mosquitto)

# rbus external dependency
#-------------------------------------------------------------------------------
ExternalProject_Add(rbus
     PREFIX ${CMAKE_CURRENT_BINARY_DIR}/_prefix/rbus
     GIT_REPOSITORY https://github.com/rdkcentral/rbus.git
     GIT_TAG main
     CMAKE_ARGS += -DBUILD_FOR_DESKTOP=ON -DCMAKE_INSTALL_PREFIX=${INSTALL_DIR} -DBUILD_TESTING=OFF
)

add_library(librbuscore STATIC SHARED IMPORTED)
add_dependencies(librbuscore rbuscore)

add_library(librtMessage STATIC SHARED IMPORTED)
add_dependencies(librtMessage rtMessage)

add_library(librbus STATIC SHARED IMPORTED)
add_dependencies(librbus rbus)

endif ()
link_directories ( ${LIBRARY_DIR} ${COMMON_LIBRARY_DIR} ${LIBRARY_DIR64} )


add_subdirectory(src)

#if (BUILD_TESTING)
 #   add_subdirectory(tests)
#endif (BUILD_TESTING)
