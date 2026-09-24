#pragma once
#include <Arduino.h>

// MQTTP: request/response over MQTT, the same protocol the backend uses to ask
// a device about itself.
//
//   request  ->  <Device>/console/controlled/<id>/in    (the command text)
//   reply    <-  <Device>/console/controlled/<id>/out
//
// A reply of 512 bytes or less arrives whole; anything larger is chunked by the
// sender as ";;<n>/<total>;;<data>" and reassembled here.
//
// Two notes on how this differs from the backend's implementation:
//
//  * Listening is per *device* and lasts as long as the page is open, not per
//    request. The library no longer takes "#", so replies have to be subscribed
//    for -- but subscribing per request does not work: esp_mqtt_client_subscribe
//    only queues a SUBSCRIBE, and at QoS 0 a device answering in a millisecond
//    beats the broker's SUBACK over a TLS link every time, so the reply is
//    dropped before anyone is listening for it. Mqttp_listenTo() therefore takes
//    "<device>/console/controlled/+/out" when the page opens and gives it back
//    when the page is left. The wildcard is one device wide, and the request id
//    still does the correlating.
//
//  * One request at a time, into a single fixed-size block that is never grown:
//    allocated when a request is made, freed by Mqttp_cancel(). The panel is a
//    viewer, not a store: it holds one answer for one device for as long as it
//    is on screen, and nothing accumulates. Reassembling chunks into a growing
//    String would fragment the heap, which on this board means the next mbedTLS
//    handshake fails; holding the block permanently would take it from that
//    same handshake. Mqttp_request() returns false if it cannot be allocated.

/// Cap on a reassembled reply. Real answers run 200B to ~4KB; the backend's
/// 256KB ceiling is a backend limit, not a device reality. Anything longer is
/// truncated rather than dropped -- a partial answer beats an error.
#define MQTTP_RESPONSE_MAX 3072

/// How long to wait before giving up. Much shorter than the backend's 20s:
/// someone is standing in front of this screen waiting for it.
#define MQTTP_TIMEOUT_MS 8000

enum MqttpState
{
    MQTTP_IDLE = 0,
    MQTTP_PENDING,
    MQTTP_DONE,
    MQTTP_TIMEOUT,
    MQTTP_FAILED
};

/// @brief Start listening for one device's correlated replies, well before any
/// request is made. Idempotent, and it replaces whatever was being listened to.
/// @return False if the subscription was refused.
bool Mqttp_listenTo(const String &device);

/// @brief Give the subscription back. Also abandons anything in flight.
void Mqttp_stopListening();

/// @brief Send a command to a device and wait for its reply.
/// Replaces any request already in flight.
/// @param device Device name in the exact casing it publishes under -- the
/// firmware derives the reply topic from the request topic, and the panel has
/// to have subscribed to that exact string.
/// @return False if MQTT is down, the subscription was refused, or the
/// arguments are empty.
bool Mqttp_request(const String &device, const String &command);

/// @brief Abandon any pending request and clear the buffer.
void Mqttp_cancel();

/// @brief Time out a stale request. Call regularly.
void Mqttp_tick();

/// @brief Offer an incoming MQTT message. True if it belonged to this module.
bool Mqttp_handleMessage(const String &topic, const String &payload);

MqttpState Mqttp_state();

/// @brief The reply so far, NUL-terminated. Valid until the next request.
const char *Mqttp_response();

/// @brief True when the reply hit MQTTP_RESPONSE_MAX and was cut short.
bool Mqttp_truncated();

/// @brief The command of the current or last request, for labelling the view.
const String &Mqttp_command();

/// @brief The device the current or last request was addressed to.
const String &Mqttp_device();

/// @brief Bumped whenever the state or the buffer changes.
uint32_t Mqttp_revision();
