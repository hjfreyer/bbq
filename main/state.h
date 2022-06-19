
#pragma once

struct State
{
    int16_t probe1_f;
    int16_t probe2_f;
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