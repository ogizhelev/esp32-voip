# ESPHome codegen for the `sip_voip` external component.
#
# Wraps Espressif's esp_rtc SIP/RTC engine (shipped inside the managed component
# `espressif/esp_media_protocols`) so an ESP32-A1S (ES8388) can do full-duplex G.711
# SIP with a continuously-clocked I2S bus. esp-idf framework only.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID

CODEOWNERS = ["@ogizhelev"]
DEPENDENCIES = ["esp32", "network"]

sip_voip_ns = cg.esphome_ns.namespace("sip_voip")
SipVoip = sip_voip_ns.class_("SipVoip", cg.Component)

CONF_SERVER = "server"
CONF_PORT = "port"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_DOMAIN = "domain"
CONF_CODEC = "codec"
CONF_AUTO_ANSWER = "auto_answer"

# rtc_payload_acodec_t: NULL=0, G711A=1, G711U=2, OPUS=3
CODECS = {"pcma": 1, "pcmu": 2}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SipVoip),
        cv.Required(CONF_SERVER): cv.string,
        cv.Optional(CONF_PORT, default=5060): cv.port,
        cv.Required(CONF_USERNAME): cv.string,
        cv.Required(CONF_PASSWORD): cv.string,
        # SIP domain / AOR host. Empty = use the server host. esp_rtc has NO separate
        # auth-username field: the digest auth username IS `username`, so the PBX
        # account's auth user must equal it. Set `domain` only if the registrar expects
        # a specific realm/domain instead of the server IP.
        cv.Optional(CONF_DOMAIN, default=""): cv.string,
        cv.Optional(CONF_CODEC, default="pcma"): cv.enum(CODECS, lower=True),
        cv.Optional(CONF_AUTO_ANSWER, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_server(config[CONF_SERVER]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_username(config[CONF_USERNAME]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_domain(config[CONF_DOMAIN]))
    cg.add(var.set_codec(config[CONF_CODEC]))
    cg.add(var.set_auto_answer(config[CONF_AUTO_ANSWER]))

    # Pull Espressif's SIP/RTC engine. esp_rtc ships (prebuilt lib + esp_rtc.h) inside
    # esp_media_protocols — ESPHome writes idf_component.yml and the IDF manager fetches it.
    esp32.add_idf_component(name="espressif/esp_media_protocols", ref="==1.0.3")

    # Our C++ uses the raw ESP-IDF drivers directly. ESPHome EXCLUDES these built-in IDF
    # components by default (to cut compile time), so re-include them to get the headers:
    #   esp_driver_i2c -> driver/i2c_master.h, esp_driver_i2s -> driver/i2s_std.h,
    #   esp_driver_gpio -> driver/gpio.h
    esp32.include_builtin_idf_component("esp_driver_i2c")
    esp32.include_builtin_idf_component("esp_driver_i2s")
    esp32.include_builtin_idf_component("esp_driver_gpio")

    # The engine spawns its own SIP/RTP tasks; give the main task some headroom.
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_MAIN_TASK_STACK_SIZE", 8192)
