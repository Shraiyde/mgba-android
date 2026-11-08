cmake_minimum_required(VERSION 3.22)
project(mgba-android LANGUAGES C CXX)

set(MGBA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../..)
set(SDL2_SRC "" CACHE PATH "Path to the SDL2 source tree")

set(LIBMGBA_ONLY ON)
add_subdirectory(${MGBA_ROOT} mgba)

if(NOT SDL2_SRC)
	message(FATAL_ERROR "Pass -DSDL2_SRC=/path/to/SDL2 (see build-apk.sh)")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL2_SRC} sdl2)

add_library(main SHARED
	${MGBA_ROOT}/src/platform/android/android-main.c
	${MGBA_ROOT}/src/platform/android/android-ui.c
	${MGBA_ROOT}/src/platform/android/android-peer-link.c
	${MGBA_ROOT}/src/platform/sdl/sdl-audio.c
	${MGBA_ROOT}/src/platform/sdl/sdl-events.c
	${MGBA_ROOT}/src/platform/sdl/sw-sdl2.c)

# The SDL frontend sources must be compiled with the same feature/function
# detection defines as the core (ENABLE_DIRECTORIES, HAVE_LOCALE_T, ...).
get_target_property(MGBA_DEFS mgba COMPILE_DEFINITIONS)
if(MGBA_DEFS)
	target_compile_definitions(main PRIVATE ${MGBA_DEFS})
endif()

target_include_directories(main PRIVATE
	${MGBA_ROOT}/include
	${MGBA_ROOT}/src
	${MGBA_ROOT}/src/platform/sdl
	${MGBA_ROOT}/src/platform/android
	${CMAKE_CURRENT_BINARY_DIR}/mgba/include)

target_link_libraries(main PRIVATE
	SDL2::SDL2-static
	mgba
	log
	android
	EGL
	GLESv1_CM
	GLESv2
	OpenSLES
	dl)
