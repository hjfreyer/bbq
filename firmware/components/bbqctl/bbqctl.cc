
#include "bbqctl.h"

#include <math.h>

namespace bbqctl {

#define TEMP_BUFFER_LEN 64

void update_state1(internal::State1* state, voltage_t ref, voltage_t ambient,
                   voltage_t food) {
  state->measurement_count = (state->measurement_count + 1) % TEMP_BUFFER_LEN;
  state->reference_voltage_mv[state->measurement_count] = ref;
  state->probe_voltage_mv[0][state->measurement_count] = ambient;
  state->probe_voltage_mv[1][state->measurement_count] = food;
}

static double probe_temp_f(const Config& config, double probe_mv,
                           double ref_mv) {
  // V = Vref * 2

  // V = Vprobe + Vresistor
  // Vresistor = V - VProbe
  // Vprobe / Rprobe = Vresistor / Rresistor
  // Rprobe = Vprobe * Rresistor / Vresistor
  // Rprobe = Vprobe * PROBE_RESISTOR_OHMS / (Vref*2 - Vprobe)
  double r_probe_ohm =
      probe_mv * config.probe_resistor_ohms / (ref_mv * 2 - probe_mv);

  // Fix up some wacky edge cases that can cause NaNs and Infs.
  // if (r_probe_ohm < 1) {
  //   r_probe_ohm = 10000000;
  // }
  // r_probe_ohm = min(r_probe_ohm, 10000000.0);
  // r_probe_ohm = max(r_probe_ohm, 10.0);

  double log_r = log(r_probe_ohm);
  double temp_k =
      1.0 / (config.steinhart_coeff0 + config.steinhart_coeff1 * log_r +
             config.steinhart_coeff2 * log_r * log_r * log_r);
  double temp_f = (temp_k - 273.15) * 9 / 5 + 32;
  if (isfinite(temp_f)) {
    return temp_f;
  } else {
    return 0.0;
  }
}

void update_state2(const Config& config, internal::State2* out,
                   internal::State1* raw, Settings* settings) {
  double avg_ref_voltage_mv = 0;
  double avg_probe_voltage_mv[2] = {0, 0};

  for (int i = 0; i < TEMP_BUFFER_LEN; i++) {
    avg_ref_voltage_mv += raw->reference_voltage_mv[i];
    avg_probe_voltage_mv[0] += raw->probe_voltage_mv[0][i];
    avg_probe_voltage_mv[1] += raw->probe_voltage_mv[1][i];
  }

  avg_ref_voltage_mv /= TEMP_BUFFER_LEN;
  avg_probe_voltage_mv[0] /= TEMP_BUFFER_LEN;
  avg_probe_voltage_mv[1] /= TEMP_BUFFER_LEN;

  out->probe_temps_f[0] =
      probe_temp_f(config, avg_probe_voltage_mv[0], avg_ref_voltage_mv);
  out->probe_temps_f[1] =
      probe_temp_f(config, avg_probe_voltage_mv[1], avg_ref_voltage_mv);

  if (settings->is_manual) {
    out->duty_pct = settings->manual_duty_pct;
  } else if (settings->lid_mode) {
    out->duty_pct = 0;
  } else if (out->probe_temps_f[0] <
             settings->threshold_f - settings->bang_bang_window) {
    // if the temp is too low, turn on the fan.
    out->duty_pct = settings->automatic_duty_pct;
  } else if (settings->threshold_f + settings->bang_bang_window <
             out->probe_temps_f[0]) {
    // if the temp is too high, turn off the fan.
    out->duty_pct = 0;
  } else {
    // Else, leave the fan alone.
  }
}

void Controller::ProvideReadings(voltage_t ref, voltage_t ambient,
                                 voltage_t food) {
  update_state1(&raw_, ref, ambient, food);
}

Output Controller::GetOutput() {
  internal::State2 state2;
  update_state2(config_, &state2, &raw_, &settings_);

  return Output{
      .ambient_temp_f = state2.probe_temps_f[0],
      .food_temp_f = state2.probe_temps_f[1],
      .duty_pct = state2.duty_pct,
  };
}

}  // namespace bbqctl