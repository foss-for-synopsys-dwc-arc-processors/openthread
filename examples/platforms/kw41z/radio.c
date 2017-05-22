/*
 *  Copyright (c) 2017, The OpenThread Authors.
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
 *   This file implements the OpenThread platform abstraction for radio communication.
 *
 */

#include <stdint.h>
#include <string.h>
#include "fsl_device_registers.h"
#include "openthread-core-kw41z-config.h"
#include "fsl_xcvr.h"
#include "openthread/platform/radio.h"
#include "openthread/platform/diag.h"
#include <utils/code_utils.h>

#define DOUBLE_BUFFERING            (1)
#define DEFAULT_CHANNEL             (11)
#define IEEE802154_ACK_REQUEST      (1 << 5)
#define DEFAULT_CCA_MODE            (XCVR_CCA_MODE1_c)
#define IEEE802154_TURNAROUND_LEN   (12)
#define IEEE802154_CCA_LEN          (8)
#define IEEE802154_PHY_SHR_LEN      (10)
#define IEEE802154_ACK_WAIT         (54)
#define ZLL_IRQSTS_TMR_ALL_MSK_MASK (ZLL_IRQSTS_TMR1MSK_MASK | \
                                     ZLL_IRQSTS_TMR2MSK_MASK | \
                                     ZLL_IRQSTS_TMR3MSK_MASK | \
                                     ZLL_IRQSTS_TMR4MSK_MASK )

typedef enum xcvr_state_tag
{
    XCVR_Idle_c,
    XCVR_RX_c,
    XCVR_TX_c,
    XCVR_CCA_c,
    XCVR_TR_c,
    XCVR_CCCA_c,
} xcvr_state_t;

typedef enum xcvr_cca_type_tag
{
    XCVR_ED_c,            /* energy detect - CCA bit not active, not to be used for T and CCCA sequences */
    XCVR_CCA_MODE1_c,     /* energy detect - CCA bit ACTIVE */
    SCVR_CCA_MODE2_c,     /* 802.15.4 compliant signal detect - CCA bit ACTIVE */
    XCVR_CCA_MODE3_c      /* 802.15.4 compliant signal detect and energy detect - CCA bit ACTIVE */
} xcvr_cca_type_t;

static PhyState     sState = kStateDisabled;
static uint16_t     sPanId;
static uint8_t      sExtSrcAddrBitmap[(RADIO_CONFIG_SRC_MATCH_ENTRY_NUM + 7) / 8];
static uint8_t      sChannel;
static int8_t       sMaxED;
static int8_t       sAutoTxPwrLevel = 0;

/* ISR Signaling Flags */
static bool         sTxDone     = false;
static bool         sRxDone     = false;
static bool         sEdScanDone = false;
static bool         sAckFpState;
static ThreadError  sTxStatus;

static RadioPacket  sTxPacket;
static RadioPacket  sRxPacket;
#if DOUBLE_BUFFERING
static uint8_t      sRxData[kMaxPHYPacketSize];
#endif

/* Private functions */
static void         rf_abort(void);
static xcvr_state_t rf_get_state(void);
static void         rf_set_channel(uint8_t channel);
static void         rf_set_tx_power(int8_t tx_power);
static uint8_t      rf_lqi_adjust(uint8_t hwLqi);
static int8_t       rf_lqi_to_rssi(uint8_t lqi);
static uint32_t     rf_get_timestamp(void);
static void         rf_set_timeout(uint32_t abs_timeout);
static uint16_t     rf_get_addr_checksum(uint8_t *pAddr, bool ExtendedAddr, uint16_t PanId);
static ThreadError  rf_add_addr_table_entry(uint16_t checksum, bool extendedAddr);
static ThreadError  rf_remove_addr_table_entry(uint16_t checksum);
static ThreadError  rf_remove_addr_table_entry_index(uint8_t index);

PhyState otPlatRadioGetState(otInstance *aInstance)
{
    (void)aInstance;
    return sState;
}

void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    (void)aInstance;
    uint32_t addrLo;
    uint32_t addrHi;

    if ((RSIM->MAC_LSB == 0xffffffff) && (RSIM->MAC_MSB == 0xff))
    {
        addrLo = SIM->UIDL;
        addrHi = SIM->UIDML;
    }
    else
    {
        addrLo = RSIM->MAC_LSB;
        addrHi = RSIM->MAC_MSB;
    }

    memcpy(aIeeeEui64, &addrLo, sizeof(addrLo));
    memcpy(aIeeeEui64 + sizeof(addrLo), &addrHi, sizeof(addrHi));
}

void otPlatRadioSetPanId(otInstance *aInstance, uint16_t aPanId)
{
    (void) aInstance;

    sPanId = aPanId;
    ZLL->MACSHORTADDRS0 &= ~ZLL_MACSHORTADDRS0_MACPANID0_MASK;
    ZLL->MACSHORTADDRS0 |= ZLL_MACSHORTADDRS0_MACPANID0(aPanId);
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance, uint8_t *aExtendedAddress)
{
    (void) aInstance;
    uint32_t addrLo;
    uint32_t addrHi;

    memcpy(&addrLo, aExtendedAddress, sizeof(addrLo));
    memcpy(&addrHi, aExtendedAddress + sizeof(addrLo), sizeof(addrHi));

    ZLL->MACLONGADDRS0_LSB = addrLo;
    ZLL->MACLONGADDRS0_MSB = addrHi;
}

void otPlatRadioSetShortAddress(otInstance *aInstance, uint16_t aShortAddress)
{
    (void) aInstance;

    ZLL->MACSHORTADDRS0 &= ~ZLL_MACSHORTADDRS0_MACSHORTADDRS0_MASK;
    ZLL->MACSHORTADDRS0 |= ZLL_MACSHORTADDRS0_MACSHORTADDRS0(aShortAddress);
}

ThreadError otPlatRadioEnable(otInstance *aInstance)
{
    otEXPECT(!otPlatRadioIsEnabled(aInstance));

    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_TRCV_MSK_MASK;
    NVIC_ClearPendingIRQ(Radio_1_IRQn);
    NVIC_EnableIRQ(Radio_1_IRQn);

    sState = kStateSleep;

exit:
    return kThreadError_None;
}

ThreadError otPlatRadioDisable(otInstance *aInstance)
{
    otEXPECT(otPlatRadioIsEnabled(aInstance));

    NVIC_DisableIRQ(Radio_1_IRQn);
    rf_abort();
    sState = kStateDisabled;

exit:
    return kThreadError_None;
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    (void) aInstance;
    return sState != kStateDisabled;
}

ThreadError otPlatRadioSleep(otInstance *aInstance)
{
    ThreadError status = kThreadError_None;
    (void) aInstance;

    otEXPECT_ACTION(((sState != kStateTransmit) && (sState != kStateDisabled)), status = kThreadError_InvalidState);

    rf_abort();
    sState = kStateSleep;

exit:
    return status;
}

ThreadError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    ThreadError status = kThreadError_None;
    (void) aInstance;

    otEXPECT_ACTION(((sState != kStateTransmit) && (sState != kStateDisabled)), status = kThreadError_InvalidState);

    sState = kStateReceive;

    otEXPECT(rf_get_state() != XCVR_RX_c);

    rf_abort();

    /* Set Power level for auto TX */
    rf_set_tx_power(sAutoTxPwrLevel);
    rf_set_channel(aChannel);
    sRxPacket.mChannel = aChannel;

    /* Clear all IRQ flags */
    ZLL->IRQSTS = ZLL->IRQSTS;
    /* Start the RX sequence */
    ZLL->PHY_CTRL |= XCVR_RX_c ;

    /* Unmask SEQ interrupt */
    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_SEQMSK_MASK;

exit:
    return status;
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    (void) aInstance;

    if (aEnable)
    {
        ZLL->SAM_CTRL |= ZLL_SAM_CTRL_SAP0_EN_MASK;
    }
    else
    {
        ZLL->SAM_CTRL &= ~ZLL_SAM_CTRL_SAP0_EN_MASK;
    }
}

ThreadError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, const uint16_t aShortAddress)
{
    (void) aInstance;
    uint16_t checksum = sPanId + aShortAddress;

    return rf_add_addr_table_entry(checksum, false);
}

ThreadError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const uint8_t *aExtAddress)
{
    (void) aInstance;
    uint16_t checksum = rf_get_addr_checksum((uint8_t *)aExtAddress, true, sPanId);

    return rf_add_addr_table_entry(checksum, true);
}

ThreadError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, const uint16_t aShortAddress)
{
    (void) aInstance;
    uint16_t checksum = sPanId + aShortAddress;

    return rf_remove_addr_table_entry(checksum);
}

ThreadError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const uint8_t *aExtAddress)
{
    (void) aInstance;
    uint16_t checksum = rf_get_addr_checksum((uint8_t *)aExtAddress, true, sPanId);

    return rf_remove_addr_table_entry(checksum);
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    (void) aInstance;
    uint32_t i;

    for (i = 0; i < RADIO_CONFIG_SRC_MATCH_ENTRY_NUM; i++)
    {
        /* Optimization: sExtSrcAddrBitmap[i / 8] & (1 << (i % 8)) */
        if (!(sExtSrcAddrBitmap[i >> 3] & (1 << (i & 7))))
        {
            rf_remove_addr_table_entry_index(i);
        }
    }
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    (void) aInstance;
    uint32_t i;

    for (i = 0; i < RADIO_CONFIG_SRC_MATCH_ENTRY_NUM; i++)
    {
        /* Optimization: sExtSrcAddrBitmap[i / 8] & (1 << (i % 8))*/
        if (sExtSrcAddrBitmap[i >> 3] & (1 << (i & 7)))
        {
            rf_remove_addr_table_entry_index(i);
        }
    }
}

RadioPacket *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    (void)aInstance;
    return &sTxPacket;
}

ThreadError otPlatRadioTransmit(otInstance *aInstance, RadioPacket *aPacket)
{
    ThreadError status = kThreadError_None;
    uint32_t timeout;

    (void) aInstance;

    otEXPECT_ACTION(((sState != kStateTransmit) && (sState != kStateDisabled)), status = kThreadError_InvalidState);

    if (rf_get_state() != XCVR_Idle_c)
    {
        rf_abort();
    }

    rf_set_channel(aPacket->mChannel);
    rf_set_tx_power(aPacket->mPower);

    *(uint8_t *)ZLL->PKT_BUFFER_TX = aPacket->mLength;

    /* Set CCA mode */
    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_CCATYPE_MASK;
    ZLL->PHY_CTRL |= ZLL_PHY_CTRL_CCATYPE(DEFAULT_CCA_MODE);

    /* Clear all IRQ flags */
    ZLL->IRQSTS = ZLL->IRQSTS;

    /* Perform automatic reception of ACK frame, if required */
    if (aPacket->mPsdu[0] & IEEE802154_ACK_REQUEST)
    {
        ZLL->PHY_CTRL |= ZLL_PHY_CTRL_RXACKRQD_MASK;
        ZLL->PHY_CTRL |= XCVR_TR_c;
        /* Set ACK wait time-out */
        timeout  = rf_get_timestamp();
        timeout += (((XCVR_TSM->END_OF_SEQ & XCVR_TSM_END_OF_SEQ_END_OF_TX_WU_MASK) >>
                     XCVR_TSM_END_OF_SEQ_END_OF_TX_WU_SHIFT) >> 4);
        timeout += IEEE802154_CCA_LEN + IEEE802154_TURNAROUND_LEN + IEEE802154_PHY_SHR_LEN +
                   (1 + aPacket->mLength) * kPhySymbolsPerOctet + IEEE802154_ACK_WAIT;
        rf_set_timeout(timeout);
    }
    else
    {
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_RXACKRQD_MASK;
        ZLL->PHY_CTRL |= XCVR_TX_c;
    }

    sAckFpState = false;
    sState = kStateTransmit;
    /* Unmask SEQ interrupt */
    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_SEQMSK_MASK;

exit:
    return status;
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    (void) aInstance;
    return (ZLL->LQI_AND_RSSI & ZLL_LQI_AND_RSSI_RSSI_MASK) >> ZLL_LQI_AND_RSSI_RSSI_SHIFT;
}

otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    (void)aInstance;
    return kRadioCapsAckTimeout | kRadioCapsEnergyScan;
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    (void) aInstance;
    return (ZLL->PHY_CTRL & ZLL_PHY_CTRL_PROMISCUOUS_MASK) == ZLL_PHY_CTRL_PROMISCUOUS_MASK;
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    (void) aInstance;

    if (aEnable)
    {
        ZLL->PHY_CTRL |= ZLL_PHY_CTRL_PROMISCUOUS_MASK;
        /* FRM_VER[11:8] = b1111. Any FrameVersion accepted */
        ZLL->RX_FRAME_FILTER |= (ZLL_RX_FRAME_FILTER_FRM_VER_FILTER_MASK |
                                 ZLL_RX_FRAME_FILTER_ACK_FT_MASK         |
                                 ZLL_RX_FRAME_FILTER_NS_FT_MASK);
    }
    else
    {
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_PROMISCUOUS_MASK;
        /* FRM_VER[11:8] = b0011. Accept FrameVersion 0 and 1 packets, reject all others */
        /* Beacon, Data and MAC command frame types accepted */
        ZLL->RX_FRAME_FILTER &= ~(ZLL_RX_FRAME_FILTER_FRM_VER_FILTER_MASK    |
                                  ZLL_RX_FRAME_FILTER_ACK_FT_MASK            |
                                  ZLL_RX_FRAME_FILTER_NS_FT_MASK             |
                                  ZLL_RX_FRAME_FILTER_ACTIVE_PROMISCUOUS_MASK);
        ZLL->RX_FRAME_FILTER |= ZLL_RX_FRAME_FILTER_FRM_VER_FILTER(3);
    }
}

ThreadError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    ThreadError status = kThreadError_None;
    uint32_t timeout;
    (void) aInstance;

    otEXPECT_ACTION(((sState != kStateTransmit) && (sState != kStateDisabled)), status = kThreadError_InvalidState);

    if (rf_get_state() != XCVR_Idle_c)
    {
        rf_abort();
    }

    sMaxED = -128;
    rf_set_channel(aScanChannel);
    /* Set CCA type to ED - Energy Detect */
    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_CCATYPE_MASK;
    ZLL->PHY_CTRL |= ZLL_PHY_CTRL_CCATYPE(XCVR_ED_c);
    /* Clear all IRQ flags */
    ZLL->IRQSTS = ZLL->IRQSTS;
    /* Start ED sequence */
    ZLL->PHY_CTRL |= XCVR_CCA_c;
    /* Unmask SEQ interrupt */
    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_SEQMSK_MASK;
    /* Set Scan time-out */
    timeout  = rf_get_timestamp();
    timeout += (aScanDuration * 1000) / kPhyUsPerSymbol;
    rf_set_timeout(timeout);

exit:
    return status;
}

void otPlatRadioSetDefaultTxPower(otInstance *aInstance, int8_t aPower)
{
    (void)aInstance;

    sAutoTxPwrLevel = aPower;
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    (void)aInstance;

    return -100;
}

/*************************************************************************************************/

static void rf_abort(void)
{
    /* Mask SEQ interrupt */
    ZLL->PHY_CTRL |= ZLL_PHY_CTRL_SEQMSK_MASK;

    /* Disable timer trigger (for scheduled XCVSEQ) */
    if (ZLL->PHY_CTRL & ZLL_PHY_CTRL_TMRTRIGEN_MASK)
    {
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_TMRTRIGEN_MASK;

        /* give the FSM enough time to start if it was triggered */
        while ((XCVR_MISC->XCVR_CTRL & XCVR_CTRL_XCVR_STATUS_TSM_COUNT_MASK) == 0)
        {
        }
    }

    /* If XCVR is not idle, abort current SEQ */
    if (ZLL->PHY_CTRL & ZLL_PHY_CTRL_XCVSEQ_MASK)
    {
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_XCVSEQ_MASK;

        /* wait for Sequence Idle (if not already) */
        while (ZLL->SEQ_STATE & ZLL_SEQ_STATE_SEQ_STATE_MASK)
        {
        }
    }

    /* Stop timers */
    ZLL->PHY_CTRL &= ~(ZLL_PHY_CTRL_TMR2CMP_EN_MASK | ZLL_PHY_CTRL_TMR3CMP_EN_MASK);
    /* Clear all PP IRQ bits to avoid unexpected interrupts( do not change TMR1 and TMR4 IRQ status ) */
    ZLL->IRQSTS &= ~(ZLL_IRQSTS_TMR1IRQ_MASK | ZLL_IRQSTS_TMR4IRQ_MASK);
}

static xcvr_state_t rf_get_state(void)
{
    return (xcvr_state_t)((ZLL->PHY_CTRL & ZLL_PHY_CTRL_XCVSEQ_MASK) >> ZLL_PHY_CTRL_XCVSEQ_SHIFT);
}

static void rf_set_channel(uint8_t channel)
{
    if (sChannel != channel)
    {
        ZLL->CHANNEL_NUM0 = channel;
        sChannel = channel;
    }
}

static void rf_set_tx_power(int8_t tx_power)
{
    if (tx_power > 2)
    {
        ZLL->PA_PWR = 30;
    }
    else if (tx_power > 1)
    {
        ZLL->PA_PWR = 24;
    }
    else if (tx_power > -1)
    {
        ZLL->PA_PWR = 18;
    }
    else if (tx_power > -3)
    {
        ZLL->PA_PWR = 14;
    }
    else if (tx_power > -4)
    {
        ZLL->PA_PWR = 12;
    }
    else if (tx_power > -6)
    {
        ZLL->PA_PWR = 10;
    }
    else if (tx_power > -8)
    {
        ZLL->PA_PWR = 8;
    }
    else if (tx_power > -11)
    {
        ZLL->PA_PWR = 6;
    }
    else if (tx_power > -14)
    {
        ZLL->PA_PWR = 4;
    }
    else if (tx_power > -20)
    {
        ZLL->PA_PWR = 2;
    }
    else
    {
        ZLL->PA_PWR = 0;
    }
}

static uint16_t rf_get_addr_checksum(uint8_t *pAddr, bool ExtendedAddr, uint16_t PanId)
{
    uint16_t checksum;

    /* Short address */
    checksum  = PanId;
    checksum += *pAddr++;
    checksum += (uint16_t)(*pAddr++) << 8;

    if (ExtendedAddr)
    {
        /* Extended address */
        checksum += *pAddr++;
        checksum += ((uint16_t)(*pAddr++)) << 8;
        checksum += *pAddr++;
        checksum += ((uint16_t)(*pAddr++)) << 8;
        checksum += *pAddr++;
        checksum += ((uint16_t)(*pAddr++)) << 8;
    }

    return checksum;
}

static ThreadError rf_add_addr_table_entry(uint16_t checksum, bool extendedAddr)
{
    uint8_t index;
    ThreadError status;

    /* Find first free index */
    ZLL->SAM_TABLE = ZLL_SAM_TABLE_FIND_FREE_IDX_MASK;

    while (ZLL->SAM_TABLE & ZLL_SAM_TABLE_SAM_BUSY_MASK)
    {
    }

    index = (ZLL->SAM_FREE_IDX & ZLL_SAM_FREE_IDX_SAP0_1ST_FREE_IDX_MASK) >> ZLL_SAM_FREE_IDX_SAP0_1ST_FREE_IDX_SHIFT;

    otEXPECT_ACTION((index < RADIO_CONFIG_SRC_MATCH_ENTRY_NUM), status = kThreadError_NoBufs);

    /* Insert the checksum at the index found */
    ZLL->SAM_TABLE = ((uint32_t)index << ZLL_SAM_TABLE_SAM_INDEX_SHIFT)       |
                     ((uint32_t)checksum << ZLL_SAM_TABLE_SAM_CHECKSUM_SHIFT) |
                     ZLL_SAM_TABLE_SAM_INDEX_WR_MASK | ZLL_SAM_TABLE_SAM_INDEX_EN_MASK;

    if (extendedAddr)
    {
        /* Optimization: sExtSrcAddrBitmap[index / 8] |= 1 << (index % 8); */
        sExtSrcAddrBitmap[index >> 3] |= 1 << (index & 7);
    }

    status = kThreadError_None;

exit:
    return status;
}

static ThreadError rf_remove_addr_table_entry(uint16_t checksum)
{
    ThreadError status = kThreadError_NoAddress;
    uint32_t i, temp;

    /* Search for an entry to match the provided checksum */
    for (i = 0; i < RADIO_CONFIG_SRC_MATCH_ENTRY_NUM; i++)
    {
        ZLL->SAM_TABLE = i << ZLL_SAM_TABLE_SAM_INDEX_SHIFT;
        /* Read checksum located at the specified index */
        temp = (ZLL->SAM_TABLE & ZLL_SAM_TABLE_SAM_CHECKSUM_MASK) >> ZLL_SAM_TABLE_SAM_CHECKSUM_SHIFT;

        if (temp == checksum)
        {
            /* Remove the entry from the table */
            status = rf_remove_addr_table_entry_index(i);
            break;
        }
    }

    return status;
}

static ThreadError rf_remove_addr_table_entry_index(uint8_t index)
{
    ThreadError status = kThreadError_None;

    otEXPECT_ACTION(index < RADIO_CONFIG_SRC_MATCH_ENTRY_NUM, status = kThreadError_NoAddress);

    ZLL->SAM_TABLE = ((uint32_t)0xFFFF << ZLL_SAM_TABLE_SAM_CHECKSUM_SHIFT) |
                     ((uint32_t)index << ZLL_SAM_TABLE_SAM_INDEX_SHIFT) |
                     ZLL_SAM_TABLE_SAM_INDEX_INV_MASK | ZLL_SAM_TABLE_SAM_INDEX_WR_MASK;

    /* Clear bitmap */
    /* Optimization: sExtSrcAddrBitmap[index / 8] &= ~(1 << (index % 8)); */
    sExtSrcAddrBitmap[index >> 3] &= ~(1 << (index & 7));

exit:
    return status;
}

static uint8_t rf_lqi_adjust(uint8_t hwLqi)
{
    if (hwLqi >= 220)
    {
        hwLqi = 255;
    }
    else
    {
        hwLqi = (51 * hwLqi) / 44;
    }

    return hwLqi;
}

static int8_t rf_lqi_to_rssi(uint8_t lqi)
{
    int32_t rssi = (36 * lqi - 9836) / 109;

    return (int8_t)rssi;
}

static uint32_t rf_get_timestamp(void)
{
    return ZLL->EVENT_TMR >> ZLL_EVENT_TMR_EVENT_TMR_SHIFT;
}

static void rf_set_timeout(uint32_t abs_timeout)
{
    uint32_t irqSts;

    /* Disable TMR3 compare */
    ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_TMR3CMP_EN_MASK;
    /* Set time-out value */
    ZLL->T3CMP = abs_timeout;
    /* Aknowledge and unmask TMR3 IRQ */
    irqSts  = ZLL->IRQSTS & ZLL_IRQSTS_TMR_ALL_MSK_MASK;
    irqSts &= ~ZLL_IRQSTS_TMR3MSK_MASK;
    irqSts |= ZLL_IRQSTS_TMR3IRQ_MASK;
    ZLL->IRQSTS = irqSts;
    /* Enable TMR3 compare */
    ZLL->PHY_CTRL |= ZLL_PHY_CTRL_TMR3CMP_EN_MASK;
}

void Radio_1_IRQHandler(void)
{
    xcvr_state_t state = rf_get_state();
    uint32_t irqStatus = ZLL->IRQSTS;
    uint8_t temp;

    ZLL->IRQSTS = irqStatus;

    /* TMR3 IRQ - time-out */
    if ((irqStatus & ZLL_IRQSTS_TMR3IRQ_MASK) && (!(irqStatus & ZLL_IRQSTS_TMR3MSK_MASK)))
    {
        /* Stop TMR3 */
        ZLL->IRQSTS = irqStatus | ZLL_IRQSTS_TMR3MSK_MASK;
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_TMR3CMP_EN_MASK;

        if (state == XCVR_CCA_c)
        {
            rf_abort();
            sEdScanDone = true;
        }
        else if ((state == XCVR_TR_c) && !(irqStatus & ZLL_IRQSTS_RXIRQ_MASK))
        {
            rf_abort();
            sState = kStateReceive;
            sTxStatus = kThreadError_NoAck;
            sTxDone = true;
        }
    }

    /* Sequence done IRQ */
    if ((!(ZLL->PHY_CTRL & ZLL_PHY_CTRL_SEQMSK_MASK)) && (irqStatus & ZLL_IRQSTS_SEQIRQ_MASK))
    {
        /* Cleanup */
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_XCVSEQ_MASK;
        ZLL->PHY_CTRL |= ZLL_PHY_CTRL_SEQMSK_MASK;

        switch (state)
        {
        case XCVR_RX_c:
            temp = (ZLL->LQI_AND_RSSI & ZLL_LQI_AND_RSSI_LQI_VALUE_MASK) >> ZLL_LQI_AND_RSSI_LQI_VALUE_SHIFT;
            sRxPacket.mLength = (ZLL->IRQSTS & ZLL_IRQSTS_RX_FRAME_LENGTH_MASK) >> ZLL_IRQSTS_RX_FRAME_LENGTH_SHIFT;
            sRxPacket.mLqi = rf_lqi_adjust(temp);
            sRxPacket.mPower = rf_lqi_to_rssi(sRxPacket.mLqi);
#if DOUBLE_BUFFERING
            memcpy(sRxData, (void *)ZLL->PKT_BUFFER_RX, sRxPacket.mLength);
#endif
            sRxDone = true;
            break;

        case XCVR_TX_c:
        case XCVR_TR_c:
            sState = kStateReceive;

            if ((ZLL->PHY_CTRL & ZLL_PHY_CTRL_CCABFRTX_MASK) && (irqStatus & ZLL_IRQSTS_CCA_MASK))
            {
                sTxStatus = kThreadError_ChannelAccessFailure;
            }
            else
            {
                sAckFpState = (irqStatus & ZLL_IRQSTS_RX_FRM_PEND_MASK) > 0;
                sTxStatus = kThreadError_None;
            }

            sTxDone = true;
            break;

        case XCVR_CCA_c:
            temp = (ZLL->LQI_AND_RSSI & ZLL_LQI_AND_RSSI_CCA1_ED_FNL_MASK) >> ZLL_LQI_AND_RSSI_CCA1_ED_FNL_SHIFT;

            if ((int8_t)temp > sMaxED)
            {
                sMaxED = (int8_t)temp;
            }

            if (!sEdScanDone)
            {
                /* Restart ED */
                while (ZLL->SEQ_STATE & ZLL_SEQ_STATE_SEQ_STATE_MASK) {}

                ZLL->IRQSTS = (ZLL->IRQSTS & ZLL_IRQSTS_TMR_ALL_MSK_MASK) | ZLL_IRQSTS_SEQIRQ_MASK;
                ZLL->PHY_CTRL |= XCVR_CCA_c;
                ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_SEQMSK_MASK;
            }

            break;

        default:
            rf_abort();
            break;
        }
    }

    if ((sState == kStateReceive) && (rf_get_state() == XCVR_Idle_c))
    {
        /* Restart RX */
        while (ZLL->SEQ_STATE & ZLL_SEQ_STATE_SEQ_STATE_MASK) {}

        ZLL->IRQSTS = ZLL->IRQSTS;
        ZLL->PHY_CTRL |= XCVR_RX_c;
        ZLL->PHY_CTRL &= ~ZLL_PHY_CTRL_SEQMSK_MASK;
    }
}

void kw41zRadioInit(void)
{
    XCVR_Init(ZIGBEE_MODE, DR_500KBPS);

    /* Disable all timers, enable AUTOACK and CCA before TX, mask all interrupts */
    ZLL->PHY_CTRL = ZLL_PHY_CTRL_CCATYPE(DEFAULT_CCA_MODE) |
                    ZLL_PHY_CTRL_CCABFRTX_MASK             |
                    ZLL_PHY_CTRL_TSM_MSK_MASK              |
                    ZLL_PHY_CTRL_WAKE_MSK_MASK             |
                    ZLL_PHY_CTRL_CRC_MSK_MASK              |
                    ZLL_PHY_CTRL_PLL_UNLOCK_MSK_MASK       |
                    ZLL_PHY_CTRL_FILTERFAIL_MSK_MASK       |
                    ZLL_PHY_CTRL_RX_WMRK_MSK_MASK          |
                    ZLL_PHY_CTRL_CCAMSK_MASK               |
                    ZLL_PHY_CTRL_RXMSK_MASK                |
                    ZLL_PHY_CTRL_TXMSK_MASK                |
                    ZLL_PHY_CTRL_SEQMSK_MASK               |
                    ZLL_PHY_CTRL_AUTOACK_MASK              |
                    ZLL_PHY_CTRL_TRCV_MSK_MASK;

    /* Clear all IRQ flags and disable all timer interrupts */
    ZLL->IRQSTS = ZLL->IRQSTS;

    /*  Frame Filtering
    FRM_VER[7:6] = b11. Accept FrameVersion 0 and 1 packets, reject all others */
    ZLL->RX_FRAME_FILTER &= ~ZLL_RX_FRAME_FILTER_FRM_VER_FILTER_MASK;
    ZLL->RX_FRAME_FILTER = ZLL_RX_FRAME_FILTER_FRM_VER_FILTER(3) |
                           ZLL_RX_FRAME_FILTER_CMD_FT_MASK       |
                           ZLL_RX_FRAME_FILTER_DATA_FT_MASK      |
                           ZLL_RX_FRAME_FILTER_BEACON_FT_MASK;

    /* Set prescaller to obtain 1 symbol (16us) timebase */
    ZLL->TMR_PRESCALE = 0x05;

    /* Set CCA threshold to -75 dBm */
    ZLL->CCA_LQI_CTRL &= ~ZLL_CCA_LQI_CTRL_CCA1_THRESH_MASK;
    ZLL->CCA_LQI_CTRL |= ZLL_CCA_LQI_CTRL_CCA1_THRESH(-75);

    /* Adjust LQI compensation */
    ZLL->CCA_LQI_CTRL &= ~ZLL_CCA_LQI_CTRL_LQI_OFFSET_COMP_MASK;
    ZLL->CCA_LQI_CTRL |= ZLL_CCA_LQI_CTRL_LQI_OFFSET_COMP(96);

    /* Adjust ACK delay to fulfill the 802.15.4 turnaround requirements */
    ZLL->ACKDELAY &= ~ZLL_ACKDELAY_ACKDELAY_MASK;
    ZLL->ACKDELAY |= ZLL_ACKDELAY_ACKDELAY(-8);

    /* Clear HW indirect queue */
    ZLL->SAM_TABLE |= ZLL_SAM_TABLE_INVALIDATE_ALL_MASK;

    rf_set_channel(DEFAULT_CHANNEL);
    rf_set_tx_power(0);

    sTxPacket.mLength = 0;
    sTxPacket.mPsdu = (uint8_t *)ZLL->PKT_BUFFER_TX + 1;
    sRxPacket.mLength = 0;
#if DOUBLE_BUFFERING
    sRxPacket.mPsdu = sRxData;
#else
    sRxPacket.mPsdu = (uint8_t *)ZLL->PKT_BUFFER_RX;
#endif
}

void kw41zRadioProcess(otInstance *aInstance)
{
    if (sTxDone)
    {
        otPlatRadioTransmitDone(aInstance, &sTxPacket, sAckFpState, sTxStatus);
        sTxDone = false;
    }

    if (sRxDone)
    {
#if OPENTHREAD_ENABLE_DIAG

        if (otPlatDiagModeGet())
        {
            otPlatDiagRadioReceiveDone(aInstance, &sRxPacket, kThreadError_None);
        }
        else
#endif
        {
            otPlatRadioReceiveDone(aInstance, &sRxPacket, kThreadError_None);
        }

        sRxDone = false;
    }

    if (sEdScanDone)
    {
        otPlatRadioEnergyScanDone(aInstance, sMaxED);
        sEdScanDone = false;
    }
}
