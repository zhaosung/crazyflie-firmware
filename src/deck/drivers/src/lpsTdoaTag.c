/*
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * LPS node firmware.
 *
 * Copyright 2016, Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * lpsTdoaTag.c is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with lpsTdoaTag.c.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <string.h>

#include "log.h"
#include "lpsTdoaTag.h"

#include "stabilizer_types.h"
#include "cfassert.h"

#ifdef ESTIMATOR_TYPE_kalman
#include "estimator_kalman.h"
#endif // ESTIMATOR_TYPE_kalman


static lpsAlgoOptions_t* options;

float uwbTdoaDistDiff[LOCODECK_NR_OF_ANCHORS];
static toaMeasurement_t lastTOA;

static rangePacket_t rxPacketBuffer[LOCODECK_NR_OF_ANCHORS];
static dwTime_t arrivals[LOCODECK_NR_OF_ANCHORS];

static double frameTime_in_cl_M = 0.0;
static double clockCorrection_T_To_M = 1.0;

static int64_t tagClockWrapOffset = 0;
static int64_t tagClockLatestTime = 0;

static int64_t masterClockWrapOffset = 0;
static int64_t masterClockLatestTime = 0;

typedef struct {
  int64_t offset;
  int64_t latestTime;
} clockWrap_t;

static clockWrap_t clockWrapTag, clockWrapMaster;

#define MASTER 0
#define MEASUREMENT_NOISE_STD 0.5f

// The maximum diff in distances that we consider to be valid
// Used to sanity check results and remove results that are wrong due to packet loss
#define MAX_DISTANCE_DIFF (300.0f)

static uint64_t timestampToUint64(uint8_t *ts) {
  dwTime_t timestamp = {.full = 0};
  memcpy(timestamp.raw, ts, sizeof(timestamp.raw));

  return timestamp.full;
}

static uint64_t truncateToTimeStamp(uint64_t fullTimeStamp) {
  return fullTimeStamp & 0x00FFFFFFFFFFul;
}

static int64_t eliminateClockWrap(clockWrap_t* data, int64_t time) {
  if ((time < data->latestTime)) {
    data->offset += 0x10000000000;
  }

  data->latestTime = time;

  return time + data->offset;
}

static void enqueueTDOA(uint8_t anchor, int64_t rxAn_by_T_in_cl_T, int64_t txAn_in_cl_M) {
  tdoaMeasurement_t tdoa = {.stdDev = MEASUREMENT_NOISE_STD};

  memcpy(&(tdoa.measurement[0]), &lastTOA, sizeof(toaMeasurement_t));

  tdoa.measurement[1].senderId = anchor;
  tdoa.measurement[1].rx = eliminateClockWrap(&clockWrapTag, rxAn_by_T_in_cl_T);
  tdoa.measurement[1].tx = eliminateClockWrap(&clockWrapMaster, txAn_in_cl_M);
  tdoa.measurement[1].x = options->anchorPosition[anchor].x;
  tdoa.measurement[1].y = options->anchorPosition[anchor].y;
  tdoa.measurement[1].z = options->anchorPosition[anchor].z;

  memcpy(&lastTOA, &tdoa.measurement[1], sizeof(toaMeasurement_t));
#ifdef ESTIMATOR_TYPE_kalman
  stateEstimatorEnqueueTDOA(&tdoa);
#endif
}

// A note on variable names. They might seem a bit verbose but express quite a lot of information
// We have three actors: Master (M), Anchor n (An) and the deck on the CF called Tag (T)
// rxM_by_An_in_cl_An should be interpreted as "The time when packet was received from the Master Anchor by Anchor N expressed in the clock of Anchor N"
static void rxcallback(dwDevice_t *dev) {
  int dataLength = dwGetDataLength(dev);
  packet_t rxPacket;

  dwGetData(dev, (uint8_t*)&rxPacket, dataLength);

  dwTime_t arrival = {.full = 0};
  dwGetReceiveTimestamp(dev, &arrival);

  uint8_t anchor = rxPacket.sourceAddress & 0xff;

  if (anchor < LOCODECK_NR_OF_ANCHORS) {
    rangePacket_t* packet = (rangePacket_t*)rxPacket.payload;

    int64_t rxM_by_T_in_cl_T  = arrivals[MASTER].full;
    int64_t rxAn_by_T_in_cl_T  = arrival.full;

    if (anchor == MASTER) {
      int64_t previous_txM_in_cl_M = timestampToUint64(rxPacketBuffer[MASTER].timestamps[MASTER]);
      int64_t txM_in_cl_M = timestampToUint64(packet->timestamps[MASTER]);
      frameTime_in_cl_M = truncateToTimeStamp(txM_in_cl_M - previous_txM_in_cl_M);
      double frameTime_in_T = truncateToTimeStamp(rxAn_by_T_in_cl_T - rxM_by_T_in_cl_T);

      clockCorrection_T_To_M = 1.0;
      if (frameTime_in_T != 0.0) {
        clockCorrection_T_To_M = frameTime_in_cl_M / frameTime_in_T;
      }

      enqueueTDOA(MASTER, rxAn_by_T_in_cl_T, txM_in_cl_M);
    } else {
      int64_t previous_txAn_in_cl_An = timestampToUint64(rxPacketBuffer[anchor].timestamps[anchor]);
      int64_t rxAn_by_M_in_cl_M = timestampToUint64(rxPacketBuffer[MASTER].timestamps[anchor]);
      int64_t rxM_by_An_in_cl_An = timestampToUint64(packet->timestamps[MASTER]);
      int64_t txM_in_cl_M = timestampToUint64(rxPacketBuffer[MASTER].timestamps[MASTER]);

      int64_t previuos_rxM_by_An_in_cl_An = timestampToUint64(rxPacketBuffer[anchor].timestamps[MASTER]);

      int64_t txAn_in_cl_An = timestampToUint64(packet->timestamps[anchor]);

      double frameTime_in_cl_An = truncateToTimeStamp(rxM_by_An_in_cl_An - previuos_rxM_by_An_in_cl_An);

      double clockCorrection_An_To_M = 1.0;
      if (frameTime_in_cl_An != 0.0) {
        clockCorrection_An_To_M = frameTime_in_cl_M / frameTime_in_cl_An;
      }

      int64_t tof_M_to_An_in_cl_M = (((truncateToTimeStamp(rxM_by_An_in_cl_An - previous_txAn_in_cl_An) * clockCorrection_An_To_M) - truncateToTimeStamp(txM_in_cl_M - rxAn_by_M_in_cl_M))) / 2.0;
      int64_t delta_txM_to_txAn_in_cl_M = (tof_M_to_An_in_cl_M + truncateToTimeStamp(txAn_in_cl_An - rxM_by_An_in_cl_An) * clockCorrection_An_To_M);
      int64_t timeDiffOfArrival_in_cl_M =  truncateToTimeStamp(rxAn_by_T_in_cl_T - rxM_by_T_in_cl_T) * clockCorrection_T_To_M - delta_txM_to_txAn_in_cl_M;
      int64_t txAn_in_cl_M = txM_in_cl_M + delta_txM_to_txAn_in_cl_M;

      float tdoaDistDiff = SPEED_OF_LIGHT * timeDiffOfArrival_in_cl_M / LOCODECK_TS_FREQ;

      // Sanity check distances in case of missed packages
      if (tdoaDistDiff > -MAX_DISTANCE_DIFF && tdoaDistDiff < MAX_DISTANCE_DIFF) {
        uwbTdoaDistDiff[anchor] = tdoaDistDiff;
        enqueueTDOA(anchor, rxAn_by_T_in_cl_T, txAn_in_cl_M);
      }
    }

    arrivals[anchor].full = arrival.full;
    memcpy(&rxPacketBuffer[anchor], rxPacket.payload, sizeof(rangePacket_t));
  }
}

static void setRadioInReceiveMode(dwDevice_t *dev) {
  dwNewReceive(dev);
  dwSetDefaults(dev);
  dwStartReceive(dev);
}

static uint32_t onEvent(dwDevice_t *dev, uwbEvent_t event) {
  switch(event) {
    case eventPacketReceived:
      rxcallback(dev);
      setRadioInReceiveMode(dev);
      break;
    case eventTimeout:
      setRadioInReceiveMode(dev);
      break;
    case eventReceiveTimeout:
      setRadioInReceiveMode(dev);
      break;
    default:
      ASSERT_FAILED();
  }

  return MAX_TIMEOUT;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
static void Initialize(dwDevice_t *dev, lpsAlgoOptions_t* algoOptions) {
  options = algoOptions;

  // Reset module state. Needed by unit tests
  memset(uwbTdoaDistDiff, 0, sizeof(uwbTdoaDistDiff));
  memset(&lastTOA, 0, sizeof(lastTOA));
  memset(rxPacketBuffer, 0, sizeof(rxPacketBuffer));
  memset(arrivals, 0, sizeof(arrivals));

  frameTime_in_cl_M = 0.0;
  clockCorrection_T_To_M = 1.0;

  tagClockWrapOffset = 0;
  tagClockLatestTime = 0;

  masterClockWrapOffset = 0;
  masterClockLatestTime = 0;

  memset(&clockWrapTag, 0, sizeof(clockWrapTag));
  memset(&clockWrapMaster, 0, sizeof(clockWrapMaster));
}
#pragma GCC diagnostic pop

uwbAlgorithm_t uwbTdoaTagAlgorithm = {
  .init = Initialize,
  .onEvent = onEvent,
};


LOG_GROUP_START(tdoa)
LOG_ADD(LOG_FLOAT, d01, &uwbTdoaDistDiff[1])
LOG_ADD(LOG_FLOAT, d02, &uwbTdoaDistDiff[2])
LOG_ADD(LOG_FLOAT, d03, &uwbTdoaDistDiff[3])
LOG_ADD(LOG_FLOAT, d04, &uwbTdoaDistDiff[4])
LOG_ADD(LOG_FLOAT, d05, &uwbTdoaDistDiff[5])
LOG_ADD(LOG_FLOAT, d06, &uwbTdoaDistDiff[6])
LOG_ADD(LOG_FLOAT, d07, &uwbTdoaDistDiff[7])
LOG_GROUP_STOP(tdoa)
