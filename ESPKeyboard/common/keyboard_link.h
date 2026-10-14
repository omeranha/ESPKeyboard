#ifndef KEYBOARD_LINK_H
#define KEYBOARD_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KEYBOARD_LINK_VERSION 1
#define KEYBOARD_LINK_REPORT_SIZE 8

typedef struct {
    uint8_t version;
    uint8_t sequence_le[2];
    uint8_t report[KEYBOARD_LINK_REPORT_SIZE];
} keyboard_link_packet_t;

static inline uint16_t keyboard_link_get_sequence(const keyboard_link_packet_t *packet)
{
    return (uint16_t)packet->sequence_le[0] |
            ((uint16_t)packet->sequence_le[1] << 8);
}

static inline void keyboard_link_set_sequence(keyboard_link_packet_t *packet,
        uint16_t sequence)
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

static inline bool keyboard_link_parse_mac(const char *text, uint8_t mac[6])
{
    if (text == NULL || mac == NULL || strlen(text) != 17) return false;

    for (size_t i = 0; i < 6; ++i) {
        size_t offset = i * 3;
        int high = keyboard_link_hex_nibble(text[offset]);
        int low = keyboard_link_hex_nibble(text[offset + 1]);
        if (high < 0 || low < 0 || (i < 5 && text[offset + 2] != ':')) {
            return false;
        }
        mac[i] = (uint8_t)((high << 4) | low);
    }

    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < 6; ++i) {
        all_zero = all_zero && mac[i] == 0;
        all_ff = all_ff && mac[i] == 0xff;
    }
    return !all_zero && !all_ff && (mac[0] & 1) == 0;
}

#endif
