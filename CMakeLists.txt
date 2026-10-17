cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Нужен vitasdk: задай переменную VITASDK")
  endif()
endif()

project(vitamidi C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaMIDI Test")
set(VITA_TITLEID  "VMID00001")
set(VITA_VERSION  "00.10")

# ---------- kernel-плагин ----------

add_executable(vitamidi_kernel
  kernel/main.c
  common/umidi.c
)
target_include_directories(vitamidi_kernel PRIVATE include common)
target_compile_options(vitamidi_kernel PRIVATE -std=gnu99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(vitamidi_kernel
  SceDebugForDriver_stub
  SceSblAIMgrForDriver_stub
  SceKernelSuspendForDriver_stub
  SceProcEventForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceThreadmgrForDriver_stub
  SceUsbdForDriver_stub
  SceUsbServForDriver_stub
  gcc
)
set_target_properties(vitamidi_kernel PROPERTIES LINK_FLAGS "-nostdlib")
vita_create_self(vitamidi.skprx vitamidi_kernel CONFIG ${CMAKE_SOURCE_DIR}/kernel/exports.yml UNSAFE)
vita_create_stubs(vitamidi_stubs vitamidi_kernel ${CMAKE_SOURCE_DIR}/kernel/exports.yml KERNEL)

# ---------- тестовое приложение ----------

add_executable(vitamidi_test app/main.c)
target_include_directories(vitamidi_test PRIVATE include)
target_compile_options(vitamidi_test PRIVATE -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter)
add_dependencies(vitamidi_test vitamidi_stubs)
target_link_directories(vitamidi_test PRIVATE ${CMAKE_BINARY_DIR}/vitamidi_stubs ${CMAKE_BINARY_DIR})
target_link_libraries(vitamidi_test
  VitaMidi_stub_weak
  vita2d
  png
  jpeg
  z
  m
  c
  SceAudio_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceLibKernel_stub
  ScePgf_stub
  ScePower_stub
  SceSysmodule_stub
  SceKernelThreadMgr_stub
)
# vita_create_self — макрос: флаги от плагина (CONFIG exports.yml) сбрасываем
set(VITA_ELF_CREATE_FLAGS "")
set(VITA_MAKE_FSELF_FLAGS "")
vita_create_self(vitamidi_test.self vitamidi_test)
vita_create_vpk(vitamidi_test.vpk ${VITA_TITLEID} vitamidi_test.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)

# ---------- контроллер: Vita как USB-MIDI для Ableton ----------
# Плагин psvita-usb-audio-midi собирается отдельно (см. workflow) в build-usb.
set(USBMIDI_DIR ${CMAKE_SOURCE_DIR}/third_party/psvita-usb-audio-midi)
set(USBMIDI_BUILD ${CMAKE_SOURCE_DIR}/build-usb CACHE PATH "сборка psvita-usb-audio-midi")
if(EXISTS ${USBMIDI_BUILD}/psvita_usb_audio_midi_client.suprx)
  add_executable(vitamidi_controller
    controller/main.c
    ${USBMIDI_DIR}/src/client/loader.c
  )
  target_include_directories(vitamidi_controller PRIVATE ${USBMIDI_DIR}/include)
  target_compile_options(vitamidi_controller PRIVATE -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter)
  target_link_directories(vitamidi_controller PRIVATE ${USBMIDI_BUILD})
  target_link_libraries(vitamidi_controller
    psvita_usb_audio_midi_client_stub_weak
    vita2d
    png
    jpeg
    z
    m
    c
    SceAudio_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceCommonDialog_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    SceLibKernel_stub
    SceKernelModulemgr_stub
    ScePgf_stub
    ScePower_stub
    SceSysmodule_stub
    SceTouch_stub
    SceKernelThreadMgr_stub
  )
  set(VITA_ELF_CREATE_FLAGS "")
  set(VITA_MAKE_FSELF_FLAGS "")
  vita_create_self(vitamidi_controller.self vitamidi_controller)
  vita_create_vpk(vitamidi_controller.vpk VMID00002 vitamidi_controller.self
    VERSION 00.10
    NAME "VitaMIDI Controller"
    FILE ${USBMIDI_BUILD}/psvita_usb_audio_midi_client.suprx psvita_usb_audio_midi_client.suprx
  )
else()
  message(STATUS "build-usb не найден: контроллер не собирается")
endif()
