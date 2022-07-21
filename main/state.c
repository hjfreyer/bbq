
#include <esp_timer.h>
#include <stdlib.h>

#include "state.h"

Settings settings_create()
{
    Settings res = {
        .is_manual = false,
        .manual_duty_pct = 70,
        .automatic_duty_pct = 70,
        .threshold_f = CONFIG_INITIAL_THRESHOLD,
    };
    return res;
}

#define JSON_KEY(key) "\"" #key "\""



size_t state_json(uint64_t session_id, State *state, char **buf)
{
    return asprintf(buf, "{"
        JSON_KEY(probe_temps_f) ": [%f, %f],"
        JSON_KEY(duty_pct) ": %d,"
        JSON_KEY(uptime_usec) ": %lld,"
        JSON_KEY(session_id) ": %lld"
        "}",
        state->probe_temps_f[0],
                state->probe_temps_f[1],
                state->duty_pct,
                esp_timer_get_time(),
                session_id
    );
}