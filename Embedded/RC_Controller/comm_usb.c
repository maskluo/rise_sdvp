/*
	Copyright 2012-2014 Benjamin Vedder	benjamin@vedder.se

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

/*
 * comm.c
 *
 *  Created on: 22 nov 2012
 *      Author: benjamin
 */

#include "commands.h"
#include "ch.h"
#include "hal.h"
#include "comm_usb.h"
#include "packet.h"
#include "comm_usb_serial.h"

// Settings
#define PACKET_HANDLER				0
#define SERIAL_RX_BUFFER_SIZE		2048
#define SERIAL_TX_BUFFER_SIZE		2048

// Private variables
static uint8_t serial_rx_buffer[SERIAL_RX_BUFFER_SIZE];
static uint8_t serial_tx_buffer[SERIAL_RX_BUFFER_SIZE];
static volatile int serial_rx_read_pos = 0;
static volatile int serial_rx_write_pos = 0;
static volatile int serial_tx_read_pos = 0;
static volatile int serial_tx_write_pos = 0;
static THD_WORKING_AREA(serial_read_thread_wa, 512);
static THD_WORKING_AREA(serial_write_thread_wa, 512);
static THD_WORKING_AREA(serial_process_thread_wa, 4096);
static mutex_t send_mutex;
static thread_t *process_tp;
static thread_t *write_tp;

// Private functions
static void process_packet(unsigned char *data, unsigned int len);
static void send_packet(unsigned char *buffer, unsigned int len);
static THD_FUNCTION(serial_read_thread, arg);
static THD_FUNCTION(serial_write_thread, arg);
static THD_FUNCTION(serial_process_thread, arg);

void comm_usb_init(void) {
	comm_usb_serial_init();
	packet_init(send_packet, process_packet, PACKET_HANDLER);

	chMtxObjectInit(&send_mutex);

	// Threads
	chThdCreateStatic(serial_read_thread_wa, sizeof(serial_read_thread_wa), NORMALPRIO, serial_read_thread, NULL);
	chThdCreateStatic(serial_write_thread_wa, sizeof(serial_write_thread_wa), NORMALPRIO, serial_write_thread, NULL);
	chThdCreateStatic(serial_process_thread_wa, sizeof(serial_process_thread_wa), NORMALPRIO, serial_process_thread, NULL);
}

void comm_usb_send_packet(unsigned char *data, unsigned int len) {
	chMtxLock(&send_mutex);
	packet_send_packet(data, len, PACKET_HANDLER);
	chMtxUnlock(&send_mutex);
}

static THD_FUNCTION(serial_read_thread, arg) {
	(void)arg;

	chRegSetThreadName("USB-Serial read");

	uint8_t buffer[128];
	int i;
	int len;
	int had_data = 0;

	for(;;) {
		len = chSequentialStreamRead(&SDU1, (uint8_t*) buffer, 1);

		for (i = 0;i < len;i++) {
			serial_rx_buffer[serial_rx_write_pos++] = buffer[i];

			if (serial_rx_write_pos == SERIAL_RX_BUFFER_SIZE) {
				serial_rx_write_pos = 0;
			}

			had_data = 1;
		}

		if (had_data) {
			chEvtSignal(process_tp, (eventmask_t) 1);
			had_data = 0;
		}
	}
}

static THD_FUNCTION(serial_process_thread, arg) {
	(void)arg;

	chRegSetThreadName("USB-Serial process");

	process_tp = chThdGetSelfX();

	for(;;) {
		chEvtWaitAny((eventmask_t) 1);

		while (serial_rx_read_pos != serial_rx_write_pos) {
			packet_process_byte(serial_rx_buffer[serial_rx_read_pos++], PACKET_HANDLER);

			if (serial_rx_read_pos == SERIAL_RX_BUFFER_SIZE) {
				serial_rx_read_pos = 0;
			}
		}
	}
}

static THD_FUNCTION(serial_write_thread, arg) {
	(void)arg;

	chRegSetThreadName("USB-Serial write");

	write_tp = chThdGetSelfX();

	for(;;) {
		chEvtWaitAny((eventmask_t) 1);

		while (serial_tx_read_pos != serial_tx_write_pos) {
			chSequentialStreamWrite(&SDU1, serial_tx_buffer + serial_tx_read_pos, 1);
			serial_tx_read_pos++;

			if (serial_tx_read_pos == SERIAL_TX_BUFFER_SIZE) {
				serial_tx_read_pos = 0;
			}
		}
	}
}

static void process_packet(unsigned char *data, unsigned int len) {
	commands_process_packet(data, len, comm_usb_send_packet);
}

static void send_packet(unsigned char *buffer, unsigned int len) {
	unsigned int remaining = (serial_tx_write_pos > serial_tx_read_pos) ?
					serial_tx_write_pos - serial_tx_read_pos :
					SERIAL_TX_BUFFER_SIZE - serial_tx_read_pos + serial_tx_write_pos;
	remaining -= 1;

	systime_t timeout = 10;
	while(len > remaining) {
		chThdSleepMilliseconds(1);

		remaining = (serial_tx_write_pos >= serial_tx_read_pos) ?
					serial_tx_write_pos - serial_tx_read_pos :
					SERIAL_TX_BUFFER_SIZE - serial_tx_read_pos + serial_tx_write_pos;
		remaining -= 1;

		timeout--;
		if (timeout == 0) {
			return;
		}
	}

	for (unsigned int i = 0;i < len;i++) {
		serial_tx_buffer[serial_tx_write_pos++] = buffer[i];

		if (serial_tx_write_pos == SERIAL_TX_BUFFER_SIZE) {
			serial_tx_write_pos = 0;
		}
	}

	chEvtSignal(write_tp, (eventmask_t) 1);
}
