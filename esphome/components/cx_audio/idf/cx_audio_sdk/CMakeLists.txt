# Precompiled Synaptics CX20921/CX22721 SDK for the LyraTD MCU board,
# shipped as an ESP-IDF component.
#
# libva_sdk_monolith.a merges three original archives:
#   - libva_dsp.a     -> 1 member (libva_dsp_va_dsp.o)          [needed]
#   - libcnx-ipc.a    -> 6 members (libcnx-ipc_*.o)            [needed]
#   - libavs (Alexa)  -> 80 members (libavs_*.c.obj)           [NOT needed]
#
# The AVS members define the same framework symbols (va_boot_dsp_signal,
# va_nvs_* ...) as our stubs in va_patch.cpp and pull in the whole Alexa
# stack (~2 MB). They are therefore stripped at CMake configure time by
# extracting only the 7 DSP/IPC members into a separate archive.
#
# The result is linked with --whole-archive: the members reference each
# other, so no single referenced symbol would pull them all in.
idf_component_register(
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../include"
    REQUIRES driver spiffs nvs_flash
)

set(_sdk_monolith "${CMAKE_CURRENT_LIST_DIR}/../../lib/libva_sdk_monolith.a")

set(_sdk_members
    libva_dsp_va_dsp.o
    libcnx-ipc_cnx20921_init.o
    libcnx-ipc_cx20921Interface.o
    libcnx-ipc_cx20921_i2c.o
    libcnx-ipc_CxFlash.o
    libcnx-ipc_cxdish.o
    libcnx-ipc_host_depend.o
)

set(_objs_dir "${CMAKE_CURRENT_BINARY_DIR}/sdk_objs")
set(_sdk_lib "${CMAKE_CURRENT_BINARY_DIR}/libcx_audio_sdk.a")

file(MAKE_DIRECTORY "${_objs_dir}")
set(_obj_files "")
foreach(_member IN LISTS _sdk_members)
    set(_obj "${_objs_dir}/${_member}")
    if(NOT EXISTS "${_obj}")
        execute_process(
            COMMAND ${CMAKE_AR} x --output "${_objs_dir}" "${_sdk_monolith}" "${_member}"
            RESULT_VARIABLE _ar_result
        )
        if(NOT _ar_result EQUAL 0)
            message(FATAL_ERROR "cx_audio_sdk: failed to extract ${_member} from ${_sdk_monolith}")
        endif()
    endif()
    list(APPEND _obj_files "${_obj}")
endforeach()

# Rebuild the archive from scratch on every configure run; `ar qc` would
# append duplicate members if the archive already exists.
file(REMOVE "${_sdk_lib}")
execute_process(
    COMMAND ${CMAKE_AR} qc "${_sdk_lib}" ${_obj_files}
    RESULT_VARIABLE _ar_result
)
if(NOT _ar_result EQUAL 0)
    message(FATAL_ERROR "cx_audio_sdk: failed to create ${_sdk_lib}")
endif()

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--whole-archive"
    "${_sdk_lib}"
    "-Wl,--no-whole-archive"
)

target_link_options(${COMPONENT_LIB} INTERFACE
    # Keep SDK entry points even when nothing in ESPHome references them.
    "-Wl,-u,va_board_init"
    "-Wl,-u,va_dsp_init"
    "-Wl,-u,cnx20921_init"
    "-Wl,-u,cnx20921_stream_audio"
    "-Wl,-u,cx20921SetMicGain"
    "-Wl,-u,va_boot_dsp_signal"
    "-Wl,-u,esp_vfs_spiffs_register"
    # Serialize all SDK I2C traffic through the ESPHome-owned mutex and feed
    # the task watchdog during long DSP firmware transfers (see va_patch.cpp).
    "-Wl,--wrap=i2c_master_cmd_begin"
    # Allow overriding the DSP firmware-flash mode from YAML (see va_patch.cpp).
    "-Wl,--wrap=cnx20921_init"
)
