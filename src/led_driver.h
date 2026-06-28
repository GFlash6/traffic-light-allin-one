#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>

#define LED_BIT_RUN 0x01
#define LED_BIT_WAIT 0x02
#define LED_BIT_ALERT 0x04
#define LED_BIT_ALL (LED_BIT_RUN | LED_BIT_WAIT | LED_BIT_ALERT)

void led_driver_init(void);
void led_driver_apply_mask(uint8_t mask);
uint8_t led_driver_applied_mask(void);

#endif
