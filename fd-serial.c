/*
**  Tullnet Full Duplex Serial UART
**  (C) 2010, Nick Andrew <nick@tull.net>
**
**  ATtiny85
**     This code uses Timer/Counter 1
**     RX is connected to PORTB2 (INT0), pin 7
**     TX is connected to PORTB3, pin 2
**     Speed 9600 bps, full duplex
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>

#include "fd-serial.h"

#ifndef CPU_FREQ
#define CPU_FREQ 8000000
#endif

#if SERIAL_RATE == 9600
// Prescaler CK/4
#define PRESCALER (1<<CS11 | 1<<CS10)
#define PRESCALER_DIVISOR 4
// 8000000 / PRESCALER / 9600 = 208.333
#define SERIAL_TOP 207
#define SERIAL_HALFBIT 104

#else
#error "Serial rates other than 9600 are not presently supported"
#endif

/* Data structure used by this module */

static struct fd_uart fd_uart1;

/*
**  Start the timer. The timer must be running while characters
**  are being received or sent.
*/

static void _starttimer(void) {

	TCCR1 |= PRESCALER;
}

/*
**  Stop the timer. This will not only save power but also stop
**  the regular timer interrupts on TIMER1_COMPA and TIMER1_COMPB.
*/

static void _stoptimer(void) {

	TCCR1 &= ~PRESCALER;
}

/*
**  Enable INT0
*/

inline void _enable_int0(void) {
	// Clear any pending INT0
	GIFR |= 1<<INTF0;
	// Enable INT0
	GIMSK |= 1<<INT0;
}

/*
**  Disable INT0
*/

inline void _disable_int0(void) {
	GIMSK &= ~( 1<<INT0 );
}

/*
**  Enable TIMER1_COMPA - TX bit timer
*/

inline void _start_tx(void) {
	TIMSK |= 1<<OCIE1A;
}

/*
**  Disable TIMER1_COMPA
*/

inline void _stop_tx(void) {
	// Enable TIMER_COMP1A
	TIMSK &= ~( 1<<OCIE1A );
}

/*
**  Enable TIMER1_COMPB - RX bit timer
*/

inline void _start_rx(void) {
	// Clear pending RX timer interrupt
	TIFR |= 1<<OCF1B;
	// Enable TIMER_COMP1B
	TIMSK |= 1<<OCIE1B;
}

/*
**  Disable TIMER1_COMPB
*/

inline void _stop_rx(void) {
	TIMSK &= ~( 1<<OCIE1B );
}

/*
**  Initialise the software UART.
**
**  Configure timer1 as follows:
**    1 interrupts per data bit
**    CTC mode (CTC1=1)
**    No output pin
**    Frequency = 8000000 / 4 / 208 = 9615 bits/sec
**    Prescaler = 4, Clock source = System clock, OCR1C = 207
**  Configure INT0 so an interrupt occurs on the falling edge
**    of INT0 (pin 7)
*/

void fdserial_init(void) {
	uint8_t com_mode = 0<<COM1A1 | 0<<COM1A0;
	uint8_t ctc_mode = 1<<CTC1;

	fd_uart1.send_ready = 1;
	fd_uart1.tx_state = 0;

	fd_uart1.available = 0;
	fd_uart1.rx_state = 0;

#ifdef RING_BUFFER
	fd_uart1.rx_head = 0;
	fd_uart1.rx_tail = 0;
#endif

	// Configure INT0 to interrupt on falling edge
	MCUCR |= 1<<ISC01;

	TCNT1 = 0;
	OCR1A = 16; // this will be used for send bit timing
	OCR1B = 32; // this will be used for receive bit timing
	OCR1C = SERIAL_TOP;

	// Configure pin PORTB3 as an output, and raise it
	DDRB |= S1_TX_PIN;
	PORTB |= S1_TX_PIN;

	// Configure pin PORTB2 as an input, and enable pullup
	DDRB &= ~( S1_RX_PIN );
	PORTB |= S1_RX_PIN;

	_stoptimer();
	TCCR1 = ctc_mode | com_mode;
	_starttimer();
	_enable_int0();
}

/*
**  fdserial_available()
**    Return true if a character has been received on the receive interface.
*/

uint8_t fdserial_available(void) {
#ifdef RING_BUFFER
	char d = fd_uart1.rx_head - fd_uart1.rx_tail;
	if (d < 0) {
		return d + RING_BUFFER;
	}

	return d;
#else
	return fd_uart1.available;
#endif
}

/*
**  fdserial_sendok()
**    Return true if the transmit interface is free to transmit a character.
*/

uint8_t fdserial_sendok(void) {
	return fd_uart1.send_ready;
}

/*
**  fdserial_send(c)
**    Send the character c.
*/

void fdserial_send(unsigned char send_arg) {
	// Wait until previous byte finished
	while (! fd_uart1.send_ready) { }

	OCR1A = TCNT1;
	fd_uart1.send_ready = 0;
	fd_uart1.send_byte = send_arg;
	fd_uart1.tx_state = 1; // Send start bit
	_start_tx();
}

/*
**  c = fdserial_recv()
**    Return the received character.
**    The receive interface is buffered, so when a character is available
**    the MCU has at most 1 character-time to receive it, otherwise it
**    will be overwritten by the next character.
**
**    This function will wait until a character is received.
*/

unsigned char fdserial_recv() {
	unsigned char c;

#ifdef RING_BUFFER
	// Wait until chars in buffer
	while (fd_uart1.rx_head == fd_uart1.rx_tail) { }

	c = fd_uart1.rx_buf[fd_uart1.rx_tail];

	// Don't use the 'mod' operation because it is expensive.
	// Also don't ever set rx_tail = RING_BUFFER.
	if (fd_uart1.rx_tail == RING_BUFFER - 1) {
		fd_uart1.rx_tail = 0;
	} else {
		fd_uart1.rx_tail ++;
	}
#else
	// Wait until available
	while (! fd_uart1.available) { }
	c = fd_uart1.recv_byte;
	fd_uart1.recv_byte = 0;  // Reading nulls means you are probably doing something wrong
	fd_uart1.available = 0;
#endif

	return c;
}


/*
**  fdserial_alarm(uint32_t duration)
**
**  Keep the transmit system busy for the specified number of ms.
**
**  Wait until the transmit state is idle, then setup the
**  transmit bit comparator to count down.
*/

void fdserial_alarm(uint32_t duration) {
	uint32_t timer_ticks = ( duration * CPU_FREQ ) / PRESCALER_DIVISOR / 1000;
	uint32_t cycles = timer_ticks / ( SERIAL_TOP + 1);
	uint8_t remainder = timer_ticks - (cycles * (SERIAL_TOP + 1));
	// Wait until available
	while (! fd_uart1.send_ready) { }

	OCR1A = TCNT1 - remainder;
	fd_uart1.delay = cycles;
	fd_uart1.send_ready = 0;
	fd_uart1.tx_state = 5;
}

/*
**  fdserial_delay(uint32_t duration)
**
**  Delay for the specified number of ms.
**
**  Setup an alarm for the specified duration, then
**  spin until the alarm has expired (transmit state
**  has returned to idle).
*/

void fdserial_delay(uint32_t duration) {
	fdserial_alarm(duration);

	// Wait until alarm expires
	while (! fd_uart1.send_ready) { }
}

/*
** Interrupt handler for timer1, TCCR1A, tx bits
*/

ISR(TIMER1_COMPA_vect)
{
	switch(fd_uart1.tx_state) {
		case 0: // Idle
			return;

		case 1: // Send start bit
			PORTB &= ~( S1_TX_PIN );
			fd_uart1.tx_state = 2;
			fd_uart1.send_bits = 8;
			return;

		case 2: // Send a bit
			if (fd_uart1.send_byte & 1) {
				PORTB |= S1_TX_PIN;
			} else {
				PORTB &= ~( S1_TX_PIN );
			}
			fd_uart1.send_byte >>= 1;

			if (! --fd_uart1.send_bits) {
				fd_uart1.tx_state = 3;
			}
			return;

		case 3: // Send stop bit
			PORTB |= S1_TX_PIN;
			fd_uart1.tx_state = 4;
			return;

		case 4: // Return to idle mode
			fd_uart1.send_ready = 1;
			fd_uart1.tx_state = 0;
			_stop_tx();
			return;
		case 5: // Timed delay
			if (! --fd_uart1.delay) {
				fd_uart1.send_ready = 1;
				fd_uart1.tx_state = 0;
			}
			return;
	}
}

/*
** Interrupt handler for timer1, TCCR1B, rx bits
*/

ISR(TIMER1_COMPB_vect)
{
	// Read the bit as early as possible, to try to hit the
	// center mark
	uint8_t read_bit = PINB & S1_RX_PIN;

	switch(fd_uart1.rx_state) {
		case 0: // Midpoint of start bit. Go on to first data bit.
			fd_uart1.rx_state = 2;
			fd_uart1.recv_bits = 8;
			break;

		case 1: // Reading start bit
			// Go straight on to first data bit
			fd_uart1.rx_state = 2;
			fd_uart1.recv_bits = 8;
			break;

		case 2: // Reading a data bit
			fd_uart1.recv_shift >>= 1;
			if (read_bit) {
				fd_uart1.recv_shift |= 0x80;
			}

			if (! --fd_uart1.recv_bits) {
				fd_uart1.rx_state = 3;
			}
			break;

		case 3: // Byte done, wait for high
			if (read_bit) {
#ifdef RING_BUFFER
				// Put the latest char in the buffer
				fd_uart1.rx_buf[fd_uart1.rx_head] = fd_uart1.recv_shift;

				// Increment the buffer head
				if (fd_uart1.rx_head == RING_BUFFER - 1) {
					fd_uart1.rx_head = 0;
				} else {
					fd_uart1.rx_head ++;
				}

				// If buffer is full,
				if (fd_uart1.rx_head == fd_uart1.rx_tail) {
					// Increment rx_tail to drop oldest character
					if (fd_uart1.rx_tail == RING_BUFFER - 1) {
						fd_uart1.rx_tail = 0;
					} else {
						fd_uart1.rx_tail ++;
					}
				}
#else
				fd_uart1.recv_byte = fd_uart1.recv_shift;
				fd_uart1.available = 1;
#endif
				fd_uart1.rx_state = 0;
				_stop_rx();
				_enable_int0();
			}
			break;
	}
}

/*
** This is called on the falling edge of INT0 (pin 7).
** It is the beginning of a start bit.
*/

ISR(INT0_vect) {
	uint8_t tcnt1 = TCNT1;

	// Set sample time, half a bit after now.
	if (tcnt1 >= SERIAL_HALFBIT) {
		OCR1B = tcnt1 - SERIAL_HALFBIT;
	} else {
		OCR1B = tcnt1 + SERIAL_HALFBIT;
	}

	_disable_int0();
	_start_rx();
}
