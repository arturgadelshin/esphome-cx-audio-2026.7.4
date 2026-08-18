from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
)
from esphome.const import CONF_ID

CODEOWNERS = ["@andreibodrov"]
DEPENDENCIES = ["esp32", "microphone", "speaker"]

cx_audio_ns = cg.esphome_ns.namespace("cx_audio")
CXAudio = cx_audio_ns.class_("CXAudio", cg.Component)

CONF_USE_FIRMWARE = "use_firmware"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CXAudio),
        cv.Optional(CONF_USE_FIRMWARE, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if config[CONF_USE_FIRMWARE]:
        cg.add(var.set_use_firmware(True))
        cg.add_build_flag("-DUSE_CX20921_FIRMWARE")

    # Board/product selection expected by the Synaptics SDK headers.
    cg.add_build_flag("-DCONFIG_SYNA_V1_2_BOARD")
    cg.add_build_flag("-DVOICE_ASSISTANT_AVS")

    # Ship the precompiled Synaptics SDK as an ESP-IDF component: it exports
    # the SDK headers to the whole project and links the closed archives with
    # --whole-archive plus the --wrap hooks implemented in va_patch.cpp.
    this_dir = Path(__file__).parent
    add_idf_component(name="cx_audio_sdk", path=str(this_dir / "idf" / "cx_audio_sdk"))

    # The legacy I2S/I2C drivers (driver/i2s.h, driver/i2c.h) and SPIFFS
    # (DSP firmware storage) are excluded from ESPHome's default IDF build.
    include_builtin_idf_component("driver")
    include_builtin_idf_component("spiffs")

    # The SDK uses the legacy I2S driver; don't drown the build log in its
    # deprecation warnings.
    add_idf_sdkconfig_option("CONFIG_I2S_SUPPRESS_DEPRECATE_WARN", True)
