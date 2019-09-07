/******************************************************************************
Title:      ATtiny Fuse Restore
Author:     Peter Fleury  <pfleury@gmx.ch> http://tinyurl.com/peterfleury
File:       $Id: HvspFuseRestore.c,v 1.1 2014/10/05 06:59:12 peter Exp $
Compiler:   AVR-gcc 4.7.2
Hardware:   ATmega8 or any AVR with at least 8 IO pins
License:    GNU General Public License 

DESCRIPTION:  
    Restore the fuse bits of a ATtiny13 or ATtiny24/44/84 or ATtiny25/45/85 to factory default 
    using High-voltage Serial Programming (HVSP). 
    This will reset the RSTDISBL fuse bit and change back the RESET Pin
    from a I/O line to a Reset input. Now further ISP-programming is possible.
    
    See also description with circuit on my home page:
    http://homepage.hispeed.ch/peterfleury/avr-hvsp-fuse-restore.html
	  
NOTES:
    Based on http://pe0fko.nl/Fuse-restore/ (Fuse restore ATtiny11, ATtiny12, ATtiny13, ATtiny25, ATtiny45, ATtiny85),
    but with different simplified circuit and software rewritten and extended.
    
LICENSE:
    Copyright (C) 2014 Peter Fleury

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

*******************************************************************************/
#include <inttypes.h>
#include <stdbool.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <avr/pgmspace.h>


/*
 * Port / Pin defines, adapt to your selected AVR and circuit
 */
#define HVSP_SDI_OUT   PORTC
#define HVSP_SDI_DDR   DDRC
#define	HVSP_SDI		   PC0		  // Serial Data Input on target

#define HVSP_SII_OUT   PORTC
#define HVSP_SII_DDR   DDRC
#define	HVSP_SII		   PC1		  // Serial Instruction Input on target

#define	HVSP_SDO_OUT   PORTC
#define HVSP_SDO_IN    PINC
#define	HVSP_SDO_DDR   DDRC
#define	HVSP_SDO       PC2	    // Serial Data Output on target

#define	HVSP_SCI_OUT   PORTC
#define	HVSP_SCI_DDR   DDRC
#define	HVSP_SCI		   PC3	    // Serial Clock Input on target

#define HVSP_VCC_OUT   PORTC
#define HVSP_VCC_DDR   DDRC
#define HVSP_VCC       PC4      // VCC to target

#define HVSP_RST_OUT   PORTC
#define HVSP_RST_DDR   DDRC
#define	HVSP_RST		   PC5		  // +12V to reset pin on target (inverted through NPN transistor)

#define LED_OUT        PORTB    
#define LED_DDR        DDRB
#define LED_PIN        PB0      // LED to show result

#define SWITCH_OUT     PORTB
#define SWITCH_DDR     DDRB
#define SWITCH_IN      PINB
#define SWITCH_PIN     PB1      // Push button to start fuse retore 


/*
 * Struct to hold signature and default fuse bits of target to be programmed
 * See AVR datasheet or avr-libc include files iotnxx.h LFUSE_DEFAULT, HFUSE_DEFAULT, EFUSE_DEFAULT
 *
 * add to a small test program the following statements and compile for each MCU to get the hex values
 *  PORTB = LFUSE_DEFAULT; PORTB = HFUSE_DEFAULT; PORTB = EFUSE_DEFAULT; 
 *
 * or use Atmel AVRｮ Fuse Calculator: http://www.engbedded.com/fusecalc/
 */
typedef struct {
	uint8_t		signature[3];
	uint8_t		fuseLowBits;
	uint8_t		fuseHighBits;
	uint8_t		fuseExtendedBits;
}TargetCpuInfo_t;

static const TargetCpuInfo_t	PROGMEM	targetCpu[] = 
{
	{	// ATtiny13
		.signature	      = { SIGNATURE_0, 0x90, 0x07 },
		.fuseLowBits	    = 0x6A,
		.fuseHighBits	    = 0xFF,
		.fuseExtendedBits	= 0x00,
	},
	{	// ATtiny24
		.signature	      = { SIGNATURE_0, 0x91, 0x0B },
		.fuseLowBits	    = 0x62,
		.fuseHighBits	    = 0xDF,
		.fuseExtendedBits	= 0xFF,
	},
	{	// ATtiny44
		.signature	      = { SIGNATURE_0, 0x92, 0x07 },
		.fuseLowBits	    = 0x62,
		.fuseHighBits	    = 0xDF,
		.fuseExtendedBits	= 0xFF,
	},
	{	// ATtiny84
		.signature	      = { SIGNATURE_0, 0x93, 0x0C },
		.fuseLowBits	    = 0x62,
		.fuseHighBits	    = 0xDF,
		.fuseExtendedBits	= 0xFF,
	},
	{	// ATtiny25
		.signature	      = { SIGNATURE_0, 0x91, 0x08 },
		.fuseLowBits	    = 0x62,
		.fuseHighBits	    = 0xDF,
		.fuseExtendedBits	= 0xFF,
	},
	{	// ATtiny45
		.signature	      = { SIGNATURE_0, 0x92, 0x06 },
		.fuseLowBits	    = 0x62,
		.fuseHighBits	    = 0xDF,
		.fuseExtendedBits	= 0xFF,
	},
	{	// ATtiny85
		.signature	      = { SIGNATURE_0, 0x93, 0x0B },
		.fuseLowBits	    = 0x62,
		.fuseHighBits	    = 0xDF,
		.fuseExtendedBits	= 0xFF,
	},
	  // mark end of list
	{ { 0,0,0 }, 0, 0, 0 },
};


/* 
 * The minimum period for the Serial Clock Input (SCI) during High-voltage Serial Programming is 220 ns.
 */
static inline void clockit(void)
{
	HVSP_SCI_OUT |= (1<<HVSP_SCI);
	_delay_us(1);
	HVSP_SCI_OUT &= ~(1<<HVSP_SCI);
}


/*
 * Send instruction and data byte to target and read one byte from target
 */
static uint8_t transferByte( uint8_t data, uint8_t instruction )
{
	uint8_t   i;
	uint8_t   byteRead = 0;

  /* first bit is zero */
  HVSP_SDI_OUT &= ~(1<<HVSP_SDI); 
  HVSP_SII_OUT &= ~(1<<HVSP_SII); 
	clockit();

	for(i=0; i<8; i++)
	{		
		/* read one bit form SDO */
		byteRead <<= 1;   // shift one bit, this clears LSB				
		if ( (HVSP_SDO_IN & (1<<HVSP_SDO)) != 0 )
		{
			byteRead |= 1;
		}
				
		/* output next data bit on SDI */
		if (data & 0x80)
		{
			HVSP_SDI_OUT |= (1<<HVSP_SDI);
		}
		else
		{
			HVSP_SDI_OUT &= ~(1<<HVSP_SDI);			
		}

    /* output next instruction bit on SII */
		if (instruction & 0x80)
		{
			HVSP_SII_OUT |= (1<<HVSP_SII);
		}
		else
		{
			HVSP_SII_OUT &= ~(1<<HVSP_SII);			
		}

		clockit();

    /* prepare for processing next bit */
		data <<= 1;
		instruction <<= 1;
	}

	/* Last two bits are zero */
  HVSP_SDI_OUT &= ~(1<<HVSP_SDI);		
  HVSP_SII_OUT &= ~(1<<HVSP_SII);			
	clockit();
	clockit();

	return byteRead;

}/* transferByte */


static void pollSDO(void)
{
	// wait until SDO goes high
	while ( 0 == (HVSP_SDO_IN & _BV(HVSP_SDO)) )
	{
	}
}

/*
 * High-voltage Serial Programming Instruction
 * Atmel ATtiny data sheet: Chapter Memory Programming -> High Voltage Serial Programming Algorithm 
 * -> Table: High-voltage Serial Programming Instruction Set
 */
 
static uint8_t getSignature(uint8_t code)
{
	transferByte(0x08, 0x4C);
	transferByte(code, 0x0C);
	transferByte(0x00, 0x68);
	return transferByte(0x00, 0x6C);
}


static void writeFuseLowBits(uint8_t code)
{
	transferByte(0x40, 0x4C);
	transferByte(code, 0x2C);
	transferByte(0x00, 0x64);
	transferByte(0x00, 0x6C);
	pollSDO();
	transferByte(0x00, 0x4C);
	pollSDO();
}


static void writeFuseHighBits(uint8_t code)
{
	transferByte(0x40, 0x4C);
	transferByte(code, 0x2C);
	transferByte(0x00, 0x74);
	transferByte(0x00, 0x7C);
	pollSDO();
	transferByte(0x00, 0x4C);
	pollSDO();
}


static void writeFuseExtendedBits(uint8_t code)
{
	transferByte(0x40, 0x4C);
	transferByte(code, 0x2C);
	transferByte(0x00, 0x66);
	transferByte(0x00, 0x6E);
	pollSDO();
	transferByte(0x00, 0x4C);
	pollSDO();
}


static uint8_t readFuseLowBits( void )
{
	transferByte(0x04, 0x4C);
	transferByte(0x00, 0x68);
	return transferByte(0x00, 0x6C);
}


static uint8_t readFuseHighBits( void )
{
	transferByte(0x04, 0x4C);
	transferByte(0x00, 0x7A);
	return transferByte(0x00, 0x7E);
}


static uint8_t readFuseExtendedBits( void )
{
	transferByte(0x04, 0x4C);
	transferByte(0x00, 0x6A);
	return transferByte(0x00, 0x6E);
}


int main(void)
{
  /*
  The following algorithm puts the device in High-voltage Serial Programming mode:
  (see Atmel ATtiny data sheet chapter Memory Programming -> High Voltage Serial Programming)
  
  1. Set Prog_enable pins (SDI, SII, SDO, SCI) ・・ RESET pin and VCC to 0V.
  2. Apply 5V between VCC and GND. Ensure that VCC reaches at least 1.8V within the next 20 microseconds.
  3. Wait 20 - 60 us, and apply 11.5 - 12.5V to RESET.
  4. Keep the Prog_enable pins unchanged for at least 10 us after the High-voltage has been
  applied to ensure the Prog_enable Signature has been latched.
  5. Release the Prog_enable[2] (SDO pin) to avoid drive contention on the Prog_enable[2]/SDO pin.
  6. Wait at least 300 us before giving any serial instructions on SDI/SII.
  7. Exit Programming mode by power the device down or by bringing RESET pin to 0V.
  */

  /* LED pin as output and initially off  */
  LED_DDR   = (1<<LED_PIN);
  LED_OUT |= (1<<LED_PIN);
  
  /* turn on internal pull-up for push button */
  SWITCH_DDR &= ~(1<<SWITCH_PIN);
  SWITCH_OUT |= (1<<SWITCH_PIN);
  
 
  while(1)
  {
    uint8_t device;
    bool    fuseRestoreSuccessful = false;

  	/* LED initially off  */
  	LED_OUT |= (1<<LED_PIN);

  	/* start when push button is pushed */
  	if ( 0 == (SWITCH_IN & (1<<SWITCH_PIN)) )
  	{  		
    	/* 1. set all pin as output */
    	HVSP_SCI_DDR  |= (1 << HVSP_SCI);
    	HVSP_SII_DDR  |= (1 << HVSP_SII);
    	HVSP_SDI_DDR  |= (1 << HVSP_SDI);
    	HVSP_SDO_DDR  |= (1 << HVSP_SDO);
    	HVSP_RST_DDR  |= (1 << HVSP_RST);
    	HVSP_VCC_DDR  |= (1 << HVSP_VCC);
    	
    	/* set pins to 0, RST to 1 because of inverting NPN transistor */
    	HVSP_SCI_OUT &= ~(1 << HVSP_SCI);
    	HVSP_SII_OUT &= ~(1 << HVSP_SII);
    	HVSP_SDI_OUT &= ~(1 << HVSP_SDI);
    	HVSP_SDO_OUT &= ~(1 << HVSP_SDO);
    	HVSP_RST_OUT |=  (1 << HVSP_RST);
    	HVSP_VCC_OUT &= ~(1 << HVSP_VCC);
    	  	
    	/* 2. Apply VCC 5V to target */
    	HVSP_VCC_OUT |= (1 << HVSP_VCC);
    	
    	/* 3: Wait 20-60us before applying 12V to RESET */
    	_delay_us(60);
    	HVSP_RST_OUT &= ~(1 << HVSP_RST);
    	
    	/* 4. Keep the Prog_enable pins unchanged for at least 10 us after the High-voltage has been applied */
    	_delay_us(20);
    	
    	/* 5. Switch Prog_enable[2] (SDO pin) to input */
    	HVSP_SDO_DDR  &= ~(1<<HVSP_SDO);
    	
    	/* 6.  wait > 300us before sending any data on SDI/SII */
      _delay_us(300);
           
      
    	for (device = 0; pgm_read_byte(&targetCpu[device].signature[0])!=0 ; device++)
    	{
    		if (getSignature(0) == pgm_read_byte(&targetCpu[device].signature[0]) && 
    			  getSignature(1) == pgm_read_byte(&targetCpu[device].signature[1]) && 
    			  getSignature(2) == pgm_read_byte(&targetCpu[device].signature[2])		)
    		{
    			/* get default fuse bits for selected target */
    			uint8_t fuseLowBits      = pgm_read_byte(&targetCpu[device].fuseLowBits);
    			uint8_t fuseHighBits     = pgm_read_byte(&targetCpu[device].fuseHighBits);
    			uint8_t fuseExtendedBits = pgm_read_byte(&targetCpu[device].fuseExtendedBits);
    			
    			/* write default fuse bits */
    			writeFuseLowBits( fuseLowBits );
    			writeFuseHighBits( fuseHighBits );
    			if ( fuseExtendedBits != 0 )
    			{
    				writeFuseExtendedBits( fuseExtendedBits );
    		  }
    
          /* verify */
          if ( (fuseLowBits == readFuseLowBits()) && 
          	   (fuseHighBits == readFuseHighBits()) && 
          	   ( (0 ==fuseExtendedBits) ||(fuseExtendedBits == readFuseExtendedBits()) ) )
          {
          	fuseRestoreSuccessful = true;
          }
    			break;
    	  }
    	}
      
      /* 7. Exit Programming mode by setting all pins to 0 and then power down the device down */
      HVSP_SCI_OUT &= ~(1 << HVSP_SCI);
      HVSP_SII_OUT &= ~(1 << HVSP_SII); 
      HVSP_SDI_OUT &= ~(1 << HVSP_SDI); 
      HVSP_RST_OUT |=  (1 << HVSP_RST);
      _delay_us(10);
      HVSP_VCC_OUT &= ~(1 << HVSP_VCC);
        
      /* turn on LED for 4 sec if fuse restore was sucessful or flash LED on error */
      int i;
      for (i=0; i<16; i++ )
      {
        if ( fuseRestoreSuccessful )
        {
        	LED_OUT &= ~(1 << LED_PIN);
        }
        else
        {
        	LED_OUT ^= (1<<LED_PIN);      		
        }
        _delay_ms(250);      	
      }
         
    }//if
     
  }//while

}//main
