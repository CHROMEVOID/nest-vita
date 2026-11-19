cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(Nest C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Nest")
set(VITA_TITLEID  "NEST00001")
set(VITA_VERSION  "01.11")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-format-truncation -Wno-stringop-truncation -fno-strict-aliasing")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED freetype2 libpng libwebp zlib)

file(GLOB NEST_SRC src/*.c)
if(NOT DEFINED NEST_PAD)
  set(NEST_PAD 1)
endif()
set_source_files_properties(src/pad.c PROPERTIES COMPILE_DEFINITIONS "NEST_PAD=${NEST_PAD}")
add_executable(Nest ${NEST_SRC} third_party/sqlite/sqlite3.c)

set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_FLAGS
  "-w -O2 -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_SHARED_CACHE")

target_include_directories(Nest PRIVATE src third_party third_party/sqlite ${DEPS_INCLUDE_DIRS})

target_link_libraries(Nest
  vita2d
  ${DEPS_STATIC_LIBRARIES}
  jpeg
  unrar
  stdc++
  pthread
  m
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceAudio_stub
  SceAvPlayer_stub
  SceRtc_stub
  ScePower_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
)

vita_create_self(eboot.bin Nest)

file(GLOB_RECURSE ASSET_FILES RELATIVE ${CMAKE_SOURCE_DIR} assets/*.ttf assets/*.nla assets/*.jpg)
set(ASSET_ARGS "")
foreach(f ${ASSET_FILES})
  list(APPEND ASSET_ARGS FILE ${CMAKE_SOURCE_DIR}/${f} ${f})
endforeach()

vita_create_vpk(Nest.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/pic0.png sce_sys/pic0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${ASSET_ARGS}
)
