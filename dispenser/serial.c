#include <string.h>
#include <avr/io.h>
#include <util/crc16.h>
#include <avr/interrupt.h>
#include "defs.h"
#include "serial.h"
#include "pack7.h"

#define BAUD 38400
#define UBBR (F_CPU / 16 / BAUD - 1)
#define RECEIVE_TIMEOUT  100
#define MAX_PACKET_LEN 16

extern uint8_t check_reset(void);
extern volatile uint32_t g_time;
extern void idle(void);

void serial_init(void)
{
    /*Set baud rate */ 
    UBRR0H = (unsigned char)(UBBR>>8); 
    UBRR0L = (unsigned char)UBBR; 
    /* Enable transmitter */ 
    UCSR0B = (1<<TXEN0) | (1<<RXEN0); 
    /* Set frame format: 8data, 1stop bit */ 
    UCSR0C = (0<<USBS0)|(3<<UCSZ00); 
}

void serial_enable(uint8_t rx, uint8_t tx)
{
    if (rx)
        UCSR0B |= (1<<RXEN0); 
    else
        UCSR0B &= ~(1<<RXEN0); 

    if (tx)
        UCSR0B |= (1<<TXEN0); 
    else
        UCSR0B &= ~(1<<TXEN0); 
}

void serial_tx(uint8_t ch)
{
    while ( !( UCSR0A & (1<<UDRE0)) );
    UDR0 = ch;
}

uint8_t serial_rx(void)
{
    while ( !(UCSR0A & (1<<RXC0))) 
        ;

    return UDR0;
}

uint8_t serial_tx_nb(uint8_t ch)
{
    if (UCSR0A & (1<<UDRE0))
    {
        UDR0 = ch;
        return 1;
    }
    return 0;
}

uint8_t serial_rx_nb(uint8_t *ch)
{
    if (UCSR0A & (1<<RXC0)) 
    {
        *ch = UDR0;
        return 1;
    }
    return 0;
}

#ifdef ROUTER
void idle()
{
}
uint8_t check_reset(void)
{
    return 0;
}
#endif

uint8_t receive_packet(packet_t *p)
{
    uint16_t crc = 0;
    uint8_t  i, ret, ack, header, restart, ch, *ptr;
    uint8_t  size, unpacked_size;
    uint8_t  data[MAX_PACKET_LEN];

    for(;!check_reset();)
    {
        header = 0;
        restart = 0;
        for(;;)
        {
            ret = serial_rx_nb(&ch);
            if (check_reset())
                return COMM_RESET;

            if (ret)
            {
                if (ch == 0xFF)
                    header++;
                else 
                    header = 0;

                if (header == 2)
                    break;
            }
            idle();
        }

        for(;;)
        {
            ret = serial_rx_nb(&ch);
            if (check_reset())
                return COMM_RESET;

            if (ret)
            {
                if (ch == 0xFF)
                {
                    restart = 1;
                    break;
                }
                size = ch;
                break;
            }
            idle();
        }
        if (restart)
            continue;

        for(i = 0; i < size; i++)
        {
            for(;;)
            {
                ret = serial_rx_nb(&ch);
                if (check_reset())
                    return COMM_RESET;

                if (ret)
                {
                    if (ch == 0xFF)
                    {
                        restart = 1;
                        break;
                    }
                    data[i] = ch;
                    break;
                }
                idle();
            }
            if (restart)
                break;
        }
        if (restart)
            continue;

        tbi(PORTB, 5);

        unpack_7bit(size, data, &unpacked_size, (uint8_t *)p);

        crc = 0;
        for(i = 0, ptr = (uint8_t *)p; i < unpacked_size - 2; i++, ptr++)
            crc = _crc16_update(crc, *ptr);

        if (crc != p->crc)
            ack = PACKET_ACK_CRC_FAIL;
        else
            ack = PACKET_ACK_OK;

        for(;;)
        {
            ret = serial_tx_nb(ack);
            if (check_reset())
                return COMM_RESET;
            if (ret)
                break;
            idle();
        }
        return (ack == PACKET_ACK_OK) ? COMM_OK : COMM_CRC_FAIL;
    }
    return COMM_RESET;
}

uint8_t send_packet8(uint8_t type, uint8_t data)
{
    packet_t p;
    
    memset(&p, 0, sizeof(packet_t));
    p.type = type;
    p.p.uint8[0] = data;

    return send_packet(&p);
}

uint8_t send_packet(packet_t *p)
{
    uint16_t crc = 0;
    uint8_t i, *ch, ret, ack, tries, encoded_size;
    uint8_t encoded[sizeof(packet_t) + ((sizeof(packet_t) + 7) / 8)];

    crc = _crc16_update(crc, p->dest);
    crc = _crc16_update(crc, p->type);
    crc = _crc16_update(crc, p->p.uint8[0]);
    crc = _crc16_update(crc, p->p.uint8[1]);
    crc = _crc16_update(crc, p->p.uint8[2]);
    p->crc = _crc16_update(crc, p->p.uint8[3]);

    pack_7bit(sizeof(packet_t), (uint8_t *)p, &encoded_size, encoded);
    for(tries = 0; tries < NUM_PACKET_SEND_TRIES; tries++)
    {
        // If we have any characters pending, nuke 'em
        serial_rx_nb(&ack);

        serial_tx(0xFF);
        serial_tx(0xFF);
        serial_tx(encoded_size);
        for(i = 0, ch = (uint8_t *)encoded; i < encoded_size; i++, ch++)
            serial_tx(*ch);

        for(;;)
        {
            ret = serial_rx_nb(&ack);
            if (check_reset())
                return COMM_RESET;

            if (ret)
                break;
        }
        if (ack == PACKET_ACK_OK)
            return COMM_OK;
    }

    // whoops, we didn't succesfully send the packet. :-(
    return COMM_SEND_FAIL;
}

