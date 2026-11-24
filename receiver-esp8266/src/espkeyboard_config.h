#ifndef _ESPKEYBOARD_CONFIG_H
#define _ESPKEYBOARD_CONFIG_H

// Written by build.ps1 from its -PeerMac and -Channel switches. Always present
// because the script regenerates it before every compile.
#include "espkeyboard_build_config.h"

// Station MAC of the paired ESP32-S3, which it prints at boot as
// "Station MAC: ...". Reports from any other sender are ignored.
//
// Prefer passing it per build so it stays out of version control:
//     .\build.ps1 -PeerMac f0:9e:9e:21:9d:98
// The script remembers it for later builds. Setting it here instead also
// works. The radio stays off until it is a valid unicast address.
//
// This is pairing, not authentication: ESP-NOW frames here are unencrypted and
// a MAC address can be spoofed. Use these boards in a trusted RF environment.
#ifndef ESPNOW_PEER_MAC
#define ESPNOW_PEER_MAC "00:00:00:00:00:00"
#endif

// Must match CONFIG_ESPNOW_CHANNEL on the transmitter. Legal 1..13. ESP-NOW
// peers only hear each other on the same channel, and this firmware never
// associates with an access point, so the channel is pinned by hand.
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif

// Release every key if the transmitter goes quiet for this long. It refreshes
// the current report every 100 ms even when nothing changes, so this is about
// fifteen missed refreshes. Without it, losing the transmitter mid-keypress
// would leave that key held down on the host forever.
#define TRANSMITTER_TIMEOUT_MS 1500

// A transmitter reboot restarts its sequence counter at zero, which looks like
// a run of stale packets. Tolerate this many before concluding it restarted
// and resynchronising, rather than discarding its reports indefinitely.
#define SEQUENCE_RESYNC_AFTER 8

// How often the pump hands a queued report to the USB endpoint. The endpoint's
// bInterval is 10 ms, so the host polls slower than this anyway.
#define REPORT_PUMP_INTERVAL_MS 2

// Give USB a chance to enumerate before the radio is switched on, since Wi-Fi
// interrupts are the most likely thing to disturb the bit-banged USB timing.
// The radio also starts as soon as a host selects a configuration, so this is
// only the upper bound on the wait.
#define RADIO_START_GRACE_MS 3000

#endif
