#ifndef LAMPGO_MIC_STREAM_H
#define LAMPGO_MIC_STREAM_H

#include <stdint.h>

#include "esp_http_server.h"

namespace MicStream {

bool begin();
void stop();
bool isRunning();
bool registerWsHandler(httpd_handle_t server);
bool isAecReady();
int clientCount();
uint32_t bytesRead();
uint32_t framesSent();
bool claimOwner(const char *ownerId, uint32_t ttlMs);
bool releaseOwner(const char *ownerId);
void closeClients();
const char *activeOwner();
uint32_t ownerLeaseRemainingMs();
bool isWakeReady();
const char *wakeModel();
const char *requestedWakeModel();
bool setWakeModel(const char *modelName);
uint32_t wakeDetections();
uint32_t lastWakeMs();
int wakeEventClientCount();
float wakeThreshold();
float wakeAfeGain();
int wakeDetectionMode();
uint32_t afeFetches();
uint32_t lastAfeMs();
int lastWakeupState();
int lastWakeWordIndex();
int lastTriggerChannel();
int lastVadState();
float lastAfeVolumeDb();
float lastAfeRingFreePct();
uint32_t lastMicRms();
uint32_t lastMicPeak();
uint32_t afeFeeds();
uint32_t afeFetchAttempts();
uint32_t afeFetchNulls();
int lastAfeRet();
int afeFeedSamples();
int afeFeedChannels();
int pushTaskCreateResult();
int fetchTaskCreateResult();
bool isInlineFetch();

}

#endif
