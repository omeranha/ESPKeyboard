// ESPKeyboard ESP8266 receiver, NONOS SDK 1.5.4.
//
// Receives complete keyboard reports from the paired ESP32-S3 over ESP-NOW and
// replays them to a USB host as a low-speed boot-protocol HID keyboard.

#include <c_types.h>
#include <espnow.h>
#include <ets_sys.h>
#include <osapi.h>
#include <user_interface.h>

#include "esp8266_rom.h"
#include "espkeyboard_config.h"
#include "keyboard_link.h"
#include "usb.h"

// In ROM, declared by no SDK header.
extern void uart_div_modify( uint8 uart_no, uint32 divisor );

#define UART_CLOCK_HZ  80000000
#define LOG_BAUD       115200

#define STATUS_INTERVAL_MS 1000

// The transmitter sends a snapshot per change plus a refresh every 100 ms, and
// the host collects one report per 10 ms frame, so this only has to absorb a
// short burst of fast typing.
#define REPORT_QUEUE_DEPTH 8

static volatile os_timer_t status_timer;
static volatile os_timer_t pump_timer;

static uint8_t paired_mac[6];

static uint8_t last_leds;
static uint32_t last_irq_count;
static bool was_configured;
static bool radio_started;
static uint32_t boot_time_us;

// Written by the ESP-NOW receive callback, drained by the pump timer. Both run
// in task context on a single core, never from the USB interrupt, so the queue
// needs no locking.
static uint8_t report_queue[REPORT_QUEUE_DEPTH][KEYBOARD_LINK_REPORT_SIZE];
static uint8_t queue_head;
static uint8_t queue_tail;

static uint8_t last_queued[KEYBOARD_LINK_REPORT_SIZE];
static bool have_last_queued;

static uint8_t current_report[KEYBOARD_LINK_REPORT_SIZE];
static bool report_pending;

static uint32_t last_packet_us;
static bool have_packet;

static uint16_t last_sequence;
static bool have_sequence;
static uint32_t stale_packets;

// Reported once a second rather than logged inline: at ten packets a second,
// logging each event would drown the port and add work to the path that feeds
// the USB endpoint.
static uint32_t rx_total;
static uint32_t rx_accepted;
static uint32_t rx_wrong_peer;
static uint32_t rx_bad_format;
static uint32_t rx_duplicate;
static uint32_t rx_not_configured;
static uint32_t link_gaps;
static uint32_t link_resyncs;
static uint32_t queue_overflows;
static uint32_t reports_sent;
static uint32_t timeout_releases;

void usb_handle_custom_control( int bmRequestType, int bRequest, int wLength, struct usb_internal_state_struct * ist )
{
	(void)bmRequestType;
	(void)bRequest;
	(void)wLength;
	(void)ist;
}

static uint32_t ICACHE_FLASH_ATTR ms_since( uint32_t past_us )
{
	// system_get_time() wraps about every 71 minutes; unsigned subtraction
	// stays correct across the wrap.
	return ( system_get_time() - past_us ) / 1000;
}

static bool ICACHE_FLASH_ATTR queue_is_empty( void )
{
	return queue_head == queue_tail;
}

static void ICACHE_FLASH_ATTR queue_reset( void )
{
	queue_head = 0;
	queue_tail = 0;
	have_last_queued = false;
}

static void ICACHE_FLASH_ATTR queue_push( const uint8_t report[KEYBOARD_LINK_REPORT_SIZE] )
{
	uint8_t next = ( uint8_t )( ( queue_tail + 1 ) % REPORT_QUEUE_DEPTH );

	if( next == queue_head )
	{
		// Drop the oldest rather than the newest: every packet carries the
		// complete state, so the latest one is the most useful.
		queue_overflows++;
		queue_head = ( uint8_t )( ( queue_head + 1 ) % REPORT_QUEUE_DEPTH );
	}

	ets_memcpy( report_queue[queue_tail], report, KEYBOARD_LINK_REPORT_SIZE );
	queue_tail = next;
}

static bool ICACHE_FLASH_ATTR queue_pop( uint8_t report[KEYBOARD_LINK_REPORT_SIZE] )
{
	if( queue_is_empty() ) return false;

	ets_memcpy( report, report_queue[queue_head], KEYBOARD_LINK_REPORT_SIZE );
	queue_head = ( uint8_t )( ( queue_head + 1 ) % REPORT_QUEUE_DEPTH );
	return true;
}

static bool ICACHE_FLASH_ATTR sequence_is_newer( uint16_t candidate, uint16_t previous )
{
	return (int16_t)( candidate - previous ) > 0;
}

static void ICACHE_FLASH_ATTR espnow_recv_cb( uint8 *mac_addr, uint8 *data, uint8 len )
{
	keyboard_link_packet_t packet;
	uint16_t sequence;

	rx_total++;

	if( mac_addr == NULL || data == NULL )
	{
		rx_bad_format++;
		return;
	}
	if( ets_memcmp( mac_addr, paired_mac, sizeof( paired_mac ) ) != 0 )
	{
		rx_wrong_peer++;
		return;
	}
	if( len != sizeof( packet ) )
	{
		rx_bad_format++;
		return;
	}

	// The payload is not guaranteed to be aligned, so work on a copy.
	ets_memcpy( &packet, data, sizeof( packet ) );
	if( packet.version != KEYBOARD_LINK_VERSION )
	{
		rx_bad_format++;
		return;
	}

	sequence = keyboard_link_get_sequence( &packet );

	if( have_sequence && !sequence_is_newer( sequence, last_sequence ) )
	{
		// A transmitter reboot restarts its counter at zero, which looks like a
		// run of stale packets. Resynchronise rather than discarding its
		// reports indefinitely.
		if( ++stale_packets < SEQUENCE_RESYNC_AFTER )
		{
			rx_duplicate++;
			return;
		}
		link_resyncs++;
	}
	else if( have_sequence && (uint16_t)( sequence - last_sequence ) != 1 )
	{
		link_gaps++;
	}

	stale_packets = 0;
	have_sequence = true;
	last_sequence = sequence;

	// Mark the link alive even when the report is not usable yet, so the
	// timeout only fires for a genuinely silent transmitter.
	last_packet_us = system_get_time();
	have_packet = true;
	rx_accepted++;

	if( usb_internal_state.configuration == 0 )
	{
		// Dropping the snapshot keeps the queue from filling with keystrokes
		// that would all be replayed at once when a host shows up; the next
		// refresh catches us up within 100 ms.
		rx_not_configured++;
		return;
	}

	// The transmitter refreshes the same report every 100 ms, so only queue a
	// state that actually differs or the host is handed a stream of repeats.
	if( have_last_queued && ets_memcmp( last_queued, packet.report, KEYBOARD_LINK_REPORT_SIZE ) == 0 )
	{
		return;
	}

	ets_memcpy( last_queued, packet.report, KEYBOARD_LINK_REPORT_SIZE );
	have_last_queued = true;
	queue_push( packet.report );
}

static bool ICACHE_FLASH_ATTR report_is_idle( const uint8_t report[KEYBOARD_LINK_REPORT_SIZE] )
{
	int i;
	for( i = 0; i < KEYBOARD_LINK_REPORT_SIZE; i++ )
	{
		if( report[i] != 0 ) return false;
	}
	return true;
}

static void ICACHE_FLASH_ATTR pump_tick( void *arg )
{
	uint8_t next_report[KEYBOARD_LINK_REPORT_SIZE];

	(void)arg;

	if( usb_internal_state.configuration == 0 )
	{
		// Forget everything queued so a later host does not inherit keys held
		// down before it arrived.
		queue_reset();
		ets_memset( current_report, 0, sizeof( current_report ) );
		report_pending = false;
		return;
	}

	if( !report_pending && queue_pop( next_report ) )
	{
		if( ets_memcmp( current_report, next_report, sizeof( current_report ) ) != 0 )
		{
			ets_memcpy( current_report, next_report, sizeof( current_report ) );
			report_pending = true;
		}
	}

	// A transmitter that disappears mid-keypress must not leave that key held
	// down on the host.
	if( !report_is_idle( current_report ) && have_packet && ms_since( last_packet_us ) >= TRANSMITTER_TIMEOUT_MS )
	{
		queue_reset();
		ets_memset( current_report, 0, sizeof( current_report ) );
		report_pending = true;
		timeout_releases++;
		os_printf( "link: transmitter timed out, releasing all keys\r\n" );
	}

	if( report_pending && usb_send_keyboard_report( current_report ) == USB_OK )
	{
		report_pending = false;
		reports_sent++;
	}
}

static void ICACHE_FLASH_ATTR start_radio( void )
{
	uint8_t local_mac[6];

	radio_started = true;

	if( ESPNOW_CHANNEL < 1 || ESPNOW_CHANNEL > 13 )
	{
		os_printf( "link: ESPNOW_CHANNEL must be 1..13, radio stays off\r\n" );
		return;
	}
	if( !keyboard_link_parse_mac( ESPNOW_PEER_MAC, paired_mac ) )
	{
		os_printf( "link: peer MAC \"%s\" is not a usable station address; radio stays off\r\n", ESPNOW_PEER_MAC );
		os_printf( "link: pass it as build.ps1 -PeerMac aa:bb:cc:dd:ee:ff, or set ESPNOW_PEER_MAC in src/espkeyboard_config.h\r\n" );
		return;
	}

	wifi_set_opmode_current( STATION_MODE );
	// Associating would move us off the ESP-NOW channel, and scanning adds long
	// bursts of radio work.
	wifi_station_set_auto_connect( 0 );
	wifi_station_disconnect();
	// Modem sleep would both miss frames and disturb the USB bit timing.
	wifi_set_sleep_type( NONE_SLEEP_T );

	if( !wifi_set_channel( ESPNOW_CHANNEL ) )
	{
		os_printf( "link: could not select channel %d\r\n", ESPNOW_CHANNEL );
		return;
	}

	if( wifi_get_macaddr( STATION_IF, local_mac ) )
	{
		os_printf( "link: station MAC %02x:%02x:%02x:%02x:%02x:%02x channel %d\r\n", local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5], ESPNOW_CHANNEL );
	}
	os_printf( "link: paired transmitter %02x:%02x:%02x:%02x:%02x:%02x\r\n", paired_mac[0], paired_mac[1], paired_mac[2], paired_mac[3], paired_mac[4], paired_mac[5] );

	if( esp_now_init() != 0 )
	{
		os_printf( "link: esp_now_init failed\r\n" );
		return;
	}
	// This SDK's ESP-NOW is the older role-based API: we only ever receive, so
	// this side is the slave and the transmitter is the controller.
	if( esp_now_set_self_role( ESP_NOW_ROLE_SLAVE ) != 0 )
	{
		os_printf( "link: esp_now_set_self_role failed\r\n" );
		return;
	}
	if( esp_now_register_recv_cb( espnow_recv_cb ) != 0 )
	{
		os_printf( "link: esp_now_register_recv_cb failed\r\n" );
		return;
	}
	if( esp_now_add_peer( (uint8 *)paired_mac, ESP_NOW_ROLE_CONTROLLER, ESPNOW_CHANNEL, NULL, 0 ) != 0 )
	{
		os_printf( "link: esp_now_add_peer failed\r\n" );
		return;
	}

	os_timer_disarm( (os_timer_t *)&pump_timer );
	os_timer_setfn( (os_timer_t *)&pump_timer, (os_timer_func_t *)pump_tick, NULL );
	os_timer_arm( (os_timer_t *)&pump_timer, REPORT_PUMP_INTERVAL_MS, 1 );

	os_printf( "link: ESP-NOW up, report pump running every %d ms\r\n", REPORT_PUMP_INTERVAL_MS );
}

// These counters come from the receive interrupt, so they separate "the host
// never sent anything" from "packets arrive but the bit decoder is losing
// them". uint32_t is unsigned long here, so every value is cast for printf.
static void ICACHE_FLASH_ATTR log_usb_state( void )
{
	struct usb_internal_state_struct * ist = &usb_internal_state;

	os_printf( "usb: cfg=%u addr=%u host=%u irqs=%u setup tokens=%u packets=%u\r\n", (unsigned)ist->configuration, (unsigned)ist->my_address,
			(unsigned)ist->there_is_a_host, (unsigned)ist->debug, (unsigned)ist->setup_token_count, (unsigned)ist->setup_packet_count );
	os_printf( "  decoder: no-high=%u no-low=%u late-lock=%u bit-timeout=%u crc-fail=%u overflow=%u bad-pid=%u ok=%u last len=%u\r\n",
			(unsigned)ist->diag[USB_DIAG_NO_HIGH], (unsigned)ist->diag[USB_DIAG_NO_LOW], (unsigned)ist->diag[USB_DIAG_LATE_LOCK],
			(unsigned)ist->diag[USB_DIAG_BIT_TIMEOUT], (unsigned)ist->diag[USB_DIAG_CRC_FAIL], (unsigned)ist->diag[USB_DIAG_OVERFLOW],
			(unsigned)ist->diag[USB_DIAG_BAD_PID], (unsigned)ist->diag[USB_DIAG_DISPATCH], (unsigned)ist->last_decode_len );
	os_printf( "  last setup=%02x/%02x value=%04x index=%04x length=%u\r\n", (unsigned)((ist->last_setup_request >> 8) & 0xff),
			(unsigned)(ist->last_setup_request & 0xff), (unsigned)ist->last_setup_value, (unsigned)ist->last_setup_index, (unsigned)ist->last_setup_length );
}

static void ICACHE_FLASH_ATTR log_link_state( void )
{
	os_printf( "link: rx=%u ok=%u peer=%u fmt=%u dup=%u nocfg=%u gaps=%u resync=%u qfull=%u sent=%u release=%u\r\n", (unsigned)rx_total,
			(unsigned)rx_accepted, (unsigned)rx_wrong_peer, (unsigned)rx_bad_format, (unsigned)rx_duplicate, (unsigned)rx_not_configured,
			(unsigned)link_gaps, (unsigned)link_resyncs, (unsigned)queue_overflows, (unsigned)reports_sent, (unsigned)timeout_releases );
	os_printf( "  report=%02x %02x %02x %02x %02x %02x %02x %02x last packet %u ms ago\r\n", current_report[0], current_report[1],
			current_report[2], current_report[3], current_report[4], current_report[5], current_report[6], current_report[7],
			have_packet ? (unsigned)ms_since( last_packet_us ) : 0u );
}

static void ICACHE_FLASH_ATTR status_tick( void *arg )
{
	struct usb_internal_state_struct * ist = &usb_internal_state;
	bool configured = ist->configuration != 0;
	uint8_t leds;

	(void)arg;

	if( configured != was_configured )
	{
		was_configured = configured;
		if( configured )
		{
			os_printf( "usb: host selected configuration %u, address %u\r\n", (unsigned)ist->configuration, (unsigned)ist->my_address );
		}
		else
		{
			os_printf( "usb: configuration dropped\r\n" );
		}
	}

	leds = usb_get_keyboard_leds();
	if( leds != last_leds )
	{
		last_leds = leds;
		os_printf( "usb: host LEDs=0x%02x (num=%u caps=%u scroll=%u)\r\n", leds, leds & 1, (leds >> 1) & 1, (leds >> 2) & 1 );
	}

	// Wi-Fi interrupts are the most likely thing to disturb the bit-banged USB
	// timing, so hold the radio off until USB has had its chance, then bring it
	// up whether or not a host turned up.
	if( !radio_started && ( configured || ms_since( boot_time_us ) >= RADIO_START_GRACE_MS ) )
	{
		os_printf( "link: starting radio (%s)\r\n", configured ? "USB configured" : "grace period elapsed" );
		start_radio();
	}

	if( !configured && ist->debug != last_irq_count )
	{
		last_irq_count = ist->debug;
		log_usb_state();
	}
	else if( radio_started )
	{
		log_link_state();
	}
}

void user_rf_pre_init( void )
{
	// Required by libmain.
}

void ICACHE_FLASH_ATTR user_init( void )
{
	struct rst_info * r;

	uart_div_modify( 0, UART_CLOCK_HZ / LOG_BAUD );

	// usb_asm_1bit.S counts CPU cycles to recover the 1.5 Mbit/s bit clock, so
	// this has to stay at 80 MHz.
	system_update_cpu_freq( 80 );
	boot_time_us = system_get_time();

	os_printf( "\r\n\r\nESPKeyboard ESP8266 receiver (NONOS %s)\r\n", system_get_sdk_version() );
	r = system_get_rst_info();
	os_printf( "reset reason=%u exccause=%u epc1=0x%08x excvaddr=0x%08x\r\n", (unsigned)r->reason, (unsigned)r->exccause, (unsigned)r->epc1, (unsigned)r->excvaddr );

	// status_tick decides when USB has had its chance and starts the radio.
	wifi_set_opmode_current( NULL_MODE );

	os_printf( "usb: D-=GPIO%d D+=GPIO%d, low speed, boot keyboard\r\n", DMINUS, DPLUS );
	usb_init();

	os_timer_disarm( (os_timer_t *)&status_timer );
	os_timer_setfn( (os_timer_t *)&status_timer, (os_timer_func_t *)status_tick, NULL );
	os_timer_arm( (os_timer_t *)&status_timer, STATUS_INTERVAL_MS, 1 );

	os_printf( "boot ok\r\n" );
}
