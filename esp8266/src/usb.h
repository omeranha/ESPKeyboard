#ifndef _USB_H
#define _USB_H

#include <usb_config.h>

#define USB_LOW_SPEED

#define USB_BUFFERSIZE 12  //Must be big enough to hold PID + DATA + CRC + EOF

#define USB_OFFSET_BUFFER		0
#define USB_OFFSET_DSTATUS		(USB_BUFFERSIZE)
#define USB_OFFSET_PACKET_SIZE  (USB_OFFSET_DSTATUS)
#define USB_OFFSET_LAST_TOKEN   (USB_OFFSET_DSTATUS+4)
#define USB_OFFSET_DEBUG        (USB_OFFSET_DSTATUS+8)
#define USB_OFFSET_DIAG         (USB_OFFSET_DSTATUS+12)
#define USB_OFFSET_DECODE_LEN   (USB_OFFSET_DSTATUS+44)

//Reasons the receive interrupt gave up, counted in usb_internal_state.diag.
//The assembly bumps these on the paths that leave the bit loop, so a device
//that will not enumerate says which stage failed instead of only that nothing
//arrived. usb_asm_1bit.S reads these indices too, so they live outside the
//_INASM_ guard.
#define USB_DIAG_NO_HIGH      0  //Never saw D+ high: woke up outside a packet.
#define USB_DIAG_NO_LOW       1  //Saw D+ high but no falling edge to lock to.
#define USB_DIAG_BIT_TIMEOUT  2  //Bit loop ran past the longest legal packet.
#define USB_DIAG_LATE_LOCK    3  //Locked on, decoded nothing: entered after SYNC.
#define USB_DIAG_CRC_FAIL     4  //Decoded a packet whose CRC did not check.
#define USB_DIAG_OVERFLOW     5  //More bytes decoded than usb_buffer holds.
#define USB_DIAG_BAD_PID      6  //Good CRC, but the PID is not one we handle.
#define USB_DIAG_DISPATCH     7  //Packets that passed CRC and reached dispatch.
#define USB_DIAG_COUNT        8

#ifndef _INASM_
#include <c_types.h>
#include <stddef.h>

#define EMPTY_SEND_BUFFER (uint8_t*)1

typedef enum
{
	USB_OK = 0,
	USB_ERR_BUSY = -1,		//Endpoint is still transmitting the previous report.
	USB_ERR_INVALID_ARG = -2,
} usb_result_t;

struct usb_endpoint
{
	const uint8_t * ptr_in;		// Pointer to "IN" data (US->PC)
	uint16_t size_in;		// Total size of the structure pointed to by ptr_in
	uint16_t advance_in;		// How much data was sent this packet? (How much to advance in ack)
	uint16_t place_in;		// Where in the ptr_in we are currently pointing.
	uint8_t toggle_in; 		// DATA0 or DATA1?
	uint8_t send;			// Sets back to 0 when done sending.
	int * transfer_in_done_ptr;

	uint8_t * ptr_out;
	int * transfer_done_ptr;  //Written to # of bytes received when a datagram is done.
	uint16_t max_size_out;
	uint16_t got_size_out;
	uint8_t toggle_out;  //Out PC->US
};

struct usb_internal_state_struct
{
	//This data is modified by the super low-level code.

	uint8_t usb_buffer[USB_BUFFERSIZE];

	uint32_t packet_size; //Of data currently in usb_buffer
	uint32_t last_token;
	volatile uint32_t debug; //GPIO interrupt entries, incremented by the USB ISR.
	volatile uint32_t diag[USB_DIAG_COUNT]; //Why reception gave up, see USB_DIAG_*.
	volatile uint32_t last_decode_len; //Bytes decoded by the last failed packet.

	struct usb_endpoint * ce;  //Current endpoint (set by IN/OUT PIDs)
	struct usb_endpoint eps[ENDPOINTS];

	//Things past here are addressable by C.

	uint32_t my_address;	//For the current address set up by the setup portion of USB.
	uint32_t setup_request; //1 if needing setup packet.
	uint32_t there_is_a_host; //1 if there is a host at all, i.e. enumeration attempts have begun.
	volatile uint32_t setup_token_count;
	volatile uint32_t setup_packet_count;
	volatile uint32_t last_setup_request;
	volatile uint32_t last_setup_value;
	volatile uint32_t last_setup_index;
	volatile uint32_t last_setup_length;
	uint32_t pending_address;
	uint32_t address_pending;
	volatile uint32_t configuration; //Nonzero after the host selects a configuration.
};

extern struct usb_internal_state_struct usb_internal_state __attribute__((aligned(4)));

//Functions that must be supplied by user.
void usb_handle_custom_control( int bmRequestType, int bRequest, int wLength, struct usb_internal_state_struct * ist );

//Functions within this suite.
//Only usb_init may live in flash; everything else runs from the interrupt.
void ICACHE_FLASH_ATTR usb_init( void );

//Hand eight bytes (modifier, reserved, six key usages) to the interrupt IN
//endpoint. Returns USB_ERR_BUSY while the previous report is still going out,
//so keep a single producer for this endpoint and retry.
usb_result_t usb_send_keyboard_report( const uint8_t report[8] );

//Latest keyboard LED state the host sent through SET_REPORT.
//Bit 0 Num Lock, bit 1 Caps Lock, bit 2 Scroll Lock, bit 3 Compose, bit 4 Kana.
uint8_t usb_get_keyboard_leds( void );

//This function is provided in assembly.
//Declared with the ISR signature ets_isr_attach expects; the handler ignores
//the argument.
extern void gpio_intr( void *arg );

//poly_function = 0 to include CRC.
//poly_function = 2 to exclude CRC.
//This function is provided in assembly
extern void usb_send_data( uint8_t * data, uint32_t length, uint32_t poly_function );

void usb_pid_handle_setup( uint32_t this_token, struct usb_internal_state_struct * ist );
void usb_pid_handle_sof( uint32_t this_token, struct usb_internal_state_struct * ist );
void usb_pid_handle_in( uint32_t this_token, struct usb_internal_state_struct * ist );
void usb_pid_handle_out( uint32_t this_token, struct usb_internal_state_struct * ist );
void usb_pid_handle_data( uint32_t this_token, struct usb_internal_state_struct * ist, uint32_t which_data );
void usb_pid_handle_ack( uint32_t this_token, struct usb_internal_state_struct * ist );

extern uint32_t usb_reinstate;

#else

.global usb_send_data;
.global usb_internal_state;
.global usb_reinstate;
.global usb_pid_handle_setup
.global usb_pid_handle_sof
.global usb_pid_handle_in
.global usb_pid_handle_out
.global usb_pid_handle_data
.global usb_pid_handle_ack
#endif



#define _BV(x) ((1)<<(x))

#if defined( _INASM_ ) || defined( NEED_CONSTANTS ) 

#define GPIO_BASE_REG       0x60000300
#define GPIO_OFFSET_OUT     0x00 
#define GPIO_OFFSET_SET     0x04
#define GPIO_OFFSET_CLEAR   0x08
#define GPIO_OFFSET_DIR     0x0c
#define GPIO_OFFSET_DIR_OUT 0x10
#define GPIO_OFFSET_DIR_IN  0x14
#define GPIO_OFFSET_INPUT   0x18

#define GPIO_OFFSET_GPIO_STATUS  0x1c
#define GPIO_OFFSET_GPIO_STATUS_W1TC 0x24

#endif


#if defined( _INASM_ )

.global usb_ramtable

#else


//Detailed analysis of some useful stuff and performance tweaking: http://naberius.de/2015/05/14/esp8266-gpio-output-performance/
//Reverse engineerd boot room can be helpful, too: http://cholla.mmto.org/esp8266/bootrom/boot.txt
//USB Protocol read from wikipedia: https://en.wikipedia.org/wiki/USB
// Neat stuff: http://www.usbmadesimple.co.uk/ums_3.htm
// Neat stuff: http://www.beyondlogic.org/usbnutshell/usb1.shtml

struct usb_urb
{
	uint8_t pktp;
	uint8_t bmRequestType;
	uint8_t bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
} __attribute__((packed));

#endif

#endif
