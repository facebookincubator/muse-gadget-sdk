/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Whether USB power is plugged in, on boards that can tell:
 * a board's devicetree names a pin that reads high with USB power as
 * /zephyr,user mg-usb-sense-gpios. While the pin is shared with a
 * peripheral that keeps a pull-up on it (a console's RX), a reading swaps
 * in the pull-down for a moment.
 *
 * The XIAO nRF54L15 has none (measured by cutting its USB
 * port's power): VBUS goes to the SAMD11 probe and the charger only. The
 * console's RX (P1.08, the SAMD11's TX) reads high only while a host has
 * the serial port open, not with USB power alone; no input on P0, P1 or P2
 * changes with USB power; and VBAT read the same on and off USB (4032 mV,
 * 87 %). So it can't wake when plugged in, and its boot gate never holds a
 * boot for a low cell (it can't tell whether it is charging).
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <hal/nrf_gpio.h>

#include "mg_usb.h"

#define USER DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(USER, mg_usb_sense_gpios)

#define SENSE_PIN NRF_DT_GPIOS_TO_PSEL(USER, mg_usb_sense_gpios)

int mg_usb_present(void)
{
	NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&(uint32_t){SENSE_PIN});
	uint32_t pin = SENSE_PIN & 0x1f;
	unsigned int key = irq_lock();
	uint32_t cnf = port->PIN_CNF[pin];

	port->PIN_CNF[pin] = (cnf & ~GPIO_PIN_CNF_PULL_Msk) |
			     (GPIO_PIN_CNF_PULL_Pulldown << GPIO_PIN_CNF_PULL_Pos);
	k_busy_wait(50); /* the line's capacitance through the pull-down */
	int v = nrf_gpio_pin_read(SENSE_PIN) ? 1 : 0;

	port->PIN_CNF[pin] = cnf;
	irq_unlock(key);
	return v;
}

void mg_usb_arm_wake(bool on_plug)
{
	nrf_gpio_cfg_sense_input(SENSE_PIN, NRF_GPIO_PIN_PULLDOWN,
				 on_plug ? NRF_GPIO_PIN_SENSE_HIGH : NRF_GPIO_PIN_SENSE_LOW);
}

#else

int mg_usb_present(void)
{
	return -ENOTSUP;
}

void mg_usb_arm_wake(bool on_plug)
{
	(void)on_plug;
}

#endif
