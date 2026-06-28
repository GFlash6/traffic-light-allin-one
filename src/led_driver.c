#include "led_driver.h"

#include "userUsbCdc/include/ch5xx.h"

/*
 * LED 硬件映射注意：
 * - 三个灯接在 P1.4/P1.5/P1.6，当前硬件是低电平点亮。
 * - P1 寄存器还有其它位，写 LED 时必须通过 p1_shadow 只改 LED 位，避免误改其它引脚。
 * - LED_BIT_RUN/WAIT/ALERT 是“固件内部硬件位”，不要直接等同于 Hub 传来的 001/010/100；
 *   UI/Hub 的逻辑颜色转换集中在 tank_app.c 的 logical_to_hw_mask()。
 */
#define P14_LED_MASK 0x10
#define P15_LED_MASK 0x20
#define P16_LED_MASK 0x40
#define P1_LED_MASK (P14_LED_MASK | P15_LED_MASK | P16_LED_MASK)

static uint8_t p1_shadow = 0xff;
static uint8_t applied_mask = 0xff;

void led_driver_init(void) {
  P1_MOD_OC &= ~P1_LED_MASK;
  P1_DIR_PU |= P1_LED_MASK;
  p1_shadow = P1 | P1_LED_MASK;
  P1 = p1_shadow;
  applied_mask = 0xff;
}

void led_driver_apply_mask(uint8_t mask) {
  mask &= LED_BIT_ALL;
  if (mask == applied_mask) return;

  applied_mask = mask;
  p1_shadow |= P1_LED_MASK;

  if (mask & LED_BIT_RUN) p1_shadow &= ~P14_LED_MASK;
  if (mask & LED_BIT_WAIT) p1_shadow &= ~P15_LED_MASK;
  if (mask & LED_BIT_ALERT) p1_shadow &= ~P16_LED_MASK;

  P1 = p1_shadow;
}

uint8_t led_driver_applied_mask(void) {
  return applied_mask & LED_BIT_ALL;
}
