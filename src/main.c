/*
 * 二次开发注意事项 / CH552G firmware guard rails
 *
 * 1. 这个固件依赖 USB CDC 下载和运行。不要删除 USBInterrupt、USBInit、EA = 1，
 *    也不要长时间关闭全局中断；否则可能出现串口不可用、Hub 不可控，甚至需要手动进 Bootloader 才能重新烧录。
 * 2. 主循环必须保持“短、快、非阻塞”。不要在 loop 里做长时间 while 等待、串口阻塞回包、复杂 JSON 输出。
 *    之前的卡死/只亮一轮问题，核心就是主循环或 USB CDC 被阻塞。
 * 3. delay_ms 只用于很短的节拍延时；需要新增功能时，优先放到 tank_app_tick() 里按 elapsed_ms 状态机推进。
 * 4. 时钟固定在 16 MHz。改 CLOCK_CFG 会影响 USB 时序和软件延时，除非重新验证 USB、烧录和 LED 节拍。
 * 5. 固件默认不主动向 USB 串口打印 ACK。CH552 CDC TX 容易在主机未读取时阻塞，二次开发不要随意加 Serial/USB 回包。
 */
#include <stdint.h>
#include <stdbool.h>

#include "led_driver.h"
#include "tank_app.h"
#include "userUsbCdc/USBCDC.h"
#include "userUsbCdc/include/ch5xx.h"

#define LOOP_MS 20

void USBInterrupt(void);

void DeviceUSBInterrupt(void) __interrupt(INT_NO_USB) {
  USBInterrupt();
}

void delayMicroseconds(__data uint16_t us) {
  while (us--) {
    __asm__("nop\n nop\n nop\n nop\n nop\n nop\n");
  }
}

static void delay_ms(uint16_t ms) {
  while (ms--) {
    delayMicroseconds(1000);
  }
}

static void clock_init(void) {
  SAFE_MOD = 0x55;
  SAFE_MOD = 0xAA;
  CLOCK_CFG = (CLOCK_CFG & ~MASK_SYS_CK_SEL) | 0x05; /* 16 MHz */
  SAFE_MOD = 0x00;
  delay_ms(5);
}

static void loop_delay(void) {
  uint8_t i;
  for (i = 0; i < LOOP_MS; ++i) {
    delay_ms(1);
  }
}

void main(void) {
  clock_init();
  led_driver_init();
  USBInit();
  EA = 1;
  tank_app_init();

  for (;;) {
    tank_app_poll_serial();
    tank_app_tick(LOOP_MS);
    loop_delay();
  }
}

unsigned char __sdcc_external_startup(void) __nonbanked {
  return 0;
}
