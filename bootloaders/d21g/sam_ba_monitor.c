/*
  Copyright (c) 2015 Arduino LLC.  All right reserved.
  Copyright (c) 2015 Atmel Corporation/Thibaut VIARD.  All right reserved.
  Copyright (C) 2017 Industruino <connect@industruino.com>  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "sam.h"
#include <string.h>
#include "sam_ba_monitor.h"
#include "sam_ba_serial.h"
#include "board_driver_serial.h"
#include "board_driver_usb.h"
#include "sam_ba_usb.h"
#include "sam_ba_cdc.h"
#include "board_driver_led.h"
#include "netcfg.h"
#include "tftp.h"
#include "utils.h"

const char RomBOOT_Version[] = SAM_BA_VERSION;
const char RomBOOT_ExtendedCapabilities[] = "[Arduino:XYZ]";

/* Provides one common interface to handle both USART and USB-CDC */
typedef struct
{
  /* send one byte of data */
  int (*put_c)(int value);
  /* Get one byte */
  int (*get_c)(void);
  /* Receive buffer not empty */
  bool (*is_rx_ready)(void);
  /* Send given data (polling) */
  uint32_t (*putdata)(void const* data, uint32_t length);
  /* Get data from comm. device */
  uint32_t (*getdata)(void* data, uint32_t length);
  /* Send given data (polling) using xmodem (if necessary) */
  uint32_t (*putdata_xmd)(void const* data, uint32_t length);
  /* Get data from comm. device using xmodem (if necessary) */
  uint32_t (*getdata_xmd)(void* data, uint32_t length);
} t_monitor_if;

#if SAM_BA_INTERFACE == SAM_BA_UART_ONLY  ||  SAM_BA_INTERFACE == SAM_BA_BOTH_INTERFACES
/* Initialize structures with function pointers from supported interfaces */
const t_monitor_if uart_if =
{
  .put_c =       serial_putc,
  .get_c =       serial_getc,
  .is_rx_ready = serial_is_rx_ready,
  .putdata =     serial_putdata,
  .getdata =     serial_getdata,
  .putdata_xmd = serial_putdata_xmd,
  .getdata_xmd = serial_getdata_xmd
};
#endif

#if SAM_BA_INTERFACE == SAM_BA_USBCDC_ONLY  ||  SAM_BA_INTERFACE == SAM_BA_BOTH_INTERFACES
//Please note that USB doesn't use Xmodem protocol, since USB already includes flow control and data verification
//Data are simply forwarded without further coding.
const t_monitor_if usbcdc_if =
{
  .put_c =         cdc_putc,
  .get_c =         cdc_getc,
  .is_rx_ready =   cdc_is_rx_ready,
  .putdata =       cdc_write_buf,
  .getdata =       cdc_read_buf,
  .putdata_xmd =   cdc_write_buf,
  .getdata_xmd =   cdc_read_buf_xmd
};
#endif

/* The pointer to the interface object use by the monitor */
t_monitor_if * ptr_monitor_if;

/* b_terminal_mode mode (ascii) or hex mode */
volatile bool b_terminal_mode = false;
volatile bool b_sam_ba_interface_usart = false;

/* Pulse generation counters to keep track of the time remaining for each pulse type */
#define TX_RX_LED_PULSE_PERIOD 100
volatile uint16_t txLEDPulse = 0; // time remaining for Tx LED pulse
volatile uint16_t rxLEDPulse = 0; // time remaining for Rx LED pulse

static uint8_t com_interface = 0xFF;

void sam_ba_monitor_init(uint8_t ci)
{
#if SAM_BA_INTERFACE == SAM_BA_UART_ONLY  ||  SAM_BA_INTERFACE == SAM_BA_BOTH_INTERFACES
  //Selects the requested interface for future actions
  if (ci == SAM_BA_INTERFACE_USART)
  {
    com_interface = ci;
    ptr_monitor_if = (t_monitor_if*) &uart_if;
    b_sam_ba_interface_usart = true;
  }
#endif
#if SAM_BA_INTERFACE == SAM_BA_USBCDC_ONLY  ||  SAM_BA_INTERFACE == SAM_BA_BOTH_INTERFACES
  if (ci == SAM_BA_INTERFACE_USBCDC)
  {
    com_interface = ci;
    ptr_monitor_if = (t_monitor_if*) &usbcdc_if;
  }
#endif
#if SAM_BA_NET_INTERFACE == SAM_BA_NET_TFTP
  if (ci == SAM_BA_NET_TFTP)
    com_interface = ci;
#endif
}

/*
 * Central SAM-BA monitor putdata function using the board LEDs
 */
static uint32_t sam_ba_putdata(t_monitor_if* pInterface, void const* data, uint32_t length)
{
	uint32_t result ;

	result=pInterface->putdata(data, length);

	LEDTX_on();
	txLEDPulse = TX_RX_LED_PULSE_PERIOD;

	return result;
}

/*
 * Central SAM-BA monitor getdata function using the board LEDs
 */
static uint32_t sam_ba_getdata(t_monitor_if* pInterface, void* data, uint32_t length)
{
	uint32_t result ;

	result=pInterface->getdata(data, length);

	if (result)
	{
		LEDRX_on();
		rxLEDPulse = TX_RX_LED_PULSE_PERIOD;
	}

	return result;
}

/*
 * Central SAM-BA monitor putdata function using the board LEDs
 */
static uint32_t sam_ba_putdata_xmd(t_monitor_if* pInterface, void const* data, uint32_t length)
{
	uint32_t result ;

	result=pInterface->putdata_xmd(data, length);

	LEDTX_on();
	txLEDPulse = TX_RX_LED_PULSE_PERIOD;

	return result;
}

/*
 * Central SAM-BA monitor getdata function using the board LEDs
 */
static uint32_t sam_ba_getdata_xmd(t_monitor_if* pInterface, void* data, uint32_t length)
{
	uint32_t result ;

	result=pInterface->getdata_xmd(data, length);

	if (result)
	{
		LEDRX_on();
		rxLEDPulse = TX_RX_LED_PULSE_PERIOD;
	}

	return result;
}

/**
 * \brief This function allows data emission by USART
 *
 * \param *data  Data pointer
 * \param length Length of the data
 */
void sam_ba_putdata_term(uint8_t* data, uint32_t length)
{
  uint8_t temp, buf[12], *data_ascii;
  uint32_t i, int_value;

  if (b_terminal_mode)
  {
    if (length == 4)
      int_value = *(uint32_t *) data;
    else if (length == 2)
      int_value = *(uint16_t *) data;
    else
      int_value = *(uint8_t *) data;

    data_ascii = buf + 2;
    data_ascii += length * 2 - 1;

    for (i = 0; i < length * 2; i++)
    {
      temp = (uint8_t) (int_value & 0xf);

      if (temp <= 0x9)
        *data_ascii = temp | 0x30;
      else
        *data_ascii = temp + 0x37;

      int_value >>= 4;
      data_ascii--;
    }
    buf[0] = '0';
    buf[1] = 'x';
    buf[length * 2 + 2] = '\n';
    buf[length * 2 + 3] = '\r';
    sam_ba_putdata(ptr_monitor_if, buf, length * 2 + 4);
  }
  else
    sam_ba_putdata(ptr_monitor_if, data, length);
  return;
}

volatile uint32_t sp;
void call_applet(uint32_t address)
{
  uint32_t app_start_address;

  __disable_irq();

  sp = __get_MSP();

  /* Rebase the Stack Pointer */
  __set_MSP(*(uint32_t *) address);

  /* Load the Reset Handler address of the application */
  app_start_address = *(uint32_t *)(address + 4);

  /* Jump to application Reset Handler in the application */
  asm("bx %0"::"r"(app_start_address));
}

uint32_t current_number;
uint32_t i, length;
uint8_t command, *ptr_data, *ptr, data[SIZEBUFMAX];
uint8_t j;
uint32_t u32tmp;

uint32_t PAGE_SIZE, PAGE_SIZE_IN_WORDS, PAGES, MAX_FLASH;

// Prints a 32-bit integer in hex.
static void put_uint32(uint32_t n)
{
  char buff[8];
  int i;
  for (i=0; i<8; i++)
  {
    int d = n & 0XF;
    n = (n >> 4);

    buff[7-i] = d > 9 ? 'A' + d - 10 : '0' + d;
  }
  sam_ba_putdata( ptr_monitor_if, buff, 8);
}

static void sam_ba_eraseflash(uint32_t dst_addr)
{
  // Note: the flash memory is erased in ROWS, that is in block of 4 pages.
  //       Even if the starting address is the last byte of a ROW the entire
  //       ROW is erased anyway.

  while (dst_addr < MAX_FLASH)
  {
    // Execute "ER" Erase Row
    NVMCTRL->ADDR.reg = dst_addr / 2;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_ER;
    while (NVMCTRL->INTFLAG.bit.READY == 0)
      ;
    dst_addr += PAGE_SIZE * 4; // Skip a ROW
  }
}

static void sam_ba_writetoflash(uint32_t *src_addr, uint32_t *dst_addr, uint32_t size)
{
  // Set automatic page write
  NVMCTRL->CTRLB.bit.MANW = 0;

  // Do writes in pages
  while (size)
  {
    // Execute "PBC" Page Buffer Clear
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_PBC;
    while (NVMCTRL->INTFLAG.bit.READY == 0)
      ;

    // Fill page buffer
    uint32_t i;
    for (i=0; i<(PAGE_SIZE/4) && i<size; i++)
    {
      dst_addr[i] = src_addr[i];
    }

    // Execute "WP" Write Page
    //NVMCTRL->ADDR.reg = ((uint32_t)dst_addr) / 2;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_WP;
    while (NVMCTRL->INTFLAG.bit.READY == 0)
      ;

    // Advance to next page
    dst_addr += i;
    src_addr += i;
    size     -= i;
  }
}

static uint16_t sam_ba_calc_crc16(uint8_t *data, uint32_t size)
{
  uint16_t crc = 0U;

  for (uint32_t i = 0UL; i < size; ++i)
    crc = serial_add_crc(*(data++), crc);

  return crc;
}

static bool sam_ba_monitor_loop(void)
{
  length = sam_ba_getdata(ptr_monitor_if, data, SIZEBUFMAX);
  ptr = data;

  bool dataReceived = (length != 0);

  for (i = 0; i < length; i++, ptr++)
  {
    if (*ptr == 0xff) continue;

    if (*ptr == '#')
    {
      if (b_terminal_mode)
      {
        sam_ba_putdata(ptr_monitor_if, "\n\r", 2);
      }
      if (command == 'S')
      {
        //Check if some data are remaining in the "data" buffer
        if(length>i)
        {
          //Move current indexes to next avail data (currently ptr points to "#")
          ptr++;
          i++;

          //We need to add first the remaining data of the current buffer already read from usb
          //read a maximum of "current_number" bytes
          if ((length-i) < current_number)
          {
            u32tmp=(length-i);
          }
          else
          {
            u32tmp=current_number;
          }

          memcpy(ptr_data, ptr, u32tmp);
          i += u32tmp;
          ptr += u32tmp;
          j = u32tmp;
        }
        //update i with the data read from the buffer
        i--;
        ptr--;
        //Do we expect more data ?
        if(j<current_number)
          sam_ba_getdata_xmd(ptr_monitor_if, ptr_data, current_number-j);

        __asm("nop");
      }
      else if (command == 'R')
      {
        sam_ba_putdata_xmd(ptr_monitor_if, ptr_data, current_number);
      }
      else if (command == 'O')
      {
        *ptr_data = (char) current_number;
      }
      else if (command == 'H')
      {
        *((uint16_t *) ptr_data) = (uint16_t) current_number;
      }
      else if (command == 'W')
      {
        *((int *) ptr_data) = current_number;
      }
      else if (command == 'o')
      {
        sam_ba_putdata_term(ptr_data, 1);
      }
      else if (command == 'h')
      {
        current_number = *((uint16_t *) ptr_data);
        sam_ba_putdata_term((uint8_t*) &current_number, 2);
      }
      else if (command == 'w')
      {
        current_number = *((uint32_t *) ptr_data);
        sam_ba_putdata_term((uint8_t*) &current_number, 4);
      }
      else if (command == 'G')
      {
        call_applet(current_number);
        /* Rebase the Stack Pointer */
        __set_MSP(sp);
        __enable_irq();
        if (b_sam_ba_interface_usart) {
          ptr_monitor_if->put_c(0x6);
        }
      }
      else if (command == 'T')
      {
        b_terminal_mode = 1;
        sam_ba_putdata(ptr_monitor_if, "\n\r", 2);
      }
      else if (command == 'N')
      {
        if (b_terminal_mode == 0)
        {
          sam_ba_putdata( ptr_monitor_if, "\n\r", 2);
        }
        b_terminal_mode = 0;
      }
      else if (command == 'V')
      {
        sam_ba_putdata( ptr_monitor_if, "v", 1);
        sam_ba_putdata( ptr_monitor_if, (uint8_t *) RomBOOT_Version, strlen(RomBOOT_Version));
        sam_ba_putdata( ptr_monitor_if, " ", 1);
        sam_ba_putdata( ptr_monitor_if, (uint8_t *) RomBOOT_ExtendedCapabilities, strlen(RomBOOT_ExtendedCapabilities));
        sam_ba_putdata( ptr_monitor_if, " ", 1);
        ptr = (uint8_t*) &(__DATE__);
        i = 0;
        while (*ptr++ != '\0')
          i++;
        sam_ba_putdata( ptr_monitor_if, (uint8_t *) &(__DATE__), i);
        sam_ba_putdata( ptr_monitor_if, " ", 1);
        i = 0;
        ptr = (uint8_t*) &(__TIME__);
        while (*ptr++ != '\0')
          i++;
        sam_ba_putdata( ptr_monitor_if, (uint8_t *) &(__TIME__), i);
        sam_ba_putdata( ptr_monitor_if, "\n\r", 2);
      }
      else if (command == 'X')
      {
        // Syntax: X[ADDR]#
        // Erase the flash memory starting from ADDR to the end of flash.

        sam_ba_eraseflash(current_number);

        // Notify command completed
        sam_ba_putdata( ptr_monitor_if, "X\n\r", 3);
      }
      else if (command == 'Y')
      {
        // This command writes the content of a buffer in SRAM into flash memory.

        // Syntax: Y[ADDR],0#
        // Set the starting address of the SRAM buffer.

        // Syntax: Y[ROM_ADDR],[SIZE]#
        // Write the first SIZE bytes from the SRAM buffer (previously set) into
        // flash memory starting from address ROM_ADDR

        static uint32_t *src_buff_addr = NULL;

        if (current_number == 0)
        {
          // Set buffer address
          src_buff_addr = (uint32_t*)ptr_data;
        }
        else
        {
          sam_ba_writetoflash(src_buff_addr, (uint32_t*)ptr_data, current_number / 4U);
        }

        // Notify command completed
        sam_ba_putdata( ptr_monitor_if, "Y\n\r", 3);
      }
      else if (command == 'Z')
      {
        // This command calculate CRC for a given area of memory.
        // It's useful to quickly check if a transfer has been done
        // successfully.

        // Syntax: Z[START_ADDR],[SIZE]#
        // Returns: Z[CRC]#

        uint8_t *data = (uint8_t *)ptr_data;
        uint32_t size = current_number;
        uint16_t crc = sam_ba_calc_crc16(data, size);

        // Send response
        sam_ba_putdata( ptr_monitor_if, "Z", 1);
        put_uint32(crc);
        sam_ba_putdata( ptr_monitor_if, "#\n\r", 3);
      }

      command = 'z';
      current_number = 0;

      if (b_terminal_mode)
      {
        sam_ba_putdata( ptr_monitor_if, ">", 1);
      }
    }
    else
    {
      if (('0' <= *ptr) && (*ptr <= '9'))
      {
        current_number = (current_number << 4) | (*ptr - '0');
      }
      else if (('A' <= *ptr) && (*ptr <= 'F'))
      {
        current_number = (current_number << 4) | (*ptr - 'A' + 0xa);
      }
      else if (('a' <= *ptr) && (*ptr <= 'f'))
      {
        current_number = (current_number << 4) | (*ptr - 'a' + 0xa);
      }
      else if (*ptr == ',')
      {
        ptr_data = (uint8_t *) current_number;
        current_number = 0;
      }
      else
      {
        command = *ptr;
        current_number = 0;
      }
    }
  }

  return dataReceived;
}

// Exported value from linker script
extern uint32_t __sketch_vectors_ptr;

static uint32_t *flashProgrammingPtr;
static bool waitingForPassword = true, waitingForCrc = true;
static uint16_t receivedCrc16 = 0U;
static uint32_t imageSize = 0UL;

static bool sam_ba_monitor_loop_tftp(void)
{
   bool dataReceived = false;

   int8_t tftpStatus = tftpRun();

   if (tftpStatus == TFTP_STATUS_NO_TRAFFIC)
      return dataReceived;

   if (!TFTP_IS_ERROR_STATUS(tftpStatus) && tftpReceivedRequest() == TFTP_RXRQ_WRQ)
   {
      if (waitingForPassword)
      {
         char receivedPassword[NETCFG_PASSWORD_SIZE];

         if (tftpGetReceivedBytes((uint8_t *) receivedPassword, NETCFG_PASSWORD_SIZE))
         {
            if (memcmp(receivedPassword, netcfgData.password, NETCFG_PASSWORD_SIZE))
               tftpStatus = TFTP_STATUS_ERROR_PASSWORD;
            else
               waitingForPassword = false;
         }
         else
            tftpStatus = TFTP_STATUS_ERROR_BAD_IMAGE;
      }

      if (!TFTP_IS_ERROR_STATUS(tftpStatus) && waitingForCrc)
      {
         uint8_t receivedByte;

         if (tftpGetReceivedBytes(&receivedByte, 1))
         {
            receivedCrc16 = receivedByte << 8;

            if (tftpGetReceivedBytes(&receivedByte, 1))
            {
               receivedCrc16 |= receivedByte;
               waitingForCrc = false;
            }
            else
               tftpStatus = TFTP_STATUS_ERROR_BAD_IMAGE;
         }
         else
            tftpStatus = TFTP_STATUS_ERROR_BAD_IMAGE;
      }

      if (!TFTP_IS_ERROR_STATUS(tftpStatus))
      {
         uint8_t pageBuffer[PAGE_SIZE];

         while (tftpGetReceivedWords(pageBuffer, PAGE_SIZE_IN_WORDS))
         {
            dataReceived = true;
            sam_ba_writetoflash((uint32_t *) pageBuffer, flashProgrammingPtr, PAGE_SIZE_IN_WORDS);
            flashProgrammingPtr += PAGE_SIZE_IN_WORDS;
            imageSize += PAGE_SIZE;
         }

         if (flashProgrammingPtr - APP_FLASH_MEMORY_START_PTR >= MAX_FLASH)
            tftpStatus = TFTP_STATUS_ERROR_FULL;

         if (TFTP_IS_RX_COMPLETED(tftpStatus))
         {
            uint16_t lastWordsCount = tftpReceivedBytesCount() >> 2;

            if (lastWordsCount)
            {
               tftpGetReceivedWords(pageBuffer, lastWordsCount);
               imageSize += lastWordsCount << 2;

               for (uint16_t idx = lastWordsCount << 2; idx < PAGE_SIZE; ++idx)
                  pageBuffer[idx] = 0xFF;

               dataReceived = true;
               sam_ba_writetoflash((uint32_t *) pageBuffer, flashProgrammingPtr, PAGE_SIZE_IN_WORDS);
               flashProgrammingPtr += PAGE_SIZE_IN_WORDS;

               if (flashProgrammingPtr - APP_FLASH_MEMORY_START_PTR >= MAX_FLASH)
                  tftpStatus = TFTP_STATUS_ERROR_FULL;
            }

            if (tftpReceivedBytesCount())
               // Some bytes left, there should be none
               tftpStatus = TFTP_STATUS_ERROR_BAD_IMAGE;
            else
            {
               if (receivedCrc16 != sam_ba_calc_crc16((uint8_t *) APP_FLASH_MEMORY_START_PTR, imageSize))
                  tftpStatus = TFTP_STATUS_ERROR_CRC;
            }
         }
      }
   }

   if (tftpSendResponse(tftpStatus))
      return dataReceived;

   tftpEnd();

   if (TFTP_IS_ERROR_STATUS(tftpStatus))
   {
      // An error occurred: erasing the flash before resetting the board
      if (flashProgrammingPtr != APP_FLASH_MEMORY_START_PTR)
         sam_ba_eraseflash(APP_FLASH_MEMORY_START_ADDR);

      NVIC_SystemReset();
   }

   startApplication();

   // Just to avoid a compiler warning...
   for(;;);
}

void sam_ba_monitor_sys_tick(void)
{
	/* Check whether the TX or RX LED one-shot period has elapsed.  if so, turn off the LED */
	if (txLEDPulse && !(--txLEDPulse))
		LEDTX_off();
	if (rxLEDPulse && !(--rxLEDPulse))
		LEDRX_off();
}

/**
 * \brief This function starts the SAM-BA monitor.
 */
void sam_ba_monitor_run(bool exitAfterTimeout)
{
  uint64_t exitTime = millis() + BOOTLOADER_MAX_RUN_TIME;
  uint32_t pageSizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024 };
  PAGE_SIZE = pageSizes[NVMCTRL->PARAM.bit.PSZ];
  PAGE_SIZE_IN_WORDS = PAGE_SIZE >> 2;
  PAGES = NVMCTRL->PARAM.bit.NVMP;
  MAX_FLASH = PAGE_SIZE * PAGES;

  ptr_data = NULL;
  command = 'z';

  bool (*loopFunction)(void);

  if (com_interface == SAM_BA_NET_TFTP)
  {
    flashProgrammingPtr = APP_FLASH_MEMORY_START_PTR;
    sam_ba_eraseflash(APP_FLASH_MEMORY_START_ADDR);
    loopFunction = sam_ba_monitor_loop_tftp;
  }
  else
    loopFunction = sam_ba_monitor_loop;

  while (1)
  {
    if (loopFunction())
      exitAfterTimeout = false;

    if (exitAfterTimeout && (millis() > exitTime))
      startApplication();
  }
}
