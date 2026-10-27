cmake_minimum_required(VERSION 3.20.0)

if(NOT BOARD)
  set(BOARD frdm_mcxn236)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(POC_CANopen_SlaveSW)

set(CO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/CANopenNode)

# CANopenNode v4 stack sources (CiA 301 core + optional modules we may use)
file(GLOB CO_301_SOURCES ${CO_DIR}/301/*.c)
file(GLOB CO_303_SOURCES ${CO_DIR}/303/*.c)
file(GLOB CO_305_SOURCES ${CO_DIR}/305/*.c)
set(CO_STORAGE_SOURCES ${CO_DIR}/storage/CO_storage.c)

zephyr_compile_definitions(SYS_CONST_DEBUG=0)

target_sources(app PRIVATE
  ${CO_DIR}/CANopen.c
  ${CO_301_SOURCES}
  ${CO_303_SOURCES}
  ${CO_305_SOURCES}
  ${CO_STORAGE_SOURCES}

  src/main.c
  src/CO_driver.c
  src/CO_storageBlank.c
  src/OD.c
  src/app_uart.c
)

target_include_directories(app PRIVATE
  src              # first: our CO_driver.h and OD.h
  src/APP
  ${CO_DIR}
)