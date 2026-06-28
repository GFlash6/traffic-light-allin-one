#ifndef __USB_CDC_H__
#define __USB_CDC_H__

// clang-format off
#include <stdint.h>
#include <stdbool.h>
#include "include/ch5xx.h"
#include "include/ch5xx_usb.h"
// clang-format on

#ifdef __cplusplus
extern "C" {
#endif

void USBInit(void);
void USBJumpToBootloader(void);
bool USBSerial(void);
uint8_t USBSerial_available(void);
char USBSerial_read(void);
uint8_t USBSerial_write(__data char c);
void USBSerial_flush(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
