project(ir_rec)
cmake_minimum_required(VERSION 2.8)
aux_source_directory(. SRC_LIST)

add_executable(${PROJECT_NAME} ${SRC_LIST})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

find_package(Liblo REQUIRED)
message("-- Liblo headers: ${LO_INCLUDE_DIRS}")
message("-- Liblo lib: ${LO_LIBRARIES}")

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
   # Mac OS X specific code
   message("Building on OS X")
   find_library(AUDIOUNIT_FM AudioUnit)
   find_library(COREAUDIO_FM CoreAudio)
   find_library(CORESERVICES_FM CoreServices)
   find_library(AUDIOTOOLBOX_FM AudioToolbox)
endif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")

find_package(Jack)
if(JACK_FOUND)
message("-- Jack headers: ${JACK_INCLUDE_DIRS}")
message("-- Jack lib: ${JACK_LIBRARIES}")
add_definitions(-DUSE_JACK)
include_directories(${JACK_INCLUDE_DIRS})
endif(JACK_FOUND)

find_package(Portaudio REQUIRED)
message("-- Portaudio headers: ${PORTAUDIO_INCLUDE_DIRS}   ${PORTAUDIO_DEFINITIONS}")
find_package(LibSndFile REQUIRED)
message("-- libsndfile headers: ${SNDFILE_INCLUDE_DIRS}")

include_directories(${PORTAUDIO_INCLUDE_DIRS} ${SNDFILE_INCLUDE_DIRS}
					${LO_INCLUDE_DIRS} ${JACK_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${PORTAUDIO_LIBRARIES}
					  ${SNDFILE_LIBRARIES} ${LO_LIBRARIES} ${JACK_LIBRARIES}
					  "-lm" "-lpthread")

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
   target_link_libraries(${PROJECT_NAME} ${AUDIOUNIT_FM} ${COREAUDIO_FM} ${CORESERVICES_FM} ${AUDIOTOOLBOX_FM})
endif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")



