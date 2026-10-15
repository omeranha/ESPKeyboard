#ifndef KEYBOARD_LINK_H
#define KEYBOARD_LINK_H

// Shared ESP-NOW wire format and pairing helpers for both boards.
//
// The ESP8266 receiver builds against the NONOS SDK, which declares the
// fixed-width types itself: its uint32_t is "unsigned long" where newlib uses
// "unsigned int", so including both <stdint.h> and c_types.h collides with a
// redefinition error. That firmware also links with -nostdlib, so nothing here
// may call into a C library.
#ifdef ESPKEYBOARD_NONOS_SDK
#include <c_types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#include <stddef.h>

#define KEYBOARD_LINK_VERSION 1
#define KEYBOARD_LINK_REPORT_SIZE 8

typedef struct {
    uint8_t version;
    uint8_t sequence_le[2];
    uint8_t report[KEYBOARD_LINK_REPORT_SIZE];
} keyboard_link_packet_t;

// The transmitter writes this struct straight onto the air with no padding, so
// the layout is part of the protocol rather than an implementation detail.
_Static_assert(sizeof(keyboard_link_packet_t) == 11, "keyboard_link_packet_t must stay 11 bytes on the wire");
_Static_assert(offsetof(keyboard_link_packet_t, version) == 0, "version must be the first byte on the wire");
_Static_assert(offsetof(keyboard_link_packet_t, sequence_le) == 1, "sequence_le must follow version");
_Static_assert(offsetof(keyboard_link_packet_t, report) == 3, "report must follow sequence_le");

static inline uint16_t keyboard_link_get_sequence(const keyboard_link_packet_t *packet)
{
    return (uint16_t)packet->sequence_le[0] | ((uint16_t)packet->sequence_le[1] << 8);
}

static inline void keyboard_link_set_sequence(keyboard_link_packet_t *packet, uint16_t sequence)
{
    packet->sequence_le[0] = (uint8_t)sequence;
    packet->sequence_le[1] = (uint8_t)(sequence >> 8);
}

static inline int keyboard_link_hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

// Parses "aa:bb:cc:dd:ee:ff" and rejects anything that cannot be a peer's
// station MAC. Walks the text one character at a time instead of measuring it
// first, so it needs no C library and never reads past the terminator.
static inline bool keyboard_link_parse_mac(const char *text, uint8_t mac[6])
{
    if (text == NULL || mac == NULL) return false;

    for (size_t i = 0; i < 6; ++i) {
        int high = keyboard_link_hex_nibble(text[i * 3]);
        if (high < 0) return false;
        int low = keyboard_link_hex_nibble(text[i * 3 + 1]);
        if (low < 0) return false;

        char separator = text[i * 3 + 2];
        if (i < 5 ? separator != ':' : separator != '\0') return false;

        mac[i] = (uint8_t)((high << 4) | low);
    }

    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < 6; ++i) {
        all_zero = all_zero && mac[i] == 0x00;
        all_ff = all_ff && mac[i] == 0xff;
    }
    // A station MAC is always unicast, so reject the unset default, broadcast,
    // and any multicast address.
    return !all_zero && !all_ff && (mac[0] & 1) == 0;
}

#endif
