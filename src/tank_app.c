#include "tank_app.h"

#include <stdint.h>

#include "led_driver.h"
#include "userUsbCdc/USBCDC.h"

#define BEACON_STEP_MS 100
#define IDLE_STEP_MS 2000
#define WORK_STEP_MS 500
#define ALERT_STEP_MS 1000
#define DIZZY_STEP_MS 300
#define CUSTOM_DEFAULT_MS 250

/*
 * 应用层维护注意：
 * - 这里是唯一解析 Hub/串口命令的地方。新增状态优先增加 AppMode 和 apply_command() 分支。
 * - 不要在解析命令时做阻塞等待或串口回复；主循环每 20 ms 调用 tick/poll，任何阻塞都会让灯效停止、USB 变慢。
 * - rx_line 故意限制为 160 字节，避免 CH552 小 RAM 被长 JSON 撑爆。Hub 复杂自定义效果靠 apply_stream_effect()
 *   在收完整行前就识别关键字段，所以不要轻易删掉流式解析。
 * - 新增 JSON 字段时保持“弱解析”：只找必要 key/value，不引入大 JSON 库，也不要分配大 buffer。
 */
typedef enum {
  MODE_BEACON,
  MODE_IDLE,
  MODE_WORK,
  MODE_RUN_SOLID,
  MODE_WAIT_SOLID,
  MODE_ALERT_SOLID,
  MODE_SLEEPING,
  MODE_ALERT_BLINK,
  MODE_DIZZY,
  MODE_MANUAL,
  MODE_CUSTOM_BLINK,
  MODE_CUSTOM_CHASE,
  MODE_CUSTOM_PAIR_CHASE,
} AppMode;

static AppMode app_mode = MODE_BEACON;
static uint16_t work_elapsed_ms = 0;
static uint8_t work_step = 0;
static uint8_t manual_mask = 0;
static uint8_t custom_mask = LED_BIT_ALL;
static uint16_t custom_period_ms = CUSTOM_DEFAULT_MS;
static __xdata char rx_line[160];
static uint8_t rx_len = 0;

static uint16_t current_step_ms(void) {
  if (app_mode == MODE_CUSTOM_BLINK || app_mode == MODE_CUSTOM_CHASE ||
      app_mode == MODE_CUSTOM_PAIR_CHASE) {
    return custom_period_ms ? custom_period_ms : CUSTOM_DEFAULT_MS;
  }
  if (app_mode == MODE_ALERT_BLINK) return ALERT_STEP_MS;
  if (app_mode == MODE_DIZZY) return DIZZY_STEP_MS;
  if (app_mode == MODE_IDLE) return IDLE_STEP_MS;
  return (app_mode == MODE_BEACON) ? BEACON_STEP_MS : WORK_STEP_MS;
}

/*
 * Hub/UI 逻辑颜色约定：001=绿/运行，010=黄/等待，100=红/警告。
 * 实际 PCB 走线和 LED_BIT_* 不完全同序，所以所有外部 mask 必须先经过这里转换。
 * 如果以后换板子或换线，只改这个函数和 led_driver.c 的硬件位，不要到处改命令名。
 */
static uint8_t logical_to_hw_mask(uint8_t mask) {
  uint8_t hw = 0;
  if (mask & 0x01) hw |= LED_BIT_WAIT;
  if (mask & 0x02) hw |= LED_BIT_ALERT;
  if (mask & 0x04) hw |= LED_BIT_RUN;
  return hw;
}

static void update_leds(void) {
  if (app_mode == MODE_RUN_SOLID) {
    led_driver_apply_mask(logical_to_hw_mask(0x01));
    return;
  }
  if (app_mode == MODE_WAIT_SOLID) {
    led_driver_apply_mask(logical_to_hw_mask(0x02));
    return;
  }
  if (app_mode == MODE_ALERT_SOLID) {
    led_driver_apply_mask(logical_to_hw_mask(0x04));
    return;
  }
  if (app_mode == MODE_SLEEPING) {
    led_driver_apply_mask(0);
    return;
  }
  if (app_mode == MODE_MANUAL) {
    led_driver_apply_mask(manual_mask);
    return;
  }
  if (app_mode == MODE_ALERT_BLINK) {
    led_driver_apply_mask((work_step & 1) ? 0 : LED_BIT_ALL);
    return;
  }
  if (app_mode == MODE_CUSTOM_BLINK) {
    led_driver_apply_mask((work_step & 1) ? 0 : custom_mask);
    return;
  }
  if (app_mode == MODE_CUSTOM_PAIR_CHASE) {
    if (work_step == 0) {
      led_driver_apply_mask(custom_mask & (LED_BIT_RUN | LED_BIT_WAIT));
    } else if (work_step == 1) {
      led_driver_apply_mask(custom_mask & (LED_BIT_WAIT | LED_BIT_ALERT));
    } else {
      led_driver_apply_mask(custom_mask & (LED_BIT_RUN | LED_BIT_ALERT));
    }
    return;
  }
  if (app_mode == MODE_DIZZY) {
    if (work_step == 0) {
      led_driver_apply_mask(LED_BIT_RUN | LED_BIT_WAIT);
    } else if (work_step == 1) {
      led_driver_apply_mask(LED_BIT_WAIT | LED_BIT_ALERT);
    } else {
      led_driver_apply_mask(LED_BIT_RUN | LED_BIT_ALERT);
    }
    return;
  }
  if (app_mode == MODE_CUSTOM_CHASE) {
    uint8_t step_mask;
    if (work_step == 0) {
      step_mask = LED_BIT_RUN;
    } else if (work_step == 1) {
      step_mask = LED_BIT_WAIT;
    } else {
      step_mask = LED_BIT_ALERT;
    }
    led_driver_apply_mask(custom_mask & step_mask);
    return;
  }

  if (work_step == 0) {
    led_driver_apply_mask(LED_BIT_RUN);
  } else if (work_step == 1) {
    led_driver_apply_mask(LED_BIT_WAIT);
  } else {
    led_driver_apply_mask(LED_BIT_ALERT);
  }
}

static void set_mode(AppMode mode) {
  app_mode = mode;
  work_elapsed_ms = 0;
  work_step = 0;
  update_leds();
}

static uint8_t streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a++ != *b++) return 0;
  }
  return *a == '\0' && *b == '\0';
}

static uint8_t starts_with(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s++ != *prefix++) return 0;
  }
  return 1;
}

static uint8_t contains_text(const char *s, const char *needle) {
  const char *p;
  const char *n;

  if (!*needle) return 1;
  while (*s) {
    p = s;
    n = needle;
    while (*p && *n && *p == *n) {
      ++p;
      ++n;
    }
    if (!*n) return 1;
    ++s;
  }
  return 0;
}

static const char *command_value(const char *line) {
  if (starts_with(line, "anim=")) return line + 5;
  if (starts_with(line, "id=")) return line + 3;
  return line;
}

static uint8_t json_string_value(char *line, const char *key, char *out, uint8_t out_size) {
  char *p = line;

  if (out_size == 0) return 0;
  while (*p) {
    const char *k = key;
    uint8_t len = 0;

    if (*p == '"') {
      ++p;
      while (*k && *p == *k) {
        ++p;
        ++k;
      }
      if (*k != '\0' || *p != '"') continue;
      ++p;
      while (*p && *p != ':') ++p;
      if (*p != ':') return 0;
      ++p;
      while (*p == ' ' || *p == '\t') ++p;
      if (*p != '"') return 0;
      ++p;
      while (*p && *p != '"' && len < out_size - 1) {
        out[len++] = *p++;
      }
      out[len] = '\0';
      return len > 0;
    }
    ++p;
  }
  return 0;
}

static uint8_t json_uint16_value(char *line, const char *key, uint16_t *out) {
  char *p = line;

  while (*p) {
    const char *k = key;
    uint16_t value = 0;
    uint8_t found_digit = 0;

    if (*p == '"') {
      ++p;
      while (*k && *p == *k) {
        ++p;
        ++k;
      }
      if (*k != '\0' || *p != '"') continue;
      ++p;
      while (*p && *p != ':') ++p;
      if (*p != ':') return 0;
      ++p;
      while (*p == ' ' || *p == '\t' || *p == '"') ++p;
      while (*p >= '0' && *p <= '9') {
        found_digit = 1;
        value = (uint16_t)(value * 10 + (*p - '0'));
        ++p;
      }
      if (!found_digit) return 0;
      *out = value;
      return 1;
    }
    ++p;
  }
  return 0;
}

static uint8_t mask_from_bits(const char *bits) {
  uint8_t mask = 0;

  if (bits[0] == '1') mask |= 0x01;
  if (bits[1] == '1') mask |= 0x02;
  if (bits[2] == '1') mask |= 0x04;
  return logical_to_hw_mask(mask);
}

static uint8_t apply_json_command(char *line, char *anim_value, uint8_t anim_size) {
  char value[16];
  uint16_t period;

  if (contains_text(line, "\"effect\"")) {
    if (contains_text(line, "\"100\"")) {
      custom_mask = mask_from_bits("100");
    } else if (contains_text(line, "\"010\"")) {
      custom_mask = mask_from_bits("010");
    } else if (contains_text(line, "\"001\"")) {
      custom_mask = mask_from_bits("001");
    } else {
      custom_mask = LED_BIT_ALL;
    }

    if (json_uint16_value(line, "period", &period) ||
        json_uint16_value(line, "period_ms", &period)) {
      if (period < 20) period = 20;
      custom_period_ms = period;
    } else {
      custom_period_ms = CUSTOM_DEFAULT_MS;
    }

    if (contains_text(line, "pair_chase")) {
      set_mode(MODE_CUSTOM_PAIR_CHASE);
    } else if (contains_text(line, "blink")) {
      set_mode(MODE_CUSTOM_BLINK);
    } else {
      set_mode(MODE_CUSTOM_CHASE);
    }
    return 1;
  }

  if (json_string_value(line, "pattern", value, sizeof(value))) {
    if (json_string_value(line, "mask", anim_value, anim_size)) {
      custom_mask = mask_from_bits(anim_value);
    } else {
      custom_mask = LED_BIT_ALL;
    }
    if (json_uint16_value(line, "period", &period)) {
      if (period < 20) period = 20;
      custom_period_ms = period;
    } else {
      custom_period_ms = CUSTOM_DEFAULT_MS;
    }

    if (streq(value, "blink")) {
      set_mode(MODE_CUSTOM_BLINK);
    } else if (streq(value, "pair_chase")) {
      set_mode(MODE_CUSTOM_PAIR_CHASE);
    } else {
      set_mode(MODE_CUSTOM_CHASE);
    }
    return 1;
  }

  if (json_string_value(line, "leds", value, sizeof(value)) ||
      json_string_value(line, "led", value, sizeof(value)) ||
      json_string_value(line, "mask", value, sizeof(value))) {
    manual_mask = mask_from_bits(value);
    set_mode(MODE_MANUAL);
    return 1;
  }

  return json_string_value(line, "anim", anim_value, anim_size) ||
         json_string_value(line, "id", anim_value, anim_size);
}

static void apply_stream_effect(char *line) {
  if (!contains_text(line, "\"effect\"")) return;
  if (!contains_text(line, "blink") && !contains_text(line, "chase")) return;

  if (contains_text(line, "\"100\"")) {
    custom_mask = mask_from_bits("100");
  } else if (contains_text(line, "\"010\"")) {
    custom_mask = mask_from_bits("010");
  } else if (contains_text(line, "\"001\"")) {
    custom_mask = mask_from_bits("001");
  } else if (contains_text(line, "\"111\"")) {
    custom_mask = LED_BIT_ALL;
  } else {
    return;
  }

  custom_period_ms = CUSTOM_DEFAULT_MS;
  if (contains_text(line, "pair_chase")) {
    set_mode(MODE_CUSTOM_PAIR_CHASE);
  } else if (contains_text(line, "blink")) {
    set_mode(MODE_CUSTOM_BLINK);
  } else {
    set_mode(MODE_CUSTOM_CHASE);
  }
}

static void apply_command(char *line) {
  char anim_value[16];
  const char *cmd = command_value(line);

  if (line[0] == '{') {
    if (!apply_json_command(line, anim_value, sizeof(anim_value))) {
      return;
    }
    if (app_mode == MODE_MANUAL || app_mode == MODE_CUSTOM_BLINK ||
        app_mode == MODE_CUSTOM_CHASE || app_mode == MODE_CUSTOM_PAIR_CHASE) {
      return;
    }
    cmd = anim_value;
  }

  if (streq(cmd, "boot")) {
    USBJumpToBootloader();
  } else if (streq(cmd, "idle")) {
    set_mode(MODE_IDLE);
  } else if (streq(cmd, "beacon") || streq(cmd, "disconnected")) {
    set_mode(MODE_BEACON);
  } else if (streq(cmd, "work") || streq(cmd, "typing") ||
             streq(cmd, "thinking") || streq(cmd, "building") ||
             streq(cmd, "juggling") || streq(cmd, "conducting") ||
             streq(cmd, "debugger") || streq(cmd, "wizard") ||
             streq(cmd, "sweeping") || streq(cmd, "walking")) {
    set_mode(MODE_WORK);
  } else if (streq(cmd, "run") || streq(cmd, "green") || streq(cmd, "happy")) {
    set_mode(MODE_RUN_SOLID);
  } else if (streq(cmd, "wait") || streq(cmd, "yellow") || streq(cmd, "confused")) {
    set_mode(MODE_WAIT_SOLID);
  } else if (streq(cmd, "alert") || streq(cmd, "red") || streq(cmd, "error")) {
    set_mode(MODE_ALERT_SOLID);
  } else if (streq(cmd, "sleeping") || streq(cmd, "going_away") || streq(cmd, "off")) {
    set_mode(MODE_SLEEPING);
  } else if (streq(cmd, "blink")) {
    set_mode(MODE_ALERT_BLINK);
  } else if (streq(cmd, "dizzy")) {
    set_mode(MODE_DIZZY);
  } else if (streq(cmd, "red_blink")) {
    custom_mask = mask_from_bits("100");
    custom_period_ms = 220;
    set_mode(MODE_CUSTOM_BLINK);
  } else if (streq(cmd, "yellow_blink")) {
    custom_mask = mask_from_bits("010");
    custom_period_ms = 420;
    set_mode(MODE_CUSTOM_BLINK);
  } else if (streq(cmd, "green_blink")) {
    custom_mask = mask_from_bits("001");
    custom_period_ms = 300;
    set_mode(MODE_CUSTOM_BLINK);
  } else if (streq(cmd, "all_chase")) {
    custom_mask = LED_BIT_ALL;
    custom_period_ms = 220;
    set_mode(MODE_CUSTOM_CHASE);
  } else if (streq(cmd, "yellow_chase")) {
    custom_mask = mask_from_bits("010");
    custom_period_ms = 180;
    set_mode(MODE_CUSTOM_CHASE);
  } else if (streq(cmd, "pair_chase")) {
    custom_mask = LED_BIT_ALL;
    custom_period_ms = 260;
    set_mode(MODE_CUSTOM_PAIR_CHASE);
  } else if (streq(cmd, "manual") || streq(cmd, "custom")) {
    set_mode(MODE_MANUAL);
  }
}

void tank_app_init(void) {
  app_mode = MODE_BEACON;
  work_elapsed_ms = 0;
  work_step = 0;
  manual_mask = 0;
  custom_mask = LED_BIT_ALL;
  custom_period_ms = CUSTOM_DEFAULT_MS;
  rx_len = 0;
  update_leds();
}

void tank_app_tick(uint16_t elapsed_ms) {
  work_elapsed_ms = (uint16_t)(work_elapsed_ms + elapsed_ms);

  if (app_mode == MODE_BEACON || app_mode == MODE_IDLE || app_mode == MODE_WORK ||
      app_mode == MODE_ALERT_BLINK || app_mode == MODE_DIZZY ||
      app_mode == MODE_CUSTOM_BLINK || app_mode == MODE_CUSTOM_CHASE ||
      app_mode == MODE_CUSTOM_PAIR_CHASE) {
    while (work_elapsed_ms >= current_step_ms()) {
      work_elapsed_ms = (uint16_t)(work_elapsed_ms - current_step_ms());
      work_step++;
      if (work_step >= 3) {
        work_step = 0;
      }
    }
  }

  update_leds();
}

/*
 * 串口轮询预算：每次最多读 8 字节，避免一次性吃太久导致 LED 动画卡顿。
 * 如果 Hub 发送更长命令，下一轮 loop 会继续读，不需要把预算改得很大。
 */
void tank_app_poll_serial(void) {
  uint8_t budget = 8;

  while (budget-- && USBSerial_available()) {
    char c = USBSerial_read();
    if (c == '\r') continue;

    if (c == '\n') {
      rx_line[rx_len] = '\0';
      apply_command(rx_line);
      rx_len = 0;
    } else if (rx_len < sizeof(rx_line) - 1) {
      rx_line[rx_len++] = c;
      rx_line[rx_len] = '\0';
      apply_stream_effect(rx_line);
    } else {
      rx_len = 0;
    }
  }
}
