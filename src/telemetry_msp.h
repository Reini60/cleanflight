/*
 * telemetry_MSP.h
 *
 *  Created on: 22 Apr 2014
 *      Author: trey marc
 */

#ifndef TELEMETRY_MSP_H_
#define TELEMETRY_MSP_H_

void initMSPTelemetry(telemetryConfig_t *initialTelemetryConfig);
void handleMSPTelemetry(void);

void freeMSPTelemetryPort(void);
void configureMSPTelemetryPort(void);

uint32_t getMSPTelemetryProviderBaudRate(void);

#endif /* TELEMETRY_MSP_H_ */
