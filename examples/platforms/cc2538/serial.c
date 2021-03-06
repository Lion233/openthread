/*
 *  Copyright (c) 2016, Nest Labs, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements the OpenThread platform abstraction for serial communication.
 *
 */

#include <stddef.h>

#include <openthread-types.h>
#include <common/code_utils.hpp>
#include <platform/serial.h>
#include "platform-cc2538.h"

enum
{
    kPlatformClock = 32000000,
    kBaudRate = 115200,
    kReceiveBufferSize = 128,
};

extern void UART0IntHandler(void);

static void processReceive(void);
static void processTransmit(void);

static const uint8_t *sTransmitBuffer = NULL;
static uint16_t sTransmitLength = 0;

static uint8_t sReceiveBuffer[kReceiveBufferSize];
static uint16_t sReceiveHead = 0;
static uint16_t sReceiveLength = 0;

ThreadError otPlatSerialEnable(void)
{
    uint32_t div;

    // clock
    HWREG(SYS_CTRL_RCGCUART) = SYS_CTRL_RCGCUART_UART0;
    HWREG(SYS_CTRL_SCGCUART) = SYS_CTRL_SCGCUART_UART0;
    HWREG(SYS_CTRL_DCGCUART) = SYS_CTRL_DCGCUART_UART0;

    HWREG(UART0_BASE + UART_O_CC) = 0;

    // tx pin
    HWREG(IOC_PA1_SEL) = IOC_MUX_OUT_SEL_UART0_TXD;
    HWREG(IOC_PA1_OVER) = IOC_OVERRIDE_OE;
    HWREG(GPIO_A_BASE + GPIO_O_AFSEL) |= GPIO_PIN_1;

    // rx pin
    HWREG(IOC_PA0_SEL) = IOC_UARTRXD_UART0;
    HWREG(IOC_PA0_OVER) = IOC_OVERRIDE_DIS;
    HWREG(GPIO_A_BASE + GPIO_O_AFSEL) |= GPIO_PIN_0;

    HWREG(UART0_BASE + UART_O_CTL) = 0;

    // baud rate
    div = (((kPlatformClock * 8) / kBaudRate) + 1) / 2;
    HWREG(UART0_BASE + UART_O_IBRD) = div / 64;
    HWREG(UART0_BASE + UART_O_FBRD) = div % 64;
    HWREG(UART0_BASE + UART_O_LCRH) = UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE;

    // configure interrupts
    HWREG(UART0_BASE + UART_O_IM) |= UART_IM_RXIM | UART_IM_RTIM;

    // enable
    HWREG(UART0_BASE + UART_O_CTL) = UART_CTL_UARTEN | UART_CTL_TXE | UART_CTL_RXE;

    // enable interrupts
    HWREG(NVIC_EN0) = 1 << ((INT_UART0 - 16) & 31);

    return kThreadError_None;
}

ThreadError otPlatSerialDisable(void)
{
    return kThreadError_None;
}

ThreadError otPlatSerialSend(const uint8_t *aBuf, uint16_t aBufLength)
{
    ThreadError error = kThreadError_None;

    VerifyOrExit(sTransmitBuffer == NULL, error = kThreadError_Busy);

    sTransmitBuffer = aBuf;
    sTransmitLength = aBufLength;

exit:
    return error;
}

void processReceive(void)
{
    uint16_t remaining;

    VerifyOrExit(sReceiveLength > 0, ;);

    remaining = kReceiveBufferSize - sReceiveHead;

    if (sReceiveLength >= remaining)
    {
        otPlatSerialReceived(sReceiveBuffer + sReceiveHead, remaining);
        sReceiveHead = 0;
        sReceiveLength -= remaining;
    }

    if (sReceiveLength > 0)
    {
        otPlatSerialReceived(sReceiveBuffer + sReceiveHead, sReceiveLength);
        sReceiveHead += sReceiveLength;
        sReceiveLength = 0;
    }

exit:
    return;
}

void processTransmit(void)
{
    VerifyOrExit(sTransmitBuffer != NULL, ;);

    for (; sTransmitLength > 0; sTransmitLength--)
    {
        while (HWREG(UART0_BASE + UART_O_FR) & UART_FR_TXFF);

        HWREG(UART0_BASE + UART_O_DR) = *sTransmitBuffer++;
    }

    sTransmitBuffer = NULL;
    otPlatSerialSendDone();

exit:
    return;
}

void PlatformSerialProcess(void)
{
    processReceive();
    processTransmit();
}

void UART0IntHandler(void)
{
    uint32_t mis;
    uint16_t tail;
    uint8_t byte;

    mis = HWREG(UART0_BASE + UART_O_MIS);
    HWREG(UART0_BASE + UART_O_ICR) = mis;

    if (mis & (UART_IM_RXIM | UART_IM_RTIM))
    {
        while (!(HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE))
        {
            byte = HWREG(UART0_BASE + UART_O_DR);

            if (sReceiveLength < kReceiveBufferSize)
            {
                tail = (sReceiveHead + sReceiveLength) % kReceiveBufferSize;
                sReceiveBuffer[tail] = byte;
                sReceiveLength++;
            }
        }
    }
}
