#ifndef TANK_APP_H
#define TANK_APP_H

#include <stdint.h>

void tank_app_init(void);
void tank_app_tick(uint16_t elapsed_ms);
void tank_app_poll_serial(void);

#endif
