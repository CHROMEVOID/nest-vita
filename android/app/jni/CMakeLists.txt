# Nest for Android: Nest's own code (src/) on top of the port layer (port/), with SDL2,
# SQLite, libwebp and unrar compiled from source. Run android/fetch_deps.sh first.
cmake_minimum_required(VERSION 3.18)
project(NestAndroid C CXX)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DEPS "${REPO}/android/deps")

# ---- SDL2 (libSDL2.so; its Java half is compiled by Gradle)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/SDL2 sdl2)

# ---- libwebp (decoder only)
foreach(o WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP WEBP_BUILD_IMG2WEBP
          WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS WEBP_BUILD_LIBWEBPMUX)
  set(${o} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${DEPS}/libwebp webp EXCLUDE_FROM_ALL)

# ---- SQLite (Nest brings its own file layer, as on the Vita)
add_library(nest_sqlite STATIC ${DEPS}/sqlite/sqlite3.c)
target_compile_options(nest_sqlite PRIVATE -w -O2)
target_compile_definitions(nest_sqlite PRIVATE SQLITE_OS_OTHER=1 SQLITE_THREADSAFE=0 SQLITE_TEMP_STORE=3
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_WAL SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_DEPRECATED SQLITE_OMIT_SHARED_CACHE)

# ---- unrar (.cbr comics), when fetch_deps.sh got it
set(HAVE_UNRAR OFF)
if(EXISTS ${DEPS}/unrar_srcs.txt)
  file(STRINGS ${DEPS}/unrar_srcs.txt UNRAR_NAMES)
  list(LENGTH UNRAR_NAMES UNRAR_N)
  if(UNRAR_N GREATER 10)
    set(UNRAR_SRC "")
    foreach(f ${UNRAR_NAMES})
      list(APPEND UNRAR_SRC ${DEPS}/unrar/${f})
    endforeach()
    add_library(nest_unrar STATIC ${UNRAR_SRC})
    target_compile_options(nest_unrar PRIVATE -w -O2 -std=c++11 -fno-strict-aliasing)
    target_compile_definitions(nest_unrar PRIVATE RARDLL SILENT _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE RAR_SMP)
    set(HAVE_UNRAR ON)
  endif()
endif()

# ---- Nest
file(GLOB NEST_SRC ${REPO}/src/*.c)
set(PORT_SRC sys gl input audio ime http assets avplayer_android)
list(TRANSFORM PORT_SRC PREPEND ${REPO}/port/)
list(TRANSFORM PORT_SRC APPEND .c)

add_library(main SHARED ${NEST_SRC} ${PORT_SRC})
target_include_directories(main PRIVATE
  ${REPO}/port/include ${REPO}/port ${REPO}/src ${REPO}/third_party
  ${DEPS}/sqlite ${DEPS}/SDL2/include ${DEPS}/libwebp/src)
target_compile_options(main PRIVATE -std=gnu11 -O2 -w -fno-strict-aliasing -include ${REPO}/port/port.h)
target_compile_definitions(main PRIVATE NEST_HAVE_WEBP=1)
target_link_libraries(main PRIVATE SDL2 nest_sqlite webpdecoder mediandk android log GLESv2 EGL z m)
if(HAVE_UNRAR)
  target_include_directories(main PRIVATE ${DEPS}/unrar_inc)
  target_compile_definitions(main PRIVATE NEST_HAVE_UNRAR=1)
  target_link_libraries(main PRIVATE nest_unrar)
endif()
