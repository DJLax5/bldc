/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#include "timeout.h"
#include "mc_interface.h"
#include "stm32f4xx_conf.h"

// Private variables
static volatile bool init_done = false;
static volatile systime_t timeout_msec;
static volatile systime_t last_update_time;
static volatile float timeout_brake_current;
static volatile bool has_timeout;
static volatile uint32_t feed_counter[MAX_THREADS_MONITOR];

// Threads
static THD_WORKING_AREA(timeout_thread_wa, 512);
static THD_FUNCTION(timeout_thread, arg);

void timeout_init(void) {
	timeout_msec = 1000;
	last_update_time = 0;
	timeout_brake_current = 0.0;
	has_timeout = false;
	init_done = true;

	IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);

	// IWDG counter clock: LSI/4
	IWDG_SetPrescaler(IWDG_Prescaler_4);

	/* Set counter reload value to obtain 12ms IWDG TimeOut.
	 *
	 * LSI timer per datasheet is 32KHz typical, but 17KHz min
	 * and 47KHz max over the complete range of operating conditions,
	 * so reload time must ensure watchdog will work correctly under
	 * all conditions.
	 *
	 * Timeout threads runs every 10ms. Take 20% margin so wdt should
	 * be fed every 12ms. The worst condition occurs when the wdt clock
	 * runs at the max freq (47KHz) due to oscillator tolerances.
	 *
	 * t_IWDG(ms) = t_LSI(ms) * 4 * 2^(IWDG_PR[2:0]) * (IWDG_RLR[11:0] + 1)
	 * t_LSI(ms) [MAX] = 0.021276ms
	 * 12ms = 0.0212765 * 4 * 1 * (140 + 1)
	 *
	 * Counter Reload Value = 140
	 *
	 * When LSI clock runs the slowest, the IWDG will expire every 33.17ms
	*/
	IWDG_SetReload(140);

	IWDG_ReloadCounter();

	/* Enable IWDG (the LSI oscillator will be enabled by hardware) */
	IWDG_Enable();

	chThdSleepMilliseconds(10);

	chThdCreateStatic(timeout_thread_wa, sizeof(timeout_thread_wa), NORMALPRIO, timeout_thread, NULL);
}

void timeout_configure(systime_t timeout, float brake_current) {
	timeout_msec = timeout;
	timeout_brake_current = brake_current;
}

void timeout_reset(void) {
	last_update_time = chVTGetSystemTime();
}

bool timeout_has_timeout(void) {
	return has_timeout;
}

systime_t timeout_get_timeout_msec(void) {
	return timeout_msec;
}

float timeout_get_brake_current(void) {
	return timeout_brake_current;
}

void timeout_feed_WDT(uint8_t index) {
	++feed_counter[index];
}

void timeout_configure_IWDT_slowest(void) {
	if (!init_done) {
		return;
	}

	while(((IWDG->SR & IWDG_SR_RVU) != 0) || ((IWDG->SR & IWDG_SR_PVU) != 0)) {
		// Continue to kick the dog
		IWDG_ReloadCounter();
	}

	// Unlock register
	IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
	// Update configuration
	IWDG_SetReload(4000);
	IWDG_SetPrescaler(IWDG_Prescaler_256);

	// Wait for the new configuration to be taken into account
	while(((IWDG->SR & IWDG_SR_RVU) != 0) || ((IWDG->SR & IWDG_SR_PVU) != 0)) {
		// Continue to kick the dog
		IWDG_ReloadCounter();
	}
}

void timeout_configure_IWDT(void) {
	if (!init_done) {
		return;
	}

	while(((IWDG->SR & IWDG_SR_RVU) != 0) || ((IWDG->SR & IWDG_SR_PVU) != 0)) {
		// Continue to kick the dog
		IWDG_ReloadCounter();
	}

	// Unlock register
	IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
	// Update configuration
	IWDG_SetReload(140);
	IWDG_SetPrescaler(IWDG_Prescaler_4);

	// Wait for the new configuration to be taken into account
	while(((IWDG->SR & IWDG_SR_RVU) != 0) || ((IWDG->SR & IWDG_SR_PVU) != 0)) {
		// Continue to kick the dog
		IWDG_ReloadCounter();
	}
}

bool timeout_had_IWDG_reset(void) {
	// Check if the system has resumed from IWDG reset
	if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET) {
		/* IWDGRST flag set */
		/* Clear reset flags */
		RCC_ClearFlag();
		return true;
	}

	return false;
}

static THD_FUNCTION(timeout_thread, arg) {
	(void)arg;

	chRegSetThreadName("Timeout");

	for(;;) {
		if (timeout_msec != 0 && chVTTimeElapsedSinceX(last_update_time) > MS2ST(timeout_msec)) {
			mc_interface_unlock();
			mc_interface_set_brake_current(timeout_brake_current);
			has_timeout = true;
		} else {
			has_timeout = false;
		}

		bool threads_ok = true;

		// Monitored threads (foc, can, timer) must report at least one iteration,
		// otherwise the watchdog won't be feed and MCU will reset. All threads should
		// be monitored
		if(feed_counter[THREAD_MCPWM] < MIN_THREAD_ITERATIONS) {
			threads_ok = false;
		}
#if CAN_ENABLE
		if(feed_counter[THREAD_CANBUS] < MIN_THREAD_ITERATIONS) {
			threads_ok = false;
		}
#endif
		if(feed_counter[THREAD_TIMER] < MIN_THREAD_ITERATIONS) {
			threads_ok = false;
		}

		for( int i = 0; i < MAX_THREADS_MONITOR; i++) {
			feed_counter[i] = 0;
		}

		if (threads_ok == true) {
			// Feed WDT
			IWDG_ReloadCounter();	// must reload in <12ms
		} else {
			// not reloading the watchdog will produce a reset.
			// This can be checked from the GUI logs as
			// "FAULT_CODE_BOOTING_FROM_WATCHDOG_RESET"
		}

		chThdSleepMilliseconds(10);
	}
}
