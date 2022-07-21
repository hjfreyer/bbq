#ifndef BBQ_MAIN_STATE_H
#define BBQ_MAIN_STATE_H	1

#include "state.h"

typedef struct Readings
{
    uint32_t reference_voltage_mv;
    uint32_t probe_voltage_mv[2];
} Readings;

typedef struct RawState
{
    uint32_t reference_voltage_mv[CONFIG_TEMP_BUFFER_LEN];
    uint32_t probe_voltage_mv[2][CONFIG_TEMP_BUFFER_LEN];
} RawState;

typedef struct State
{
    double probe_temps_f[2];
    uint8_t duty_pct;
} State;

typedef struct Settings
{
    uint8_t is_manual;
    uint8_t manual_duty_pct;
    uint8_t automatic_duty_pct;
    int16_t threshold_f;
} Settings;

Settings settings_create();

size_t state_json(uint64_t session_id, State *state, char **buf);
#endif
