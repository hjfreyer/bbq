
#pragma once

struct Readings {
    uint32_t reference_voltage_mv;
    uint32_t probe_voltage_mv[2];
};

struct RawState
{
    uint32_t reference_voltage_mv[CONFIG_TEMP_BUFFER_LEN];
    uint32_t probe_voltage_mv[2][CONFIG_TEMP_BUFFER_LEN];
};

struct State
{
    double probe_temps_f[2];
    uint8_t duty_pct;
};

struct Settings
{
    uint8_t is_manual;
    uint8_t manual_duty_pct;
    uint8_t automatic_duty_pct;
    int16_t threshold_f;
};

struct Settings settings_create()
{
    struct Settings res = {
        .is_manual = false,
        .manual_duty_pct = 70,
        .automatic_duty_pct = 70,
        .threshold_f = CONFIG_INITIAL_THRESHOLD,
    };
    return res;
}