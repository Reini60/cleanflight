/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * telemetry_hott.c
 *
 * Authors:
 * Dominic Clifton - Hydra - Software Serial, Electronics, Hardware Integration and debugging, HoTT Code cleanup and fixes, general telemetry improvements.
 * Carsten Giesen - cGiesen - Baseflight port
 * Oliver Bayer - oBayer - MultiWii-HoTT, HoTT reverse engineering
 * Adam Majerczyk - HoTT-for-ardupilot from which some information and ideas are borrowed.
 *
 * https://github.com/obayer/MultiWii-HoTT
 * https://github.com/oBayer/MultiHoTT-Module
 * https://code.google.com/p/hott-for-ardupilot
 *
 * HoTT is implemented in Graupner equipment using a bi-directional protocol over a single wire.
 *
 * Generally the receiver sends a single request byte out using normal uart signals, then waits a short period for a
 * multiple byte response and checksum byte before it sends out the next request byte.
 * Each response byte must be send with a protocol specific delay between them.
 *
 * Serial ports use two wires but HoTT uses a single wire so some electronics are required so that
 * the signals don't get mixed up.  When cleanflight transmits it should not receive it's own transmission.
 *
 * Connect as follows:
 * HoTT TX/RX -> Serial RX (connect directly)
 * Serial TX -> 1N4148 Diode -(|  )-> HoTT TX/RX (connect via diode)
 *
 * The diode should be arranged to allow the data signals to flow the right way
 * -(|  )- == Diode, | indicates cathode marker.
 *
 * As noticed by Skrebber the GR-12 (and probably GR-16/24, too) are based on a PIC 24FJ64GA-002, which has 5V tolerant digital pins.
 *
 * Note: The softserial ports are not listed as 5V tolerant in the STM32F103xx data sheet pinouts and pin description
 * section.  Verify if you require a 5v/3.3v level shifters.  The softserial port should not be inverted.
 *
 * There is a technical discussion (in German) about HoTT here
 * http://www.rc-network.de/forum/showthread.php/281496-Graupner-HoTT-Telemetrie-Sensoren-Eigenbau-DIY-Telemetrie-Protokoll-entschl%C3%BCsselt/page21
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#include "common/axis.h"

#include "drivers/system.h"

#include "drivers/serial.h"
#include "io/serial.h"

#include "config/runtime_config.h"

#include "sensors/sensors.h"
#include "sensors/battery.h"

#include "flight/flight.h"
#include "io/gps.h"

#include "telemetry/telemetry.h"
#include "telemetry/hott.h"

extern int16_t debug[4];

//#define HOTT_DEBUG

#define HOTT_MESSAGE_PREPARATION_FREQUENCY_5_HZ ((1000 * 1000) / 5)
#define HOTT_RX_SCHEDULE 4000
#define HOTT_TX_DELAY_US 3000

static uint32_t lastHoTTRequestCheckAt = 0;
static uint32_t lastMessagesPreparedAt = 0;

static bool hottIsSending = false;

static uint8_t *hottMsg = NULL;
static uint8_t hottMsgRemainingBytesToSendCount;
static uint8_t hottMsgCrc;

#define HOTT_CRC_SIZE (sizeof(hottMsgCrc))

#define HOTT_BAUDRATE 19200
#define HOTT_INITIAL_PORT_MODE MODE_RX

static serialPort_t *hottPort;

static telemetryConfig_t *telemetryConfig;

static HOTT_GPS_MSG_t hottGPSMessage;
static HOTT_EAM_MSG_t hottEAMMessage;

typedef enum {
    GPS_FIX_CHAR_NONE = '-',
    GPS_FIX_CHAR_2D = '2',
    GPS_FIX_CHAR_3D = '3',
    GPS_FIX_CHAR_DGPS = 'D',
} gpsFixChar_e;

static void initialiseEAMMessage(HOTT_EAM_MSG_t *msg, size_t size)
{
    memset(msg, 0, size);
    msg->start_byte = 0x7C;
    msg->eam_sensor_id = HOTT_TELEMETRY_EAM_SENSOR_ID;
    msg->sensor_id = HOTT_EAM_SENSOR_TEXT_ID;
    msg->stop_byte = 0x7D;
}

static void initialiseGPSMessage(HOTT_GPS_MSG_t *msg, size_t size)
{
    memset(msg, 0, size);
    msg->start_byte = 0x7C;
    msg->gps_sensor_id = HOTT_TELEMETRY_GPS_SENSOR_ID;
    msg->sensor_id = HOTT_GPS_SENSOR_TEXT_ID;
    msg->stop_byte = 0x7D;
}

static void initialiseMessages(void)
{
    initialiseEAMMessage(&hottEAMMessage, sizeof(hottEAMMessage));
    initialiseGPSMessage(&hottGPSMessage, sizeof(hottGPSMessage));
}

void addGPSCoordinates(HOTT_GPS_MSG_t *hottGPSMessage, int32_t latitude, int32_t longitude)
{
    int16_t deg = latitude / 10000000L;
    int32_t sec = (latitude - (deg * 10000000L)) * 6;
    int8_t min = sec / 1000000L;
    sec = (sec % 1000000L) / 100L;
    uint16_t degMin = (deg * 100L) + min;

    hottGPSMessage->pos_NS = (latitude < 0);
    hottGPSMessage->pos_NS_dm_L = degMin;
    hottGPSMessage->pos_NS_dm_H = degMin >> 8;
    hottGPSMessage->pos_NS_sec_L = sec;
    hottGPSMessage->pos_NS_sec_H = sec >> 8;

    deg = longitude / 10000000L;
    sec = (longitude - (deg * 10000000L)) * 6;
    min = sec / 1000000L;
    sec = (sec % 1000000L) / 100L;
    degMin = (deg * 100L) + min;

    hottGPSMessage->pos_EW = (longitude < 0);
    hottGPSMessage->pos_EW_dm_L = degMin;
    hottGPSMessage->pos_EW_dm_H = degMin >> 8;
    hottGPSMessage->pos_EW_sec_L = sec;
    hottGPSMessage->pos_EW_sec_H = sec >> 8;
}

void hottPrepareGPSResponse(HOTT_GPS_MSG_t *hottGPSMessage)
{
    hottGPSMessage->gps_satelites = GPS_numSat;

    if (!f.GPS_FIX) {
        hottGPSMessage->gps_fix_char = GPS_FIX_CHAR_NONE;
        return;
    }

    if (GPS_numSat >= 5) {
        hottGPSMessage->gps_fix_char = GPS_FIX_CHAR_3D;
    } else {
        hottGPSMessage->gps_fix_char = GPS_FIX_CHAR_2D;
    }

    addGPSCoordinates(hottGPSMessage, GPS_coord[LAT], GPS_coord[LON]);

    // GPS Speed in km/h
    uint16_t speed = (GPS_speed / 100) * 36; // 0->1m/s * 0->36 = km/h
    hottGPSMessage->gps_speed_L = speed & 0x00FF;
    hottGPSMessage->gps_speed_H = speed >> 8;

    hottGPSMessage->home_distance_L = GPS_distanceToHome & 0x00FF;
    hottGPSMessage->home_distance_H = GPS_distanceToHome >> 8;

    uint16_t hottGpsAltitude = (GPS_altitude / 10) + HOTT_GPS_ALTITUDE_OFFSET; // 1 / 0.1f == 10, GPS_altitude of 1 == 0.1m

    hottGPSMessage->altitude_L = hottGpsAltitude & 0x00FF;
    hottGPSMessage->altitude_H = hottGpsAltitude >> 8;

    hottGPSMessage->home_direction = GPS_directionToHome;
}

static inline void hottEAMUpdateBattery(HOTT_EAM_MSG_t *hottEAMMessage)
{
    hottEAMMessage->main_voltage_L = vbat & 0xFF;
    hottEAMMessage->main_voltage_H = vbat >> 8;
    hottEAMMessage->batt1_voltage_L = vbat & 0xFF;
    hottEAMMessage->batt1_voltage_H = vbat >> 8;
}

void hottPrepareEAMResponse(HOTT_EAM_MSG_t *hottEAMMessage)
{
    // Reset alarms
    hottEAMMessage->warning_beeps = 0x0;
    hottEAMMessage->alarm_invers1 = 0x0;

    hottEAMUpdateBattery(hottEAMMessage);
}

static void hottSerialWrite(uint8_t c)
{
    static uint8_t serialWrites = 0;
    serialWrites++;
    serialWrite(hottPort, c);
}

static portMode_t previousPortMode;
static uint32_t previousBaudRate;

void freeHoTTTelemetryPort(void)
{
    // FIXME only need to do this if the port is shared
    serialSetMode(hottPort, previousPortMode);
    serialSetBaudRate(hottPort, previousBaudRate);

    endSerialPortFunction(hottPort, FUNCTION_TELEMETRY);
}

void initHoTTTelemetry(telemetryConfig_t *initialTelemetryConfig)
{
    telemetryConfig = initialTelemetryConfig;

    initialiseMessages();
}

void configureHoTTTelemetryPort(void)
{
    hottPort = findOpenSerialPort(FUNCTION_TELEMETRY);
    if (hottPort) {
        previousPortMode = hottPort->mode;
        previousBaudRate = hottPort->baudRate;

        //waitForSerialPortToFinishTransmitting(hottPort); // FIXME locks up the system

        serialSetBaudRate(hottPort, HOTT_BAUDRATE);
        serialSetMode(hottPort, HOTT_INITIAL_PORT_MODE);
        beginSerialPortFunction(hottPort, FUNCTION_TELEMETRY);
    } else {
        hottPort = openSerialPort(FUNCTION_TELEMETRY, NULL, HOTT_BAUDRATE, HOTT_INITIAL_PORT_MODE, SERIAL_NOT_INVERTED);

        // FIXME only need to do this if the port is shared
        previousPortMode = hottPort->mode;
        previousBaudRate = hottPort->baudRate;
    }
}

static void hottSendResponse(uint8_t *buffer, int length)
{
    if(hottIsSending) {
        return;
    }

    hottMsg = buffer;
    hottMsgRemainingBytesToSendCount = length + HOTT_CRC_SIZE;
}

static inline void hottSendGPSResponse(void)
{
    hottSendResponse((uint8_t *)&hottGPSMessage, sizeof(hottGPSMessage));
}

static inline void hottSendEAMResponse(void)
{
    hottSendResponse((uint8_t *)&hottEAMMessage, sizeof(hottEAMMessage));
}

static void hottPrepareMessages(void) {
    hottPrepareEAMResponse(&hottEAMMessage);
    hottPrepareGPSResponse(&hottGPSMessage);
}

static void processBinaryModeRequest(uint8_t address) {

#ifdef HOTT_DEBUG
    static uint8_t hottBinaryRequests = 0;
    static uint8_t hottGPSRequests = 0;
    static uint8_t hottEAMRequests = 0;
#endif

    switch (address) {
        case 0x8A:
#ifdef HOTT_DEBUG
            hottGPSRequests++;
#endif
            if (sensors(SENSOR_GPS)) {
                hottSendGPSResponse();
            }
            break;
        case 0x8E:
#ifdef HOTT_DEBUG
            hottEAMRequests++;
#endif
            hottSendEAMResponse();
            break;
    }


#ifdef HOTT_DEBUG
    hottBinaryRequests++;
    debug[0] = hottBinaryRequests;
    debug[1] = hottGPSRequests;
    debug[2] = hottEAMRequests;
#endif

}

static void flushHottRxBuffer(void)
{
    while (serialTotalBytesWaiting(hottPort) > 0) {
        serialRead(hottPort);
    }
}

static void hottCheckSerialData(uint32_t currentMicros) {

    static bool lookingForRequest = true;

    uint8_t bytesWaiting = serialTotalBytesWaiting(hottPort);

    if (bytesWaiting <= 1) {
        return;
    }

    if (bytesWaiting != 2) {
        flushHottRxBuffer();
        lookingForRequest = true;
        return;
    }

    if (lookingForRequest) {
        lastHoTTRequestCheckAt = currentMicros;
        lookingForRequest = false;
        return;
    } else {
        bool enoughTimePassed = currentMicros - lastHoTTRequestCheckAt >= HOTT_RX_SCHEDULE;

        if (!enoughTimePassed) {
            return;
        }
        lookingForRequest = true;
    }

    uint8_t requestId = serialRead(hottPort);
    uint8_t address = serialRead(hottPort);

    if (requestId == HOTT_BINARY_MODE_REQUEST_ID) {
        processBinaryModeRequest(address);
    }
}

static void hottSendTelemetryData(void) {
    if (!hottIsSending) {
        hottIsSending = true;
        serialSetMode(hottPort, MODE_TX);
        hottMsgCrc = 0;
        return;
    }

    if (hottMsgRemainingBytesToSendCount == 0) {
        hottMsg = NULL;
        hottIsSending = false;

        serialSetMode(hottPort, MODE_RX);
        flushHottRxBuffer();
        return;
    }

    --hottMsgRemainingBytesToSendCount;
    if(hottMsgRemainingBytesToSendCount == 0) {
        hottSerialWrite(hottMsgCrc++);
        return;
    }

    hottMsgCrc += *hottMsg;
    hottSerialWrite(*hottMsg++);
}

static inline bool shouldPrepareHoTTMessages(uint32_t currentMicros)
{
    return currentMicros - lastMessagesPreparedAt >= HOTT_MESSAGE_PREPARATION_FREQUENCY_5_HZ;
}

static inline bool shouldCheckForHoTTRequest()
{
    if (hottIsSending) {
        return false;
    }
    return true;
}

void handleHoTTTelemetry(void)
{
    static uint32_t serialTimer;
    uint32_t now = micros();


    if (shouldPrepareHoTTMessages(now)) {
        hottPrepareMessages();
        lastMessagesPreparedAt = now;
    }

    if (shouldCheckForHoTTRequest()) {
        hottCheckSerialData(now);
    }

    if (!hottMsg)
        return;

    if (hottIsSending) {
        if(now - serialTimer < HOTT_TX_DELAY_US) {
            return;
        }
    }
    hottSendTelemetryData();
    serialTimer = now;
}

uint32_t getHoTTTelemetryProviderBaudRate(void) {
    return HOTT_BAUDRATE;
}
