/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "drivers/serial.h"
#ifdef _WIN32
#include "winsock2.h"
#endif
#include "dyad.h"

#define RX_BUFFER_SIZE 1400
#define TX_BUFFER_SIZE 1400

typedef struct {
    serialPort_t port;
    uint8_t rxBuffer[RX_BUFFER_SIZE];
    uint8_t txBuffer[TX_BUFFER_SIZE];

    dyad_Stream *serv;
    dyad_Stream *conn;

    bool connected;
    uint16_t clientCount;
    uint8_t id;
} tcpPort_t;

serialPort_t *serTcpOpen(int id,
                         serialReceiveCallbackPtr rxCallback,
                         void *rxCallbackData,
                         uint32_t baudRate,
                         portMode_e mode,
                         portOptions_e options);

// tcpPort API
void tcpDataIn(tcpPort_t *instance, uint8_t *ch, int size);
void tcpDataOut(tcpPort_t *instance);

bool tcpIsStart(void);
bool *tcpGetUsed(void);
tcpPort_t *tcpGetPool(void);
