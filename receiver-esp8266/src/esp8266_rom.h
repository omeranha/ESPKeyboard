#ifndef _ESP8266_ROM_H
#define _ESP8266_ROM_H

// NONOS SDK 1.5.4 refers to these ROM routines from macros in ets_sys.h and
// osapi.h but never declares them, so every caller gets an implicit
// declaration unless the application supplies prototypes. Getting them right
// matters here: an implicitly declared function is assumed to return int and
// take unpromoted arguments, which silently misdescribes the pointer-taking
// ones.

#include <c_types.h>
#include <ets_sys.h>
#include <stddef.h>

// Interrupt plumbing, used by ETS_GPIO_INTR_ATTACH / _ENABLE / _DISABLE.
void ets_isr_attach( int interrupt_number, void (*handler)(void *), void *arg );
void ets_isr_mask( uint32_t mask );
void ets_isr_unmask( uint32_t mask );

// Busy-wait. Safe to call before the scheduler is running.
void ets_delay_us( uint32_t microseconds );

// ROM memcpy. Preferred inside the USB interrupt: it is in ROM, so unlike a
// flash-resident libc memcpy it cannot stall on an instruction cache miss.
void *ets_memcpy( void *dest, const void *src, size_t length );
int ets_memcmp( const void *left, const void *right, size_t length );
void *ets_memset( void *dest, int value, size_t length );

// Software timers, behind the os_timer_* macros in osapi.h.
void ets_timer_arm_new( ETSTimer *timer, uint32_t value, bool repeat, bool is_milliseconds );
void ets_timer_disarm( ETSTimer *timer );
void ets_timer_setfn( ETSTimer *timer, ETSTimerFunc *function, void *arg );

// Backs the os_printf macro.
int os_printf_plus( const char *format, ... ) __attribute__((format(printf, 1, 2)));

#endif
