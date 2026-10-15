#define NEED_CONSTANTS

#include "usb.h"
#include <string.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <rom/ets_sys.h>
#include <sdkconfig.h>
#include "usb_table_1bit.h"

#if CONFIG_ESP8266_DEFAULT_CPU_FREQ_MHZ != 80
#error "The bit-banged USB timing is calibrated for an 80 MHz CPU clock."
#endif

struct usb_internal_state_struct usb_internal_state __attribute__((aligned(4)));

static uint8_t keyboard_report[8];
static uint8_t keyboard_leds;
static uint8_t control_reply[2];

#define ENDPOINT0_SIZE 8 //Fixed for USB 1.1, Low Speed.

#define INSTANCE_DESCRIPTORS
#include <usb_config.h>

//Point an endpoint at the data the host asked for, clamped to the requested length.
static void IRAM_ATTR usb_arm_in_transfer( struct usb_endpoint * e, const uint8_t * data,
		uint16_t length, uint16_t requested )
{
	e->ptr_in = data;
	e->place_in = 0;
	e->size_in = ( requested < length ) ? requested : length;
	e->send = 1;
}

//USB 2.0 9.4.5: configuring the device resets the data toggle of every endpoint.
//Without this a bus reset or a host sleep/wake cycle leaves our toggles inverted
//and the host silently discards every report as a retransmission.
static void IRAM_ATTR usb_reset_data_endpoints( struct usb_internal_state_struct * ist )
{
	int i;
	for( i = 1; i < ENDPOINTS; i++ )
	{
		struct usb_endpoint * e = &ist->eps[i];
		e->toggle_in = 0;
		e->toggle_out = 0;
		e->send = 0;
		e->ptr_in = 0;
		e->place_in = 0;
		e->size_in = 0;
		e->advance_in = 0;
		e->got_size_out = 0;
	}
}

//Received a setup for a specific endpoint.
void IRAM_ATTR usb_pid_handle_setup( uint32_t this_token, struct usb_internal_state_struct * ist )
{
	uint8_t addr = (this_token>>8) & 0x7f;
	uint8_t endp = (this_token>>15) & 0xf;

	ist->there_is_a_host = 1;
	ist->setup_token_count++;

	if( endp >= ENDPOINTS ) goto end;
	if( addr != 0 && addr != ist->my_address ) goto end;

	struct usb_endpoint * e = ist->ce = &ist->eps[endp];
	if( endp == 0 ) ist->address_pending = 0;
	e->toggle_out = 0;
	e->toggle_in = 1;
	e->ptr_in = 0;
	e->send = 0;
	e->ptr_out = 0;
	e->got_size_out = 0;
	ist->setup_request = 1;
end:
	__asm__ __volatile__( "movi a0, usb_reinstate" ); //After this token, we are immediately expecting another grouping.  This short-circuits the 'return'.
}

void IRAM_ATTR usb_pid_handle_sof( uint32_t this_token, struct usb_internal_state_struct * ist )
{
}

void IRAM_ATTR usb_pid_handle_in( uint32_t this_token, struct usb_internal_state_struct * ist )
{
	uint8_t addr = (this_token>>8) & 0x7f;
	uint8_t endp = (this_token>>15) & 0xf;

	//If we get an "in" token, we have to strike any accept buffers.

	if( endp >= ENDPOINTS ) return;
	if( addr != 0 && addr != ist->my_address ) return;

	struct usb_endpoint * e = ist->ce = &ist->eps[endp];

	e->got_size_out = 0;  //Cancel any out transaction


	int tosend = 0;
	uint8_t sendnow[12];
	sendnow[0] = 0x80;

	if( e->send && e->ptr_in ) 
	{
		tosend = e->size_in - e->place_in;

		if( tosend > 8 ) tosend = 8;
	}

	if( e->toggle_in )
	{
		sendnow[1] = 0b01001011; //DATA1
	}
	else
	{
		sendnow[1] = 0b11000011; //DATA0
	}

	if( tosend == 0 || !e->send || !e->ptr_in || e->ptr_in == EMPTY_SEND_BUFFER )  //Tricky: Empty packet.
	{

		//Tricky: Control messages are not allowed to send NAKs.  We /have/ to send an empty packet for them if no more data is available.
		//With endpoints, proper, it's okay to send NAKs.

		if( endp == 0 )
		{
			sendnow[2] = 0;
			sendnow[3] = 0; //CRC = 0
			usb_send_data( sendnow, 4, 2 );  //Force a CRC
			e->ptr_in = 0;
		}
		else
		{
			sendnow[1] = 0x5a; //Empty data (NAK)
			usb_send_data( sendnow, 2, 2 );
		}
	}
	else
	{
		memcpy( sendnow+2, e->ptr_in + e->place_in, tosend );
		usb_send_data( sendnow, tosend+2, 0 );
		e->advance_in = tosend;
	}
	return;
}

void IRAM_ATTR usb_pid_handle_out( uint32_t this_token, struct usb_internal_state_struct * ist )
{
	//We need to handle this here because we could have an interrupt in the middle of a control or bulk transfer.
	//This will correctly swap back the endpoint.
	uint8_t addr = (this_token>>8) & 0x7f;
	uint8_t endp = (this_token>>15) & 0xf;
	if( endp >= ENDPOINTS ) return;
	if( addr != 0 && addr != ist->my_address ) return;
	ist->ce = &ist->eps[endp];

	__asm__ __volatile__( "movi a0, usb_reinstate" );  //After this token, we are immediately expecting another grouping.  This short-circuits the 'return'.
}

void IRAM_ATTR usb_pid_handle_data( uint32_t this_token, struct usb_internal_state_struct * ist, uint32_t which_data )
{
	//Received data from host.

	struct usb_endpoint * e = ist->ce;

	if( e == 0 ) return;

	if( e->toggle_out != which_data )
	{
		goto just_ack;
	}

	e->toggle_out = !e->toggle_out;

	if( ist->setup_request )
	{
		ist->setup_request = 0;
		struct usb_urb * s = (struct usb_urb *)ist->usb_buffer;
		ist->setup_packet_count++;
		ist->last_setup_request = ((uint32_t)s->bmRequestType << 8) | s->bRequest;
		ist->last_setup_value = s->wValue;
		ist->last_setup_index = s->wIndex;
		ist->last_setup_length = s->wLength;

		//Send just a data packet.
		e->ptr_in = EMPTY_SEND_BUFFER;
		e->place_in = 0;
		e->size_in = 0;
		e->send = 1;

		if( s->bmRequestType & 0x80 )
		{
			uint8_t recipient = s->bmRequestType & 0x1f;

			if( s->bRequest == 0x06 ) //Get Descriptor
			{
				int i;
				for( i = 0; i < DESCRIPTOR_LIST_ENTRIES; i++ )
				{
					if( descriptor_list[i].wIndex == s->wIndex &&
						descriptor_list[i].wValue == s->wValue )
						break;
				}

				if( i == DESCRIPTOR_LIST_ENTRIES )
				{
					//Unknown descriptor.  The empty data packet already armed above
					//ends the transfer with a short packet, which the host accepts.
					goto custom_control;
				}

				const struct descriptor_list_struct * dl = &descriptor_list[i];
				usb_arm_in_transfer( e, dl->addr, dl->length, s->wLength );
			}
			else if( ( s->bmRequestType & 0x60 ) == 0x00 ) //Standard device requests
			{
				if( s->bRequest == 0x00 ) //Get Status (device, interface or endpoint)
				{
					control_reply[0] = 0;
					control_reply[1] = 0;
					usb_arm_in_transfer( e, control_reply, 2, s->wLength );
				}
				else if( s->bRequest == 0x08 && recipient == 0 ) //Get Configuration
				{
					control_reply[0] = (uint8_t)ist->configuration;
					usb_arm_in_transfer( e, control_reply, 1, s->wLength );
				}
				else if( s->bRequest == 0x0a && recipient == 1 ) //Get Interface
				{
					control_reply[0] = 0; //Only the default alternate setting exists.
					usb_arm_in_transfer( e, control_reply, 1, s->wLength );
				}
			}
			else if( ( s->bmRequestType & 0x60 ) == 0x20 ) //HID class requests
			{
				if( s->bRequest == 0x01 ) //Get Report
				{
					usb_arm_in_transfer( e, keyboard_report, sizeof( keyboard_report ), s->wLength );
				}
				else if( s->bRequest == 0x02 ) //Get Idle
				{
					control_reply[0] = 0; //Report only on change.
					usb_arm_in_transfer( e, control_reply, 1, s->wLength );
				}
				else if( s->bRequest == 0x03 ) //Get Protocol
				{
					control_reply[0] = 0; //Boot protocol.
					usb_arm_in_transfer( e, control_reply, 1, s->wLength );
				}
			}
		}
		else if( s->bmRequestType == 0x00 ) //Standard device write
		{
			if( s->bRequest == 0x05 ) //Set address.
			{
				ist->pending_address = s->wValue;
				ist->address_pending = 1;
			}
			if( s->bRequest == 0x09 ) //Set configuration.
			{
				ist->configuration = s->wValue;
				usb_reset_data_endpoints( ist );
			}
		}
		else if( s->bmRequestType == 0x02 && s->bRequest == 0x01 && s->wValue == 0x0000 )
		{
			//Clear Feature (endpoint halt) must also reset that endpoint's toggle.
			uint8_t halted = s->wIndex & 0x0f;
			if( halted > 0 && halted < ENDPOINTS )
			{
				struct usb_endpoint * he = &ist->eps[halted];
				he->toggle_in = 0;
				he->toggle_out = 0;
				he->send = 0;
				he->ptr_in = 0;
				he->place_in = 0;
				he->size_in = 0;
				he->advance_in = 0;
			}
		}
		else if( s->bmRequestType == 0x21 && s->bRequest == 0x09 && ( s->wValue >> 8 ) == 0x02 )
		{
			//Set Report (output): the host sends its keyboard LED state in the data stage.
			e->ptr_out = &keyboard_leds;
			e->max_size_out = sizeof( keyboard_leds );
			e->got_size_out = 0;
		}

custom_control:
		usb_handle_custom_control( s->bmRequestType, s->bRequest, s->wLength, ist );
	}
	else if( e->ptr_out )
	{
		//Read into that buffer.
		int acc = (int)ist->packet_size - 3;  //packet_size includes CRC and PID, need just data size.
		int place = e->got_size_out;

		if( acc < 0 ) acc = 0;  //Runt packet: nothing to copy.

		if( place + acc > e->max_size_out )
		{
			acc = e->max_size_out - place;
		}

		memcpy( e->ptr_out + e->got_size_out, ist->usb_buffer+1, acc );  //First byte of USB buffer is token.
		e->got_size_out += acc;
		if( e->got_size_out == e->max_size_out && e->transfer_done_ptr ) {
			e->ptr_out = 0;
			*e->transfer_done_ptr = e->got_size_out;
			e->transfer_done_ptr = 0;
		}
	}


just_ack:
	{
		//Got the right data.  Acknowledge.
		uint8_t sendword[2] = { 0x80, 0xD2 };
		usb_send_data( sendword, 2, 2 );
	}
}

void IRAM_ATTR usb_pid_handle_ack( uint32_t this_token, struct usb_internal_state_struct * ist )
{
	struct usb_endpoint * e = ist->ce;
	if( !e ) return;



	e->toggle_in = !e->toggle_in;
	e->place_in += e->advance_in;
	e->advance_in = 0;
	if( e->place_in == e->size_in )
	{
		e->send = 0;
		if( e->transfer_in_done_ptr ) (*e->transfer_in_done_ptr) = 1;
		e->transfer_in_done_ptr = 0;
	}
	if( e == &ist->eps[0] && ist->address_pending )
	{
		ist->my_address = ist->pending_address;
		ist->address_pending = 0;
	}
}


esp_err_t usb_init(void)
{
	const uint32_t usb_gpio_mask = (1U << DMINUS) | (1U << DPLUS);
	const gpio_config_t config = {
		.pin_bit_mask = usb_gpio_mask,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	esp_err_t err;

	// Both lines float: the USB side is driven by the external 1.5k pull-up on
	// D-, not by the weak internal pull-ups.
	err = gpio_config(&config);
	if (err != ESP_OK) return err;

	// Hold both USB lines low briefly so the host sees a fresh connection.
	err = gpio_set_level((gpio_num_t)DMINUS, 0);
	if (err != ESP_OK) return err;
	err = gpio_set_level((gpio_num_t)DPLUS, 0);
	if (err != ESP_OK) return err;
	err = gpio_set_direction((gpio_num_t)DMINUS, GPIO_MODE_OUTPUT);
	if (err != ESP_OK) return err;
	err = gpio_set_direction((gpio_num_t)DPLUS, GPIO_MODE_OUTPUT);
	if (err != ESP_OK) return err;
	ets_delay_us(10000);

	err = gpio_set_direction((gpio_num_t)DMINUS, GPIO_MODE_INPUT);
	if (err != ESP_OK) return err;
	err = gpio_set_direction((gpio_num_t)DPLUS, GPIO_MODE_INPUT);
	if (err != ESP_OK) return err;
	err = gpio_set_intr_type((gpio_num_t)DPLUS, GPIO_INTR_POSEDGE);
	if (err != ESP_OK) return err;

	*(volatile uint32_t *)(GPIO_BASE_REG + GPIO_OFFSET_GPIO_STATUS_W1TC) = usb_gpio_mask;
	err = gpio_isr_register(gpio_intr, NULL, 0, NULL);
	if (err != ESP_OK) return err;

	// gpio_isr_register() only attaches the handler. Nothing in the SDK enables
	// the GPIO interrupt line itself, so without this the USB ISR never runs.
	_xt_isr_unmask(1 << ETS_GPIO_INUM);
	return ESP_OK;
}

static esp_err_t usb_send_report(uint8_t endpoint_number, uint8_t *buffer,
		const uint8_t *report, uint16_t report_size)
{
	struct usb_endpoint *endpoint = &usb_internal_state.eps[endpoint_number];

	if (endpoint->send) return ESP_ERR_INVALID_STATE;

	memcpy(buffer, report, report_size);
	endpoint->ptr_in = buffer;
	endpoint->place_in = 0;
	endpoint->size_in = report_size;
	// The ISR must never observe send == 1 before the buffer description above.
	__asm__ __volatile__( "" ::: "memory" );
	endpoint->send = 1;
	return ESP_OK;
}

esp_err_t usb_send_keyboard_report(const uint8_t report[8])
{
	if (report == NULL) return ESP_ERR_INVALID_ARG;
	return usb_send_report(1, keyboard_report, report, sizeof(keyboard_report));
}

uint8_t usb_get_keyboard_leds(void)
{
	return keyboard_leds;
}
