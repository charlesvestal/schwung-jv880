/*
 * Copyright (C) 2021, 2024 nukeykt
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include
 * the complete source code, including the source code for all components used
 * by a binary built from the modified sources. However, as a special exception,
 * the source code distributed need not include anything that is normally
 * distributed (in either source or binary form) with the major components
 * (compiler, kernel, and so on) of the operating system on which the executable
 * runs, unless that component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
#include "mcu.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <cstdlib>

#if __linux__
#include <limits.h>
#include <unistd.h>
#endif

void MCU::MCU_ErrorTrap() { printf("trap %.2x %.4x\n", mcu.cp, mcu.pc); }

uint16_t MCU::MCU_AnalogReadPin(const uint32_t pin) {
  if (pin == 1)
    return ANALOG_LEVEL_BATTERY;
  return 0x3ff;
}

void MCU::MCU_AnalogSample(const int channel) {
  int value = MCU_AnalogReadPin(channel);
  int dest = (channel << 1) & 6;
  dev_register[DEV_ADDRAH + dest] = value >> 2;
  dev_register[DEV_ADDRAL + dest] = (value << 6) & 0xc0;
}

void MCU::MCU_DeviceWrite(uint32_t address, const uint8_t data) {
  address &= 0x7f;
  if (address >= 0x10 && address < 0x40) {
    TIMER_Write(address, data);
    return;
  }
  if (address >= 0x50 && address < 0x55) {
    TIMER2_Write(address, data);
    return;
  }
  switch (address) {
  case DEV_P1DDR: // P1DDR
    break;
  case DEV_P5DDR:
    break;
  case DEV_P6DDR:
    break;
  case DEV_P7DDR:
    break;
  case DEV_SCR:
    break;
  case DEV_WCR:
    break;
  case DEV_P9DDR:
    break;
  case DEV_RAME: // RAME
    break;
  case DEV_P1CR: // P1CR
    break;
  case DEV_DTEA:
    break;
  case DEV_DTEB:
    break;
  case DEV_DTEC:
    break;
  case DEV_DTED:
    break;
  case DEV_SMR:
    break;
  case DEV_BRR:
    break;
  case DEV_IPRA:
    break;
  case DEV_IPRB:
    break;
  case DEV_IPRC:
    break;
  case DEV_IPRD:
    break;
  case DEV_PWM1_DTR:
    break;
  case DEV_PWM1_TCR:
    break;
  case DEV_PWM2_DTR:
    break;
  case DEV_PWM2_TCR:
    break;
  case DEV_PWM3_DTR:
    break;
  case DEV_PWM3_TCR:
    break;
  case DEV_P7DR:
    break;
  case DEV_TMR_TCNT:
    break;
  case DEV_TMR_TCR:
    break;
  case DEV_TMR_TCSR:
    break;
  case DEV_TMR_TCORA:
    break;
  case DEV_TDR:
    break;
  case DEV_ADCSR: {
    dev_register[address] &= ~0x7f;
    dev_register[address] |= data & 0x7f;
    if ((data & 0x80) == 0 && adf_rd) {
      dev_register[address] &= ~0x80;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_ANALOG, 0);
    }
    if ((data & 0x40) == 0)
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_ANALOG, 0);
    return;
  }
  case DEV_SSR: {
    if ((data & 0x80) == 0 && (ssr_rd & 0x80) != 0) {
      dev_register[address] &= ~0x80;
      uart_tx_delay = mcu.cycles + 3000;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_TX, 0);
    }
    if ((data & 0x40) == 0 && (ssr_rd & 0x40) != 0) {
      uart_rx_delay = mcu.cycles + 3000;
      dev_register[address] &= ~0x40;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_RX, 0);
    }
    if ((data & 0x20) == 0 && (ssr_rd & 0x20) != 0) {
      dev_register[address] &= ~0x20;
    }
    if ((data & 0x10) == 0 && (ssr_rd & 0x10) != 0) {
      dev_register[address] &= ~0x10;
    }
    break;
  }
  default:
    address += 0;
    break;
  }
  dev_register[address] = data;
}

uint8_t MCU::MCU_DeviceRead(uint32_t address) {
  address &= 0x7f;
  if (address >= 0x10 && address < 0x40) {
    return TIMER_Read(address);
  }
  if (address >= 0x50 && address < 0x55) {
    return TIMER_Read2(address);
  }
  switch (address) {
  case DEV_ADDRAH:
  case DEV_ADDRAL:
  case DEV_ADDRBH:
  case DEV_ADDRBL:
  case DEV_ADDRCH:
  case DEV_ADDRCL:
  case DEV_ADDRDH:
  case DEV_ADDRDL:
    return dev_register[address];
  case DEV_ADCSR:
    adf_rd = (dev_register[address] & 0x80) != 0;
    return dev_register[address];
  case DEV_SSR:
    ssr_rd = dev_register[address];
    return dev_register[address];
  case DEV_RDR:
    return uart_rx_byte;
  case 0x00:
    return 0xff;
  case DEV_P7DR: {
    uint8_t data = 0xff;
    uint32_t button_pressed = mcu_button_pressed;

    if (io_sd == 0b11111011)
      data &= ((button_pressed >> 0) & 0b11111) ^ 0xFF;
    if (io_sd == 0b11110111)
      data &= ((button_pressed >> 5) & 0b11111) ^ 0xFF;
    if (io_sd == 0b11101111)
      data &= ((button_pressed >> 10) & 0b1111) ^ 0xFF;

    data |= 0b10000000;
    return data;
  }
  case DEV_P9DR: {
    int cfg = 2;
    int dir = dev_register[DEV_P9DDR];

    int val = cfg & (dir ^ 0xff);
    val |= dev_register[DEV_P9DR] & dir;
    return val;
  }
  case DEV_SCR:
    if (dev_register[address] == 0x30)
      midi_ready = true; // FIXME
    return dev_register[address];
  case DEV_TDR:
    return dev_register[address];
  case DEV_SMR:
    return dev_register[address];
  case DEV_IPRC:
  case DEV_IPRD:
  case DEV_DTEC:
  case DEV_DTED:
  case DEV_FRT2_TCSR:
  case DEV_FRT1_TCSR:
  case DEV_FRT1_TCR:
  case DEV_FRT1_FRCH:
  case DEV_FRT1_FRCL:
  case DEV_FRT3_TCSR:
  case DEV_FRT3_OCRAH:
  case DEV_FRT3_OCRAL:
    return dev_register[address];
  }
  return dev_register[address];
}

void MCU::MCU_DeviceReset() {
  dev_register[DEV_RAME] = 0x80;
  dev_register[DEV_SSR] = 0x80;
}

void MCU::MCU_UpdateAnalog(const uint64_t cycles) {
  int ctrl = dev_register[DEV_ADCSR];
  int isscan = (ctrl & 16) != 0;

  if (ctrl & 0x20) {
    if (analog_end_time == 0)
      analog_end_time = cycles + 200;
    else if (analog_end_time < cycles) {
      if (isscan) {
        int base = ctrl & 4;
        for (int i = 0; i <= (ctrl & 3); i++)
          MCU_AnalogSample(base + i);
        analog_end_time = cycles + 200;
      } else {
        MCU_AnalogSample(ctrl & 7);
        dev_register[DEV_ADCSR] &= ~0x20;
        analog_end_time = 0;
      }
      dev_register[DEV_ADCSR] |= 0x80;
      if (ctrl & 0x40)
        MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_ANALOG, 1);
    }
  } else
    analog_end_time = 0;
}

uint8_t MCU::MCU_Read(uint32_t address) {
  uint32_t address_rom = address & 0x3ffff;
  uint8_t page = (address >> 16) & 0xf;
  address &= 0xffff;
  uint8_t ret = 0xff;
  switch (page) {
  case 0:
    if (!(address & 0x8000))
      ret = rom1[address & 0x7fff];
    else {
      if (address >= 0xfb80 && address < 0xff80 &&
          (dev_register[DEV_RAME] & 0x80) != 0)
        ret = ram[(address - 0xfb80) & 0x3ff];
      else if (address >= 0x8000 && address < 0xe000)
        ret = sram[address & 0x7fff];
      else if (address >= 0xff80)
        ret = MCU_DeviceRead(address & 0x7f);
      else if (address >= 0xf000 && address < 0xf400)
        ret = pcm.PCM_Read(address & 0x3f);
      else if (address == 0xf402) {
        ret = ga_int_trigger;
        ga_int_trigger = 0;
        MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_IRQ0, 0);
      }
      // else
      //     printf("Unknown read %x%04x\n", page, address);
    }
    break;
  case 1:
    ret = rom2[address_rom & rom2_mask];
    break;
  case 2:
    ret = rom2[address_rom & rom2_mask];
    break;
  case 3:
    ret = rom2[address_rom & rom2_mask];
    break;
  case 4:
    ret = rom2[address_rom & rom2_mask];
    break;
  case 14:
  case 15:
    ret = cardram[address & 0x7fff];
    break;
  case 10:
  case 11:
    ret = sram[address & 0x7fff];
    break;
  case 12:
  case 13:
    ret = nvram[address & 0x7fff];
    break;
    // default:
    //     printf("Unknown read %x%04x\n", page, address);
  }
  return ret;
}

void MCU::MCU_Write(uint32_t address, const uint8_t value) {
  uint8_t page = (address >> 16) & 0xf;
  address &= 0xffff;
  if (page == 0 && address & 0x8000) {
    if (address >= 0xfb80 && address < 0xff80 &&
        (dev_register[DEV_RAME] & 0x80) != 0)
      ram[(address - 0xfb80) & 0x3ff] = value;
    else if (address < 0xe000)
      sram[address & 0x7fff] = value;
    else if (address >= 0xf400 && address < 0xf800) {
      if (address == 0xf404 || address == 0xf405)
        lcd.LCD_Write(address & 1, value);
      else if (address == 0xf401) {
        io_sd = value;
        lcd.LCD_Enable((value & 1) == 0);
      } else if (address == 0xf402)
        ga_int_enable = (value << 1);
    } else if (address >= 0xf100 && address < 0xf110) {
      // JV-880 alternate LCD addresses
      if (address == 0xf105)
        lcd.LCD_Write(0, value);
      else if (address == 0xf104)
        lcd.LCD_Write(1, value);
    } else if (address >= 0xf000 && address < 0xf400)
      pcm.PCM_Write(address & 0x3f, value);
    else if (address >= 0xff80)
      MCU_DeviceWrite(address & 0x7f, value);
    // else
    //     printf("Unknown write %x%04x\n", page, address);
  } else if (page == 10)
    sram[address & 0x7fff] = value;
  else if (page == 12)
    nvram[address & 0x7fff] = value;
  else if (page == 14)
    cardram[address & 0x7fff] = value;
  // else
  //     printf("Unknown write %x%04x\n", page, address);
}

void MCU::MCU_Init() { memset(&mcu, 0, sizeof(mcu_t)); }

void MCU::MCU_Reset() {
  mcu.r[0] = 0;
  mcu.r[1] = 0;
  mcu.r[2] = 0;
  mcu.r[3] = 0;
  mcu.r[4] = 0;
  mcu.r[5] = 0;
  mcu.r[6] = 0;
  mcu.r[7] = 0;

  mcu.pc = 0;

  mcu.sr = 0x700;

  mcu.cp = 0;
  mcu.dp = 0;
  mcu.ep = 0;
  mcu.tp = 0;
  mcu.br = 0;

  uint32_t reset_address = MCU_GetVectorAddress(VECTOR_RESET);
  mcu.cp = (reset_address >> 16) & 0xff;
  mcu.pc = reset_address & 0xffff;

  mcu.exception_pending = -1;
  mcu.interrupt_pending_any = 1; // ensure first scan always runs

  MCU_DeviceReset();
}

void MCU::MCU_PostUART(const uint8_t data) {
  if (!midi_ready)
    return;
  uart_buffer[uart_write_ptr] = data;
  uart_write_ptr = (uart_write_ptr + 1) % uart_buffer_size;
}

void MCU::MCU_UpdateUART_RX() {
  if ((dev_register[DEV_SCR] & 16) == 0) // RX disabled
    return;
  if (uart_write_ptr == uart_read_ptr) // no byte
    return;

  if (dev_register[DEV_SSR] & 0x40)
    return;

  if (mcu.cycles < uart_rx_delay)
    return;

  uart_rx_byte = uart_buffer[uart_read_ptr];
  uart_read_ptr = (uart_read_ptr + 1) % uart_buffer_size;
  dev_register[DEV_SSR] |= 0x40;
  MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_RX,
                           (dev_register[DEV_SCR] & 0x40) != 0);
}

// dummy TX
void MCU::MCU_UpdateUART_TX() {
  if ((dev_register[DEV_SCR] & 32) == 0) // TX disabled
    return;

  if (dev_register[DEV_SSR] & 0x80)
    return;

  if (mcu.cycles < uart_tx_delay)
    return;

  dev_register[DEV_SSR] |= 0x80;
  MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_TX,
                           (dev_register[DEV_SCR] & 0x80) != 0);

  // printf("tx:%x\n", dev_register[DEV_TDR]);
}

void MCU::MCU_PatchROM() {
  // Disable intro
  rom2[0x318f7] = 0x19;
}

void unscramble(const uint8_t *src, uint8_t *dst, const int len) {
  for (int i = 0; i < len; i++) {
    int address = i & ~0xfffff;
    static const int aa[] = {2, 0,  3,  4,  1, 9, 13, 10, 18, 17,
                             6, 15, 11, 16, 8, 5, 12, 7,  14, 19};
    for (int j = 0; j < 20; j++) {
      if (i & (1 << j))
        address |= 1 << aa[j];
    }
    uint8_t srcdata = src[address];
    uint8_t data = 0;
    static const int dd[] = {2, 0, 4, 5, 7, 6, 3, 1};
    for (int j = 0; j < 8; j++) {
      if (srcdata & (1 << dd[j]))
        data |= 1 << j;
    }
    dst[i] = data;
  }
}

void MCU::MCU_GA_SetGAInt(const int line, const int value) {
  // guesswork
  if (value && !ga_int[line] && (ga_int_enable & (1 << line)) != 0)
    ga_int_trigger = line;
  ga_int[line] = value;

  MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_IRQ0, ga_int_trigger != 0);
}

void MCU::MCU_EncoderTrigger(const int dir) {
  MCU_GA_SetGAInt(dir == 0 ? 3 : 4, 0);
  MCU_GA_SetGAInt(dir == 0 ? 3 : 4, 1);
}

void MCU::MCU_Interrupt_Handle() {
  // Fast path: skip the scan entirely when provably nothing is pending.
  // The flag is set by every code path that can make an interrupt/exception
  // become pending (SetRequest with value!=0, Exception, TRAPA).  It is
  // cleared here only when the scan observes zero pending requests across ALL
  // sources, including masked ones — so a pending-but-masked interrupt keeps
  // the flag set and will be re-checked whenever the firmware lowers the mask.
  if (!mcu.interrupt_pending_any)
    return;

  uint32_t i;
  bool any_pending = false; // tracks whether anything remains pending after scan

  for (i = 0; i <= 8; i++) {
    if (mcu.trapa_pending[i]) {
      any_pending = true; // there are pending trapas (this one and possibly more)
      mcu.trapa_pending[i] = 0;
      MCU_Interrupt_StartVector(VECTOR_TRAPA_0 + i, -1);
      return; // flag stays set — remaining trapas or interrupts may still be pending
    }
  }
  uint32_t mask = (mcu.sr >> 8) & 7;
  for (i = INTERRUPT_SOURCE_NMI + 1; i < INTERRUPT_SOURCE_MAX; i++) {
    int32_t vector = -1;
    int32_t level = 0;
    if (!mcu.interrupt_pending[i])
      continue;
    // At least one interrupt_pending slot is set — even if ultimately masked,
    // we must keep any_pending true so we re-scan after a mask change.
    any_pending = true;
    switch (i) {
    case INTERRUPT_SOURCE_IRQ0:
      if ((dev_register[DEV_P1CR] & 0x20) == 0)
        continue;
      vector = VECTOR_IRQ0;
      level = (dev_register[DEV_IPRA] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_IRQ1:
      if ((dev_register[DEV_P1CR] & 0x40) == 0)
        continue;
      vector = VECTOR_IRQ1;
      level = (dev_register[DEV_IPRA] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_FRT0_OCIA:
      vector = VECTOR_INTERNAL_INTERRUPT_94;
      level = (dev_register[DEV_IPRB] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_FRT0_OCIB:
      vector = VECTOR_INTERNAL_INTERRUPT_98;
      level = (dev_register[DEV_IPRB] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_FRT0_FOVI:
      vector = VECTOR_INTERNAL_INTERRUPT_9C;
      level = (dev_register[DEV_IPRB] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_FRT1_OCIA:
      vector = VECTOR_INTERNAL_INTERRUPT_A4;
      level = (dev_register[DEV_IPRB] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_FRT1_OCIB:
      vector = VECTOR_INTERNAL_INTERRUPT_A8;
      level = (dev_register[DEV_IPRB] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_FRT1_FOVI:
      vector = VECTOR_INTERNAL_INTERRUPT_AC;
      level = (dev_register[DEV_IPRB] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_FRT2_OCIA:
      vector = VECTOR_INTERNAL_INTERRUPT_B4;
      level = (dev_register[DEV_IPRC] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_FRT2_OCIB:
      vector = VECTOR_INTERNAL_INTERRUPT_B8;
      level = (dev_register[DEV_IPRC] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_FRT2_FOVI:
      vector = VECTOR_INTERNAL_INTERRUPT_BC;
      level = (dev_register[DEV_IPRC] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_TIMER_CMIA:
      vector = VECTOR_INTERNAL_INTERRUPT_C0;
      level = (dev_register[DEV_IPRC] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_TIMER_CMIB:
      vector = VECTOR_INTERNAL_INTERRUPT_C4;
      level = (dev_register[DEV_IPRC] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_TIMER_OVI:
      vector = VECTOR_INTERNAL_INTERRUPT_C8;
      level = (dev_register[DEV_IPRC] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_ANALOG:
      vector = VECTOR_INTERNAL_INTERRUPT_E0;
      level = (dev_register[DEV_IPRD] >> 0) & 7;
      break;
    case INTERRUPT_SOURCE_UART_RX:
      vector = VECTOR_INTERNAL_INTERRUPT_D4;
      level = (dev_register[DEV_IPRD] >> 4) & 7;
      break;
    case INTERRUPT_SOURCE_UART_TX:
      vector = VECTOR_INTERNAL_INTERRUPT_D8;
      level = (dev_register[DEV_IPRD] >> 4) & 7;
      break;
    default:
      break;
    }

    if ((int32_t)mask < level) {
      MCU_Interrupt_StartVector(vector, level);
      return; // flag stays set — remaining pending interrupts need future scans
    }
  }

  // Scan completed without dispatching anything.  Clear flag only when no
  // pending sources were observed; if any source is pending-but-masked we must
  // keep the flag so we re-scan when the mask drops (no new SetRequest fires).
  if (!any_pending)
    mcu.interrupt_pending_any = 0;
}

void MCU::TIMER_Reset() {
  timer_tempreg = 0;

  timer8_enabled = false;
  timer8_cmiea = false;
  timer8_cmfa = false;
  timer8_cmfa_read = false;
  timer8_tcora = 0;
  timer8_tcnt = 0;

  timer0_ocra = 0;
  timer1_ocra = 0;
  timer2_ocra = 0;
  timer0_frc = 0;
  timer1_frc = 0;
  timer2_frc = 0;
  timer0_ocfa = false;
  timer1_ocfa = false;
  timer2_ocfa = false;
  timer0_ocfa_read = false;
  timer1_ocfa_read = false;
  timer2_ocfa_read = false;
  timer0_ociea = false;
  timer1_ociea = false;
  timer2_ociea = false;
}

void MCU::TIMER_Write(const uint32_t address, const uint8_t data) {
  switch (address) {
  case DEV_FRT1_TCR:
    timer0_ociea = data == 0b00100000;
    break;
  case DEV_FRT2_TCR:
    timer1_ociea = data == 0b00100000;
    break;
  case DEV_FRT3_TCR:
    timer2_ociea = data == 0b00100000;
    break;
  case DEV_FRT1_TCSR:
    if ((data & 0x20) == 0 && timer0_ocfa_read) {
      timer0_ocfa = false;
      timer0_ocfa_read = false;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_FRT0_OCIA, 0);
    }
    break;
  case DEV_FRT2_TCSR:
    if ((data & 0x20) == 0 && timer1_ocfa_read) {
      timer1_ocfa = false;
      timer1_ocfa_read = false;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_FRT1_OCIA, 0);
    }
    break;
  case DEV_FRT3_TCSR:
    if ((data & 0x20) == 0 && timer2_ocfa_read) {
      timer2_ocfa = false;
      timer2_ocfa_read = false;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_FRT2_OCIA, 0);
    }
    break;
  case DEV_FRT1_OCRAH:
  case DEV_FRT2_OCRAH:
  case DEV_FRT3_OCRAH:
    timer_tempreg = data;
    break;
  case DEV_FRT1_OCRAL:
    timer0_ocra = (timer_tempreg << 8) | data;
    break;
  case DEV_FRT2_OCRAL:
    timer1_ocra = (timer_tempreg << 8) | data;
    break;
  case DEV_FRT3_OCRAL:
    timer2_ocra = (timer_tempreg << 8) | data;
    break;
  }
}

uint8_t MCU::TIMER_Read(const uint32_t address) {
  uint8_t ret;
  switch (address) {
  case DEV_FRT1_TCSR:
    ret = 0b01110001;
    timer0_ocfa_read |= timer0_ocfa;
    return ret;
  case DEV_FRT2_TCSR:
    ret = 0b01110001;
    timer1_ocfa_read |= timer1_ocfa;
    return ret;
  case DEV_FRT3_TCSR:
    ret = 0b01110001;
    timer2_ocfa_read |= timer2_ocfa;
    return ret;
  }
  return 0xff;
}

void MCU::TIMER2_Write(const uint32_t address, const uint8_t data) {
  switch (address) {
  case DEV_TMR_TCR:
    timer8_enabled = data & 1;
    timer8_cmiea = data >> 6;
    break;
  case DEV_TMR_TCSR:
    if ((data & 0x40) == 0 && timer8_cmfa) {
      timer8_cmfa = false;
      timer8_cmfa_read = false;
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_TIMER_CMIA, 0);
    }
    break;
  case DEV_TMR_TCORA:
    timer8_tcora = data;
    break;
  case DEV_TMR_TCNT:
    timer8_tcnt = data;
    break;
  }
}
uint8_t MCU::TIMER_Read2(const uint32_t address) {
  if (address == DEV_TMR_TCSR) {
    uint8_t ret = timer8_cmfa ? 0b11100000 : 0b10100000;
    timer8_cmfa_read |= timer8_cmfa;
    return ret;
  }
  return 0xff;
}

void MCU::TIMER_Clock(const uint64_t cycles) {
  if (timer8_enabled && (cycles & 0x3f) == 0) {
    timer8_cmfa = true;
    if (timer8_cmiea)
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_TIMER_CMIA, 1);
  }

  {
    bool matcha = (timer0_frc >> 2) >= timer0_ocra;
    if (matcha)
      timer0_frc = 0;
    else
      timer0_frc += 6;

    if (matcha)
      timer0_ocfa |= 0x20;
    if (timer0_ociea && matcha)
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_FRT0_OCIA, 1);
  }
  {
    bool matcha = (timer1_frc >> 2) >= timer1_ocra;
    if (matcha)
      timer1_frc = 0;
    else
      timer1_frc += 6;

    if (matcha)
      timer1_ocfa |= 0x20;
    if (timer1_ociea && matcha)
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_FRT1_OCIA, 1);
  }
  {
    bool matcha = (timer2_frc >> 2) >= timer2_ocra;
    if (matcha)
      timer2_frc = 0;
    else
      timer2_frc += 6;

    if (matcha)
      timer2_ocfa |= 0x20;
    if (timer2_ociea && matcha)
      MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_FRT2_OCIA, 1);
  }
}

MCU::MCU() : pcm(this), lcd(this) {}

int MCU::startSC55(const uint8_t *s_rom1, const uint8_t *s_rom2,
                   const uint8_t *s_waverom1, const uint8_t *s_waverom2,
                   const uint8_t *s_nvram) {
  uint8_t *tempbuf = (uint8_t *)malloc(0x800000);

  memset(&mcu, 0, sizeof(mcu_t));

  memcpy(rom1, s_rom1, ROM1_SIZE);
  memcpy(rom2, s_rom2, ROM2_SIZE);
  memcpy(nvram, s_nvram, NVRAM_SIZE);

  memcpy(tempbuf, s_waverom1, 0x200000);
  unscramble(tempbuf, pcm.waverom1, 0x200000);
  memcpy(tempbuf, s_waverom2, 0x200000);
  unscramble(tempbuf, pcm.waverom2, 0x200000);

  free(tempbuf);

  SC55_Reset();

  return 0;
}

// Replay the FRT (timer0/1/2) counters for 'n' skipped TIMER_Clock calls during
// a SLEEP fast-forward. Mirrors TIMER_Clock's per-FRT frc/ocfa arithmetic exactly
// but omits the interrupt request: the caller only invokes this for skip windows
// that contain no OBSERVABLE match (see MCU_NextEventCycles / frtNextObservable),
// so every match replayed here is non-interrupting and ocfa is already set (the
// |= is a no-op). frc still must be reproduced bit-exactly because a match resets
// it to 0, so we run the same conditional advancement n times.
void MCU::FRT_AdvanceSkipped(uint64_t n) {
  for (uint64_t s = 0; s < n; s++) {
    if ((timer0_frc >> 2) >= timer0_ocra) {
      timer0_frc = 0;
      timer0_ocfa |= 0x20;
    } else
      timer0_frc += 6;
    if ((timer1_frc >> 2) >= timer1_ocra) {
      timer1_frc = 0;
      timer1_ocfa |= 0x20;
    } else
      timer1_frc += 6;
    if ((timer2_frc >> 2) >= timer2_ocra) {
      timer2_frc = 0;
      timer2_ocfa |= 0x20;
    } else
      timer2_frc += 6;
  }
}

// Compute the earliest peripheral-step boundary (a multiple of STEP_CYCLES,
// strictly greater than the current mcu.cycles) at which ANY peripheral could
// produce an observable effect. Used to fast-forward through SLEEP without
// running the per-step machinery on cycles where provably nothing happens.
//
// Step model: in the normal loop mcu.cycles only ever increments by 12, starting
// from 0, so every peripheral evaluation lands on a multiple of 12. An event
// "at" cycle C is serviced at the first multiple-of-12 boundary >= C (or > C for
// strictly-greater comparisons). We reproduce those exact service-time semantics
// by returning the absolute boundary cycle of the soonest event; the caller jumps
// mcu.cycles there and runs one normal iteration, then re-evaluates.
uint64_t MCU::MCU_NextEventCycles() {
  const uint64_t STEP = 12;
  const uint64_t now = mcu.cycles; // always a multiple of 12
  uint64_t best = UINT64_MAX;

  // first multiple of STEP strictly greater than x
  auto nextStepAfter = [STEP](uint64_t x) -> uint64_t {
    return (x / STEP + 1) * STEP;
  };
  // first multiple of STEP greater than or equal to x
  auto stepAtLeast = [STEP](uint64_t x) -> uint64_t {
    return ((x + STEP - 1) / STEP) * STEP;
  };

  // --- FRT timer0/1/2 -------------------------------------------------------
  // TIMER_Clock advances each frc by +6 PER CALL (one call per step) and, on a
  // match (frc>>2 >= ocra at the START of the call), resets frc to 0, sets
  // ocfa |= 0x20, and (if ociea) raises an interrupt. The cadence is call-count
  // driven (independent of cycle value): the match occurs at call j (step
  // now+12*j) with j the smallest j>=1 s.t. frc + 6*(j-1) >= ocra*4.
  //
  // A match is OBSERVABLE only when it (a) raises an interrupt (ociea set, which
  // can wake the CPU) OR (b) transitions ocfa from clear to set (a sticky pollable
  // flag the firmware can read after waking via some other source). When neither
  // holds — ociea clear AND ocfa already set — the match changes nothing the
  // firmware can observe: ocfa stays set and frc merely resets. (Observed in this
  // firmware: timer2 runs with ocra==0, ociea==0, ocfa already set, so it
  // "matches" every single step yet is completely invisible.) We therefore bind
  // the jump only on the next OBSERVABLE match; non-observable matches are
  // replayed exactly by FRT_AdvanceSkipped() so frc/ocfa stay bit-exact.
  //
  // frc is a uint16 counter; if ocra*4 > 0xffff the target is unreachable
  // (frc>>2 < ocra always) -> no match ever.
  auto frtNextObservable = [&](uint32_t frc, uint32_t ocra, bool ociea,
                               bool ocfa) -> uint64_t {
    if (ociea == false && ocfa == true)
      return UINT64_MAX; // matches are invisible; replayed during the skip
    uint64_t target = (uint64_t)ocra << 2; // frc value needed for a match
    if (target > 0xffff)
      return UINT64_MAX; // unreachable
    if (frc >= target)
      return now + STEP; // matches on the very next call (j=1)
    uint64_t deficit = target - frc;
    uint64_t jm1 = (deficit + 5) / 6; // ceil(deficit/6) = (j-1), >=1
    return now + STEP * (jm1 + 1);     // step of call j = now + 12*j
  };
  best = std::min(best, frtNextObservable(timer0_frc, timer0_ocra,
                                          timer0_ociea, timer0_ocfa));
  best = std::min(best, frtNextObservable(timer1_frc, timer1_ocra,
                                          timer1_ociea, timer1_ocfa));
  best = std::min(best, frtNextObservable(timer2_frc, timer2_ocra,
                                          timer2_ociea, timer2_ocfa));

  // --- timer8 (8-bit TMR) ---------------------------------------------------
  // Fires when enabled and (cycles & 0x3f)==0. Since cycles is always a multiple
  // of 12, the combined condition (mult of 64 AND mult of 12) holds exactly on
  // multiples of lcm(12,64)=192. Next event = next multiple of 192 > now.
  if (timer8_enabled) {
    uint64_t next192 = (now / 192 + 1) * 192;
    best = std::min(best, next192);
  }

  // --- UART RX --------------------------------------------------------------
  // Fires when RX enabled (SCR&16), a byte is queued (write!=read ptr), RDRF
  // (SSR&0x40) clear, and mcu.cycles >= uart_rx_delay. The byte queue only
  // changes via MCU_PostUART, which runs between updateSC55 calls (never during
  // this loop), so these gates are constant across the jump. If satisfiable, the
  // event lands at the first step >= uart_rx_delay.
  if ((dev_register[DEV_SCR] & 16) && (uart_write_ptr != uart_read_ptr) &&
      !(dev_register[DEV_SSR] & 0x40)) {
    uint64_t when =
        (uart_rx_delay <= now) ? (now + STEP) : stepAtLeast(uart_rx_delay);
    best = std::min(best, when);
  }

  // --- UART TX --------------------------------------------------------------
  // Fires when TX enabled (SCR&32), TDRE (SSR&0x80) clear, and
  // mcu.cycles >= uart_tx_delay. Gates constant across the jump (SSR/SCR only
  // change via firmware, which is asleep).
  if ((dev_register[DEV_SCR] & 32) && !(dev_register[DEV_SSR] & 0x80)) {
    uint64_t when =
        (uart_tx_delay <= now) ? (now + STEP) : stepAtLeast(uart_tx_delay);
    best = std::min(best, when);
  }

  // --- Analog (A/D) ---------------------------------------------------------
  // MCU_UpdateAnalog: when ADST (ctrl&0x20) set and analog_end_time==0, the NEXT
  // call schedules analog_end_time = cycles+200 (cycle-value dependent), so we
  // must not skip it -> bound to next step. When analog_end_time!=0, the sample
  // fires on the first call with cycles > analog_end_time. When ADST clear but
  // analog_end_time!=0, the next call resets it to 0 (a state change) -> next
  // step.
  {
    int ctrl = dev_register[DEV_ADCSR];
    if (ctrl & 0x20) {
      if (analog_end_time == 0)
        best = std::min(best, now + STEP);
      else
        best = std::min(best, nextStepAfter(analog_end_time));
    } else if (analog_end_time != 0) {
      best = std::min(best, now + STEP);
    }
  }

  // --- PCM ------------------------------------------------------------------
  // PCM_Update(mcu.cycles) runs its body while pcm.cycles < cycles, producing
  // samples (which can terminate updateSC55 by satisfying nSamples) and advancing
  // pcm.cycles by a fixed step per body. Its accounting is purely delta-based on
  // (pcm.cycles vs the passed cycles), so skipping intermediate calls is safe.
  // The next body execution requires the passed cycles to exceed the current
  // pcm.cycles, i.e. the first step boundary strictly greater than pcm.cycles.
  // Including this guarantees we never jump past the cycle producing the sample
  // that satisfies nSamples.
  best = std::min(best, nextStepAfter(pcm.pcm.cycles));

  return best;
}

void MCU::updateSC55(const int nSamples) {
  sample_write_ptr = 0;
  while (sample_write_ptr < nSamples) {
    if (!mcu.ex_ignore)
      MCU_Interrupt_Handle();
    else
      mcu.ex_ignore = 0;

    // Fast-forward through SLEEP: while the CPU is asleep no instruction
    // executes, so the only state changes come from peripherals. Jump mcu.cycles
    // directly to the soonest step at which any peripheral could act, instead of
    // single-stepping 12 cycles at a time.
    //
    // Gating on mcu.sleep ALONE (not !interrupt_pending_any) is correct and
    // necessary: during SLEEP the firmware is waiting on an interrupt that is
    // pending-but-masked, so interrupt_pending_any stays set the entire time
    // (MCU_Interrupt_Handle scans every step, finds the masked source, and never
    // clears the flag). Reaching this point with mcu.sleep still set means the
    // Handle() call at the top of THIS iteration did not dispatch (StartVector
    // clears sleep), i.e. nothing is currently dispatchable. The dispatch outcome
    // can only change via (a) a new MCU_Interrupt_SetRequest from a peripheral,
    // or (b) a change to the SR interrupt mask. (b) only happens in firmware
    // (asleep) or StartVector (not reached), so it is invariant across the
    // skipped steps. (a) can only come from the peripherals enumerated in
    // MCU_NextEventCycles() — timers, UART, analog, and PCM (whose body may raise
    // the PCM/GA IRQ). External MIDI and encoder/GA input arrive via
    // postMidiSC55 / MCU_EncoderTrigger BETWEEN updateSC55 calls, never mid-loop.
    // Therefore jumping to the soonest peripheral event and re-running Handle()
    // there reproduces wake timing exactly.
    if (mcu.sleep) {
      uint64_t target = MCU_NextEventCycles();
      // target is a multiple of 12 strictly > mcu.cycles (or UINT64_MAX). If
      // bounded and more than one step ahead, jump to it. Count each skipped
      // 12-cycle slot toward both probe counters so the sleep fraction stays
      // comparable, then run one normal iteration at the boundary below.
      if (target != UINT64_MAX && target > mcu.cycles + 12) {
        uint64_t skipped = (target - 12 - mcu.cycles) / 12;
        probe_steps += skipped;
        probe_sleep_steps += skipped;
        // Replay the FRT counters for the skipped TIMER_Clock calls. By
        // construction the jump stops at/before the next OBSERVABLE match, so any
        // match occurring on a skipped step is non-observable (ociea clear AND
        // ocfa already set): it raises NO interrupt and the ocfa |= 0x20 is a
        // no-op. We still must reproduce frc's exact value (matches reset it to
        // 0), so we replicate TIMER_Clock's frc/ocfa arithmetic verbatim, minus
        // the interrupt. The target step's own TIMER_Clock (run normally below)
        // performs the final advancement. 'skipped' is small (bounded by the
        // soonest peripheral event, typically the A/D 200-cycle or PCM step), so
        // the loop is cheap.
        FRT_AdvanceSkipped(skipped);
        mcu.cycles = target - 12; // the +=12 below lands exactly on target
      }
    }

    probe_steps++;
    if (!mcu.sleep)
      MCU_ReadInstruction();
    else
      probe_sleep_steps++;

    mcu.cycles += 12; // FIXME: assume 12 cycles per instruction

    TIMER_Clock(mcu.cycles);
    MCU_UpdateUART_RX();
    MCU_UpdateUART_TX();
    MCU_UpdateAnalog(mcu.cycles);

    pcm.PCM_Update(mcu.cycles);
  }
}

void MCU::SC55_Reset() {
  mcu_button_pressed = 0x00;
  memset(ga_int, 0x00, sizeof(ga_int));
  ga_int_enable = 0;
  ga_int_trigger = 0;
  ga_lcd_counter = 0;
  io_sd = 0x00;
  adf_rd = 0;
  analog_end_time = 0;
  ssr_rd = 0;
  midi_ready = false;
  uart_write_ptr = 0x00;
  uart_read_ptr = 0x00;
  memset(uart_buffer, 0x00, uart_buffer_size);
  uart_rx_byte = 0x00;
  uart_rx_delay = 0x00;
  uart_tx_delay = 0x00;
  memset(dev_register, 0, sizeof(dev_register));

  MCU_Init();
  MCU_PatchROM();
  MCU_Reset();
  pcm.PCM_Reset();
  TIMER_Reset();

  sample_write_ptr = 0;
}

void MCU::postMidiSC55(const uint8_t *message, const int length) {
  for (int i = 0; i < length; i++) {
    MCU_PostUART(message[i]);
  }
}
