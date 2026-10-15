#ifndef _KEYBOARD_LINK_WIRE_H
#define _KEYBOARD_LINK_WIRE_H

// Mirrors ../../common/keyboard_link.h, which cannot be included here: that
// header pulls in <stdint.h>, and the NONOS SDK's c_types.h declares uint32_t
// as "unsigned long" where newlib uses "unsigned int", so the typedefs collide.
// Change both files together and bump the version.

#include <c_types.h>
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

static inline uint16_t keyboard_link_get_sequence( const keyboard_link_packet_t *packet )
{
	return (uint16_t)packet->sequence_le[0] | ((uint16_t)packet->sequence_le[1] << 8);
}

#endif
