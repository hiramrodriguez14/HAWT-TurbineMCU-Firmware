#include "ti/devices/msp/m0p/mspm0g350x.h"
#include "ti_msp_dl_config.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdalign.h>

#define UNHANDLED_INTERRUPT()                                                  \
  do {                                                                         \
    __disable_irq();                                                           \
    __BKPT(0);                                                                 \
    while (1) {                                                                \
    }                                                                          \
  } while (0)

volatile bool global_critical_condition = false; // should be true for production
volatile bool global_wind_adc_start = false;
volatile bool global_adjust_pitch = false;
volatile bool global_send_uart = false;
volatile bool global_wind_adc_done = false;
volatile bool global_hall_effect_capture = false;
volatile bool global_hall_effect_timeout = false;
volatile bool global_reset_timeout = false;
volatile bool global_uart_debug_eot = false;
volatile bool global_uart_debug_dma = false;
volatile bool global_uart_load_eot = false;
volatile bool global_uart_load_dma = false;

#define EVERY_SECOND_DELTA_TIME_SECONDS 1.f // secs
void EVERY_SECOND_INST_IRQHandler(void) {
  switch (DL_Timer_getPendingInterrupt(EVERY_SECOND_INST)) {
  case DL_TIMER_IIDX_ZERO:
    global_wind_adc_start = true;
    global_adjust_pitch = true;
    global_send_uart = true;
    break;
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

void WIND_ADC12_INST_IRQHandler(void) {
  switch (DL_ADC12_getPendingInterrupt(WIND_ADC12_INST)) {
  case DL_ADC12_IIDX_MEM1_RESULT_LOADED:
    global_wind_adc_done = true;
    break;
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

void HALL_EFFECT_CAPTURE_INST_IRQHandler(void) {
  switch (DL_Timer_getPendingInterrupt(HALL_EFFECT_CAPTURE_INST)) {
  case DL_TIMER_IIDX_CC0_DN:
    global_hall_effect_capture = true;
    break;
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

void HALL_EFFECT_TIMEOUT_INST_IRQHandler(void) {
  switch (DL_Timer_getPendingInterrupt(HALL_EFFECT_TIMEOUT_INST)) {
  case DL_TIMER_IIDX_ZERO:
    global_hall_effect_timeout = true;
    break;
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

void RESET_TIMEOUT_INST_IRQHandler(void) {
  switch (DL_Timer_getPendingInterrupt(RESET_TIMEOUT_INST)) {
  case DL_TIMER_IIDX_ZERO:
    global_reset_timeout = true;
    break;
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

void UART_DEBUG_INST_IRQHandler(void) {
  switch (DL_UART_getPendingInterrupt(UART_DEBUG_INST)) {
  case DL_UART_IIDX_EOT_DONE:
    global_uart_debug_eot = true;
    break;
  case DL_UART_IIDX_DMA_DONE_TX:
    global_uart_debug_dma = true;
    break;
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

void UART_LOAD_INST_IRQHandler(void) {
  switch (DL_UART_getPendingInterrupt(UART_LOAD_INST)) {
  case DL_UART_IIDX_EOT_DONE:
    global_uart_load_eot = true;
    break;
  case DL_UART_IIDX_DMA_DONE_TX:
    global_uart_load_dma = true;
    break;
  case DL_UART_IIDX_RX: {
    uint8_t code = DL_UART_Main_receiveData(UART_LOAD_INST);
    global_critical_condition = (bool)(code & 0x01);
    break;
  }
  default:
    UNHANDLED_INTERRUPT();
    break;
  }
}

typedef enum {
  STATE_INIT,
  STATE_MAXIMIZE,
  STATE_RATED,
  STATE_DURABILITY,
  STATE_RESTART,
  STATE_SAFETY,
} State;

const char *state_string_lut[] = {
  [STATE_INIT] = "STATE_INIT",
  [STATE_MAXIMIZE] = "STATE_MAXIMIZE",
  [STATE_RATED] = "STATE_RATED",
  [STATE_DURABILITY] = "STATE_DURABILITY",
  [STATE_RESTART] = "STATE_RESTART",
  [STATE_SAFETY] = "STATE_SAFETY",
};

State global_state = STATE_INIT;

void uart_debug_write(const uint8_t *data, uint16_t size) {
  DL_DMA_setSrcAddr(DMA, DMA_CH0_CHAN_ID, (uint32_t)(data));
  DL_DMA_setDestAddr(DMA, DMA_CH0_CHAN_ID,
                     (uint32_t)(&UART_DEBUG_INST->TXDATA));
  DL_DMA_setTransferSize(DMA, DMA_CH0_CHAN_ID, size);

  DL_SYSCTL_disableSleepOnExit();

  DL_DMA_enableChannel(DMA, DMA_CH0_CHAN_ID);

  while (!global_uart_debug_dma) {
    __WFE();
  }

  while (!global_uart_debug_eot) {
    __WFE();
  }

  global_uart_debug_eot = false;
  global_uart_debug_dma = false;
}

void uart_debug_printf(const char *fmt, ...) {
  char buf[256];

  va_list args;
  va_start(args, fmt);

  int len = vsnprintf(buf, sizeof(buf), fmt, args);

  va_end(args);

  if (len < 0) {
    return;
  }

  if (len >= (int)sizeof(buf)) {
    len = sizeof(buf) - 1;
  }

  uart_debug_write((const uint8_t *)buf, (uint16_t)len);
}

void uart_load_write(const uint8_t *data, uint16_t size) {
  DL_DMA_setSrcAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t)(data));
  DL_DMA_setDestAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t)(&UART_LOAD_INST->TXDATA));
  DL_DMA_setTransferSize(DMA, DMA_CH1_CHAN_ID, size);

  DL_SYSCTL_disableSleepOnExit();

  DL_DMA_enableChannel(DMA, DMA_CH1_CHAN_ID);

  while (!global_uart_load_dma) {
    __WFE();
  }

  while (!global_uart_load_eot) {
    __WFE();
  }

  global_uart_load_eot = false;
  global_uart_load_dma = false;
}

#define ADC_MAX_COUNT (4095.0f)
#define ADC_REF_VOLTAGE (3.3f)

static float adc_to_voltage(uint16_t adc_code) {
  return ((float)adc_code * ADC_REF_VOLTAGE) / ADC_MAX_COUNT;
}

void getSensorVoltages(float *arr) {

  // Getting values from Registers
  uint16_t tmp_raw =
      (uint16_t)DL_ADC12_getMemResult(WIND_ADC12_INST, DL_ADC12_MEM_IDX_0);
  uint16_t rv_raw =
      (uint16_t)DL_ADC12_getMemResult(WIND_ADC12_INST, DL_ADC12_MEM_IDX_1);

  // Convert to Voltage
  float tmp1_Volts = adc_to_voltage(tmp_raw);
  float rv1_Volts = adc_to_voltage(rv_raw);

  // Convert to Voltage before voltage Divider  3/2 = 1.5
  float temp_sensor_tmp = tmp1_Volts * 1.45;
  float temp_sensor_rv = rv1_Volts * 1.45;
  arr[0] = temp_sensor_tmp;
  arr[1] = temp_sensor_rv;
}

volatile float global_windspeed_calibration = 0.27f;
// volatile float global_windspeed_calibration = 0.446033058758f;
float calculateMetersPerSecond(float *arr) {
  const float zeroWindAdjustment =
      .2; // Have to change according to documentation
  float tmp_volts = arr[0];
  float rv_volts = arr[1];

  // Convert real volts into equivalent values of ADC output of Arduino, in
  // order to use formulas provided by documentation
  float tmp = (tmp_volts / 5.0f) * 1024.0f;
  float rv = (rv_volts / 5.0f) * 1024.0f;

  float RV_Wind_Volts = (rv * 0.0048828125f);

  int TempCtimes100 = (0.005f * tmp * tmp) - (16.862f * tmp) + 9075.4f;

  float zeroWind_ADunits = -0.0006f * tmp * tmp + 1.0727f * tmp + 47.172f;

  float zeroWind_volts =
      (zeroWind_ADunits * 0.0048828125f) - zeroWindAdjustment;

  // return zeroWind_volts;
  float base = (RV_Wind_Volts - zeroWind_volts) / 0.2300f;

  if (base <= 0.0f) {
    return 0.0f;
  }

  return global_windspeed_calibration * 0.44704 * powf(base, 2.7265f);
}

#define PULSES_PER_REV (1)

float global_rpm = 0;

void pending_hall_effect(void) {
  static uint32_t last_capture = 0;
  static bool first_sample = true;

  if (global_hall_effect_timeout) {
    global_hall_effect_timeout = false;
    first_sample = true;
    global_rpm = 0.f;

    // uart_debug_printf("rpm = %f\r\n", global_rpm);
  }

  if (global_hall_effect_capture) {
    global_hall_effect_capture = false;

    uint32_t capture = DL_Timer_getCaptureCompareValue(HALL_EFFECT_CAPTURE_INST,
                                                       DL_TIMER_CC_0_INDEX);

    if (first_sample) {
      last_capture = capture;
      global_rpm = 0.f;
      first_sample = false;

      // uart_debug_printf("rpm = %f\r\n", global_rpm);
      return;
    }

    uint32_t diff;

    // timer counts DOWN
    if (capture <= last_capture)
      diff = last_capture - capture;
    else
      diff = last_capture + (HALL_EFFECT_CAPTURE_INST_LOAD_VALUE - capture);

    last_capture = capture;

    if (diff > 0) {

      static const float rpm_factor = (60.f * (32768.f / 8.f)) / PULSES_PER_REV;

      global_rpm = rpm_factor / diff;

      // restart timeout timer
      HALL_EFFECT_TIMEOUT_INST->COUNTERREGS.CTRCTL &= ~0x1;
      HALL_EFFECT_TIMEOUT_INST->COUNTERREGS.CTR = 0;
      HALL_EFFECT_TIMEOUT_INST->COUNTERREGS.LOAD =
          HALL_EFFECT_TIMEOUT_INST_LOAD_VALUE;
      HALL_EFFECT_TIMEOUT_INST->COUNTERREGS.CTRCTL |= 0x1;

      // uart_debug_printf("rpm = %f\r\n", global_rpm);
    }
  }
}

float global_windspeed_m_s;
void pending_sensors(void) {
  pending_hall_effect();

  // TODO: feels weird
  if (global_wind_adc_start) {
    global_wind_adc_start = false;

    DL_ADC12_startConversion(WIND_ADC12_INST);
  }

  if (global_wind_adc_done) {
    global_wind_adc_done = false;

    // Getting Raw Voltages, voltages after divider, voltages before dividers
    float sensor_voltages[2];
    getSensorVoltages(sensor_voltages);

    // float sensor_tmp = sensor_voltages[0];
    // float sensor_rv = sensor_voltages[1];
    // uart_debug_printf("TMP: %.4f\t RV: %.4f\r\r\n", sensor_tmp, sensor_rv);

    global_windspeed_m_s = calculateMetersPerSecond(sensor_voltages);
    // uart_debug_printf("Wind Speed(m/s): %.4f\r\r\n", global_windspeed_m_s);

    // Enable ADC Conversion for next iteration
    DL_ADC12_enableConversions(WIND_ADC12_INST);
  }
}

#define UART_DEBUG
#define UART_DEBUG_EXTRA

float global_blade_deg = 0.0f;
void pending_uart(void) {
  typedef struct __attribute__((packed)) {
    DL_RTC_Common_Calendar calendar;
    float hall_effect_rpm, wind_speed_m_s, blade_deg;
    State state;
  } TelemetryPacket;
  typedef struct __attribute__((packed)) {
    uint8_t start_of_frame; // 0xAA
    uint8_t length; // sizeof(payload)
    uint8_t _padding[1];
    TelemetryPacket payload;
    // uint32_t crc;
  } Frame;
  // [ 0 1 2 3 4 5 6 7 8 ... 32 26 27 28 29]
  //   s l _ <.....payload....> < ..crc...>

  if (global_send_uart) {
    global_send_uart = false;

    TelemetryPacket packet = {
        .calendar = DL_RTC_Common_getCalendarTime(RTC),
        .hall_effect_rpm = global_rpm,
        .wind_speed_m_s = global_windspeed_m_s,
        .blade_deg = global_blade_deg,
        .state = global_state,
    };
    Frame frame = {
      .start_of_frame = 0xAA,
      .length = sizeof(TelemetryPacket),
      .payload = packet,
      // .crc = 0, // set latter
    };
    // #define CRC_OFFSET offsetof(Frame, crc)
    // static_assert(CRC_OFFSET % sizeof(uint32_t) == 0, "DL_CRC_calculateBlock32 requeries align of 4");
    // frame.crc = DL_CRC_calculateBlock32(CRC, CRC_SEED, (uint32_t *)&frame, CRC_OFFSET); // blocking

#ifdef UART_DEBUG
    uart_debug_printf("%04d-%02d-%02dT%02d:%02d:%02d", packet.calendar.year,
                packet.calendar.month, packet.calendar.dayOfMonth,
                packet.calendar.hours, packet.calendar.minutes,
                packet.calendar.seconds);
    uart_debug_printf(", hall_effect_rpm = %f", packet.hall_effect_rpm);
    uart_debug_printf(", wind_speed_m_s = %f", packet.wind_speed_m_s);
    uart_debug_printf(", blade_deg = %2.0f", global_blade_deg);
    uart_debug_printf(", state = %s", state_string_lut[global_state]);
    uart_debug_printf("\r\n");
#endif // UART_DEBUG
    uart_load_write((uint8_t *)&frame, sizeof(Frame));
  }
}

// Servo
//     ↓
// Pinion gear
//     ↓
// Rack
//     ↓
// Sliding ring/collar (prismatic slider)
//     ↓
// Pitch links
//     ↓
// Blade root crank arms
//     ↓
// Blade pitch rotation

#define SERVO_CC_MIN 39000.0f
#define SERVO_CC_MAX 37000.0f

#define SERVO_DEG_MIN 0.0f
#define SERVO_DEG_MAX 67.0f

#define BLADE_PITCH_DEG_MIN 0.0f
#define BLADE_PITCH_DEG_MAX 67.0f

// #define BLADE_PITCH_DEG_POWER     15.0f
// #define BLADE_PITCH_DEG_POWER     40.0f
// #define BLADE_PITCH_DEG_FEATHERED BLADE_PITCH_DEG_MIN

#define BLADE_PITCH_DEG_POWER BLADE_PITCH_DEG_MIN
#define BLADE_PITCH_DEG_FEATHERED 50.0f

static inline float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

static inline float inv_lerp(float a, float b, float v) {
  return (v - a) / (b - a);
}

static inline float clampf(float x, float min, float max) {
  if (x < min) return min;
  if (x > max) return max;
  return x;
}

/* servo compare counts -> servo angle */
float servo_deg_from_cc(float cc) {
  float t = inv_lerp(SERVO_CC_MIN, SERVO_CC_MAX, cc);
  t = clampf(t, 0.f, 1.f);

  return lerp(SERVO_DEG_MIN, SERVO_DEG_MAX, t);
}

/* servo angle -> compare counts */
float cc_from_servo_deg(float deg) {
  float t = inv_lerp(SERVO_DEG_MIN, SERVO_DEG_MAX, deg);
  t = clampf(t, 0.f, 1.f);

  return lerp(SERVO_CC_MIN, SERVO_CC_MAX, t);
}
/* servo angle -> blade pitch */
float blade_pitch_deg_from_servo_deg(float servo_deg) {
  float t = inv_lerp(SERVO_DEG_MIN, SERVO_DEG_MAX, servo_deg);
  t = clampf(t, 0.f, 1.f);

  return lerp(BLADE_PITCH_DEG_MIN, BLADE_PITCH_DEG_MAX, t);
}

/* blade pitch -> servo angle */
float servo_deg_from_blade_pitch_deg(float blade_deg) {
  float t = inv_lerp(BLADE_PITCH_DEG_MIN, BLADE_PITCH_DEG_MAX, blade_deg);
  t = clampf(t, 0.f, 1.f);

  return lerp(SERVO_DEG_MIN, SERVO_DEG_MAX, t);
}

void blade_pitch_set(float blade_deg) {
  // float servo_deg_0 = servo_deg_from_blade_pitch_deg(global_blade_deg);
  float servo_deg_0 = servo_deg_from_blade_pitch_deg(blade_deg);
  float servo_deg_1 = SERVO_DEG_MAX - servo_deg_0; // mechanically mirrored
  // global_blade_deg += 5;
  // global_blade_deg = fmodf(global_blade_deg, SERVO_DEG_MAX);
  global_blade_deg = blade_deg;

  uint32_t cc_0 = (uint32_t)cc_from_servo_deg(servo_deg_0);
  uint32_t cc_1 = (uint32_t)cc_from_servo_deg(servo_deg_1);

  DL_Timer_setCaptureCompareValue(SERVO_PWM_INST, cc_0, DL_TIMER_CC_0_INDEX);
  DL_Timer_setCaptureCompareValue(SERVO_PWM_INST, cc_1, DL_TIMER_CC_1_INDEX);
}

void blade_pitch_lut(void) {
  // TODO: implement later when measured expermimentaly
  //       defaulting to attack angle (power)
  if (global_windspeed_m_s < 5.f) {
    blade_pitch_set(BLADE_PITCH_DEG_POWER);
  } else {
    blade_pitch_set(BLADE_PITCH_DEG_FEATHERED);
  }
}

typedef struct {
  float kp;
  float ki;
  float kd;

  float integral;
  float prev_error;

  float out_min;
  float out_max;
} PID;

static PID pitch_pid_state = {
  .kp = 0.05f,
  .ki = 0.01f,
  .kd = 0.0f,

  .integral = 0.0f,
  .prev_error = 0.0f,

  .out_min = BLADE_PITCH_DEG_MIN,
  .out_max = BLADE_PITCH_DEG_MAX,
};

float pid_update(PID *pid, float target, float measured, float dt) {
  float error = target - measured;

  pid->integral += error * dt;
  float derivative = (error - pid->prev_error) / dt;
  pid->prev_error = error;

  float out =
    pid->kp * error +
    pid->ki * pid->integral +
    pid->kd * derivative;

  out = clampf(out, pid->out_min, pid->out_max);

  return out;
}

void blade_pitch_pid(float target_rpm, float measured_rpm, float dt) {
  float blade_deg = pid_update(&pitch_pid_state, target_rpm, measured_rpm, dt);
  blade_pitch_set(blade_deg);
}

void blade_pitch_feather(void) {
  blade_pitch_set(BLADE_PITCH_DEG_FEATHERED);
}

// #define LED_MASK (GPIO_LEDS_RED_PIN | GPIO_LEDS_GREEN_PIN |
// GPIO_LEDS_BLUE_PIN)

// uint32_t rainbow[6] = {
//     GPIO_LEDS_RED_PIN,                        // red
//     GPIO_LEDS_RED_PIN | GPIO_LEDS_GREEN_PIN,  // yellow
//     GPIO_LEDS_GREEN_PIN,                      // green
//     GPIO_LEDS_GREEN_PIN | GPIO_LEDS_BLUE_PIN, // cyan
//     GPIO_LEDS_BLUE_PIN,                       // blue
//     GPIO_LEDS_BLUE_PIN | GPIO_LEDS_RED_PIN,   // magenta
// };

// volatile uint32_t global_rainbow_idx = 0;

void init(void) {
  SYSCFG_DL_init();

  NVIC_EnableIRQ(EVERY_SECOND_INST_INT_IRQN);
  NVIC_EnableIRQ(WIND_ADC12_INST_INT_IRQN);
  NVIC_EnableIRQ(HALL_EFFECT_TIMEOUT_INST_INT_IRQN);
  NVIC_EnableIRQ(HALL_EFFECT_CAPTURE_INST_INT_IRQN);
  NVIC_EnableIRQ(RESET_TIMEOUT_INST_INT_IRQN);
  NVIC_EnableIRQ(UART_DEBUG_INST_INT_IRQN);
  NVIC_EnableIRQ(UART_LOAD_INST_INT_IRQN);

  DL_Timer_startCounter(EVERY_SECOND_INST);
  DL_Timer_startCounter(HALL_EFFECT_TIMEOUT_INST);
  DL_Timer_startCounter(HALL_EFFECT_CAPTURE_INST);
  DL_Timer_startCounter(RESET_TIMEOUT_INST);
  DL_Timer_startCounter(SERVO_PWM_INST);

#ifdef UART_DEBUG
  uart_debug_printf("FINISHED INIT\r\n");
#endif
  global_state = STATE_DURABILITY;
}

// #define RPM_RATED_SET_POINT 1700
#define RPM_RATED_SET_POINT 500
#define RPM_SAFETY 2100
#define RPM_HYSTERISIS 200

#define RPM_MAXIMIZE_OUT RPM_RATED_SET_POINT - RPM_HYSTERISIS
#define RPM_RATED_IN RPM_MAXIMIZE_OUT

#define RPM_RATED_OUT RPM_HYSTERISIS + RPM_HYSTERISIS
#define RPM_DURABILITY_IN RPM_RATED_OUT

#define RPM_DURABILITY_OUT RPM_SAFETY - RPM_HYSTERISIS
#define RPM_SAFETY_IN RPM_SAFETY_IN

void state_maximize(void) {
  pending_sensors();
  if (global_windspeed_m_s >= 0.8f) {
    global_state = STATE_DURABILITY;
    return;
  }
  // if (global_rpm >= RPM_MAXIMIZE_OUT) {
  //   global_state = STATE_RATED;
  //   return;
  // }
  if (global_adjust_pitch) {
    global_adjust_pitch = false;
    blade_pitch_set(BLADE_PITCH_DEG_POWER);
    // blade_pitch_lut();
  }
  pending_uart();
}

void state_rated(void) {
  pending_sensors();
  if (global_rpm <= RPM_RATED_IN) {
    global_state = STATE_MAXIMIZE;
    return;
  }
  if (global_rpm >= RPM_RATED_OUT) {
    global_state = STATE_DURABILITY;
    return;
  }
  if (global_adjust_pitch) {
    global_adjust_pitch = false;
    blade_pitch_pid(BLADE_PITCH_DEG_POWER, global_rpm, EVERY_SECOND_DELTA_TIME_SECONDS);
  }
  pending_uart();
}

void state_durability(void) {
  pending_sensors();
  if (global_windspeed_m_s < 0.8f) {
    global_state = STATE_MAXIMIZE;
    return;
  }
  // if (global_rpm <= RPM_DURABILITY_IN) {
  //   global_state = STATE_RATED;
  //   return;
  // }
  // if (global_rpm >= RPM_DURABILITY_OUT) {
  //   global_state = STATE_SAFETY;
  //   return;
  // }
  if (global_adjust_pitch) {
    global_adjust_pitch = false;
    // blade_pitch_feather();
    blade_pitch_set(BLADE_PITCH_DEG_FEATHERED);
  }
  pending_uart();
}

void state_restart(void) {
  if (!global_critical_condition && global_reset_timeout) {
    global_state = STATE_DURABILITY;
    return;
  }
}

void state_safety(void) {
  pending_sensors();
  if (!global_critical_condition) {
    global_state = STATE_RESTART;

    RESET_TIMEOUT_INST->COUNTERREGS.CTRCTL &= ~0x1;
    RESET_TIMEOUT_INST->COUNTERREGS.CTR = 0;
    RESET_TIMEOUT_INST->COUNTERREGS.LOAD = RESET_TIMEOUT_INST_LOAD_VALUE;
    RESET_TIMEOUT_INST->COUNTERREGS.CTRCTL |= 0x1;

    return;
  }
  if (global_adjust_pitch) {
    global_adjust_pitch = false;
    blade_pitch_feather();
  }
  pending_uart();
}

int main(void) {
  global_state = STATE_INIT;
  init();

  // for testing
  // blade_pitch_set(0.f);
  // blade_pitch_set(45.f);
  // blade_pitch_set(90.f);

  while (true) {
    if (global_critical_condition) {
      global_state = STATE_SAFETY;
      DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_RED_PIN);
    } else {
      DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_RED_PIN);
    }

    switch (global_state) {
    case STATE_INIT:
      __BKPT(0);
      break;
    case STATE_MAXIMIZE:
      state_maximize();
      break;
    case STATE_RATED:
      state_rated();
      break;
    case STATE_DURABILITY:
      state_durability();
      break;
    case STATE_RESTART:
      state_restart();
      break;
    case STATE_SAFETY:
      state_safety();
      break;
    }

    __WFI();
  }
}
