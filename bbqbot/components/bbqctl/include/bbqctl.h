
#ifndef BBQCTL_BBQCTL_H
#define BBQCTL_BBQCTL_H

#include <stdint.h>

namespace bbqctl {

struct Config {
  double steinhart_coeff0;
  double steinhart_coeff1;
  double steinhart_coeff2;

  uint32_t probe_resistor_ohms;
};

struct Settings {
  bool lid_mode;
  uint8_t is_manual;
  uint8_t manual_duty_pct;
  uint8_t automatic_duty_pct;
  int16_t threshold_f;
  int16_t bang_bang_window;
};

struct Output {
    double ambient_temp_f;
    double food_temp_f;
    uint8_t duty_pct;
};

// class Device {
// public:
//     virtual uint32_t ReadReferenceVoltage() = 0;
//     virtual uint32_t ReadAmbientVoltage() = 0;
//     virtual uint32_t ReadFoodVoltage() = 0;
// };

namespace internal {

#define TEMP_BUFFER_LEN 64
struct State1 {
  uint8_t measurement_count;
  uint32_t reference_voltage_mv[TEMP_BUFFER_LEN];
  uint32_t probe_voltage_mv[2][TEMP_BUFFER_LEN];
};

struct State2 {
  double probe_temps_f[2];
  uint8_t duty_pct;
};
}

typedef uint32_t voltage_t;

class Controller {
 public:
  explicit Controller(Config config) : config_(config) {}

  void ProvideReadings(voltage_t ref, voltage_t ambient, voltage_t food);

    Output GetOutput();

 private:
  Config config_;
Settings settings_;

  internal::State1 raw_;
  internal::State2 higher_;

};

}  // namespace bbqctl

#endif /* BBQCTL_BBQCTL_H */
