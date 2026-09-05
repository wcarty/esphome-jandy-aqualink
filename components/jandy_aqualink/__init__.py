import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

AUTO_LOAD = ["sensor", "number", "binary_sensor", "text_sensor"]

jandy_ns = cg.esphome_ns.namespace("jandy_aqualink")
JandyAqualink = jandy_ns.class_("JandyAqualink", cg.Component)

CONF_TX_PIN = "tx_pin"
CONF_RX_PIN = "rx_pin"
CONF_BAUD = "baud_rate"
CONF_KEYPAD_ADDRESS = "keypad_address"
CONF_POLLS_ANSWERED = "polls_answered"
CONF_REPLY_LATENCY = "reply_latency"
CONF_CHECKSUM_ERRORS = "checksum_errors"
CONF_AIR_TEMP = "air_temp"
CONF_POOL_TEMP = "pool_temp"
CONF_SPA_TEMP = "spa_temp"
CONF_PUMP_RPM = "pump_rpm"
CONF_PUMP_WATTS = "pump_watts"
CONF_SALT_LEVEL = "salt_level"
CONF_SALT_CHLORINATOR_OUTPUT = "salt_chlorinator_output"
CONF_SALT_CHLORINATOR_STATUS = "salt_chlorinator_status"
CONF_SALT_CHLORINATOR_GENERATING = "salt_chlorinator_generating"
CONF_PH = "ph"
CONF_ORP = "orp"
CONF_SPA_MODE = "spa_mode"
CONF_AIR_BLOWER = "air_blower"
CONF_FILTER_PUMP_STATE = "filter_pump_state"
CONF_CLEANER_STATE = "cleaner_state"
CONF_POOL_HEAT_ENABLED = "pool_heat_enabled"
CONF_SPA_HEAT_ENABLED = "spa_heat_enabled"


def _temp_sensor():
    return sensor.sensor_schema(
        unit_of_measurement="°F",
        accuracy_decimals=0,
        device_class="temperature",
        state_class=STATE_CLASS_MEASUREMENT,
    )

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(JandyAqualink),
        cv.Optional(CONF_TX_PIN, default=19): cv.int_,
        cv.Optional(CONF_RX_PIN, default=22): cv.int_,
        cv.Optional(CONF_BAUD, default=9600): cv.positive_int,
        cv.Optional(CONF_KEYPAD_ADDRESS, default=0x08): cv.int_range(min=0, max=255),
        cv.Optional(CONF_POLLS_ANSWERED): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:check-network-outline",
        ),
        cv.Optional(CONF_REPLY_LATENCY): sensor.sensor_schema(
            unit_of_measurement="µs",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:timer-outline",
        ),
        cv.Optional(CONF_CHECKSUM_ERRORS): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-circle-outline",
        ),
        cv.Optional(CONF_AIR_TEMP): _temp_sensor(),
        cv.Optional(CONF_POOL_TEMP): _temp_sensor(),
        cv.Optional(CONF_SPA_TEMP): _temp_sensor(),
        cv.Optional(CONF_PUMP_RPM): sensor.sensor_schema(
            unit_of_measurement="RPM",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:pump",
        ),
        cv.Optional(CONF_PUMP_WATTS): sensor.sensor_schema(
            unit_of_measurement="W",
            accuracy_decimals=0,
            device_class="power",
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:flash",
        ),
        cv.Optional(CONF_SALT_LEVEL): sensor.sensor_schema(
            unit_of_measurement="ppm",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:shaker-outline",
        ),
        cv.Optional(CONF_SALT_CHLORINATOR_OUTPUT): sensor.sensor_schema(
            unit_of_measurement="%",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:percent",
        ),
        cv.Optional(CONF_SALT_CHLORINATOR_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:water-check",
        ),
        cv.Optional(CONF_SALT_CHLORINATOR_GENERATING): binary_sensor.binary_sensor_schema(
            icon="mdi:water-check",
        ),
        cv.Optional(CONF_PH): sensor.sensor_schema(
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:ph",
        ),
        cv.Optional(CONF_ORP): sensor.sensor_schema(
            unit_of_measurement="mV",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:flash",
        ),
        cv.Optional(CONF_SPA_MODE): binary_sensor.binary_sensor_schema(
            icon="mdi:hot-tub",
        ),
        cv.Optional(CONF_AIR_BLOWER): binary_sensor.binary_sensor_schema(
            icon="mdi:weather-windy",
        ),
        cv.Optional(CONF_FILTER_PUMP_STATE): binary_sensor.binary_sensor_schema(
            device_class="running",
            icon="mdi:pump",
        ),
        cv.Optional(CONF_CLEANER_STATE): binary_sensor.binary_sensor_schema(
            device_class="running",
            icon="mdi:robot-vacuum",
        ),
        cv.Optional(CONF_POOL_HEAT_ENABLED): binary_sensor.binary_sensor_schema(
            icon="mdi:radiator",
        ),
        cv.Optional(CONF_SPA_HEAT_ENABLED): binary_sensor.binary_sensor_schema(
            icon="mdi:fire",
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_tx_pin(config[CONF_TX_PIN]))
    cg.add(var.set_rx_pin(config[CONF_RX_PIN]))
    cg.add(var.set_baud(config[CONF_BAUD]))
    cg.add(var.set_keypad_address(config[CONF_KEYPAD_ADDRESS]))

    if CONF_POLLS_ANSWERED in config:
        s = await sensor.new_sensor(config[CONF_POLLS_ANSWERED])
        cg.add(var.set_polls_sensor(s))
    if CONF_REPLY_LATENCY in config:
        s = await sensor.new_sensor(config[CONF_REPLY_LATENCY])
        cg.add(var.set_latency_sensor(s))
    if CONF_CHECKSUM_ERRORS in config:
        s = await sensor.new_sensor(config[CONF_CHECKSUM_ERRORS])
        cg.add(var.set_errors_sensor(s))
    if CONF_AIR_TEMP in config:
        s = await sensor.new_sensor(config[CONF_AIR_TEMP])
        cg.add(var.set_air_temp_sensor(s))
    if CONF_POOL_TEMP in config:
        s = await sensor.new_sensor(config[CONF_POOL_TEMP])
        cg.add(var.set_pool_temp_sensor(s))
    if CONF_SPA_TEMP in config:
        s = await sensor.new_sensor(config[CONF_SPA_TEMP])
        cg.add(var.set_spa_temp_sensor(s))
    if CONF_PUMP_RPM in config:
        s = await sensor.new_sensor(config[CONF_PUMP_RPM])
        cg.add(var.set_pump_rpm_sensor(s))
    if CONF_PUMP_WATTS in config:
        s = await sensor.new_sensor(config[CONF_PUMP_WATTS])
        cg.add(var.set_pump_watts_sensor(s))
    if CONF_SALT_LEVEL in config:
        s = await sensor.new_sensor(config[CONF_SALT_LEVEL])
        cg.add(var.set_salt_level_sensor(s))
    if CONF_SALT_CHLORINATOR_OUTPUT in config:
        s = await sensor.new_sensor(config[CONF_SALT_CHLORINATOR_OUTPUT])
        cg.add(var.set_salt_chlorinator_output_sensor(s))
    if CONF_SALT_CHLORINATOR_STATUS in config:
        s = await text_sensor.new_text_sensor(config[CONF_SALT_CHLORINATOR_STATUS])
        cg.add(var.set_salt_chlorinator_status_ts(s))
    if CONF_SALT_CHLORINATOR_GENERATING in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_SALT_CHLORINATOR_GENERATING])
        cg.add(var.set_salt_chlorinator_generating_bs(b))
    if CONF_PH in config:
        s = await sensor.new_sensor(config[CONF_PH])
        cg.add(var.set_ph_sensor(s))
    if CONF_ORP in config:
        s = await sensor.new_sensor(config[CONF_ORP])
        cg.add(var.set_orp_sensor(s))
    if CONF_SPA_MODE in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_SPA_MODE])
        cg.add(var.set_spa_mode_bs(b))
    if CONF_AIR_BLOWER in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_AIR_BLOWER])
        cg.add(var.set_air_blower_bs(b))
    if CONF_FILTER_PUMP_STATE in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_FILTER_PUMP_STATE])
        cg.add(var.set_filter_pump_bs(b))
    if CONF_CLEANER_STATE in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_CLEANER_STATE])
        cg.add(var.set_cleaner_bs(b))
    if CONF_POOL_HEAT_ENABLED in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_POOL_HEAT_ENABLED])
        cg.add(var.set_pool_heat_bs(b))
    if CONF_SPA_HEAT_ENABLED in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_SPA_HEAT_ENABLED])
        cg.add(var.set_spa_heat_bs(b))
