/* RNDISDevice.h
 * tetherd -- userspace RNDIS USB tethering daemon for macOS.
 *
 * One RNDIS function of a USB device, driven from userspace through IOUSBLib:
 * the control interface (encapsulated RNDIS commands on the default control
 * pipe) and the CDC data interface (bulk IN / bulk OUT).
 *
 * RNDIS control logic follows HoRNDIS (Copyright (c) 2012 Joshua Wise,
 * 2018 Mikhail Iakhiaev), itself after linux/drivers/net/usb/rndis_host.c
 * (Copyright (c) 2005 David Brownell).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include "RNDISProtocol.h"

class RNDISDevice {
public:
    RNDISDevice() = default;
    ~RNDISDevice() { close(); }
    RNDISDevice(const RNDISDevice &) = delete;
    RNDISDevice &operator=(const RNDISDevice &) = delete;

    /// Opens both USB interfaces and locates the bulk pipes.
    bool open(io_service_t controlService, io_service_t dataService, uint8_t controlIfNum);
    void close();

    /// REMOTE_NDIS_INITIALIZE. Must succeed before anything else below.
    bool rndisInit();
    /// Reads the Ethernet address the device assigned to the host side.
    bool queryMacAddress(uint8_t mac[6]);
    /// 0 stops the device sending; RNDIS_DEFAULT_FILTER starts it.
    bool setPacketFilter(uint32_t filter);

    /// Blocking bulk IN read. *length is the buffer size in, bytes read out.
    IOReturn readBulk(void *buffer, uint32_t *length);
    /// Wraps one Ethernet frame in an RNDIS data message and sends it.
    IOReturn writeFrame(const uint8_t *frame, uint32_t length);

    /// Asynchronous bulk I/O, so several transfers can be in flight at once.
    /// Completions are delivered on the run loop given to addAsyncSource,
    /// which must be called first.
    bool addAsyncSource(CFRunLoopRef runLoop);
    IOReturn readAsync(void *buffer, uint32_t length, IOAsyncCallback1 callback, void *refcon);
    /// Writes an RNDIS data message carrying frame into out, which must hold
    /// maxFrameLength() + header + 1 bytes. Returns the number of bytes to
    /// submit (with the short-packet pad byte if needed), or 0 if too large.
    uint32_t buildDataMessage(uint8_t *out, const uint8_t *frame, uint32_t length) const;
    IOReturn writeAsync(void *buffer, uint32_t length, IOAsyncCallback1 callback, void *refcon);

    void clearStallIn();
    void clearStallOut();
    /// Unblocks readBulk / writeFrame callers on other threads.
    void abortIO();

    /// Largest Ethernet frame (header included) one transfer can carry.
    uint32_t maxFrameLength() const { return fMaxOutTransfer - (uint32_t)sizeof(rndis_data_hdr); }

private:
    typedef IOUSBInterfaceInterface300 **InterfaceRef;

    static InterfaceRef openInterface(io_service_t service, const char *what);
    static void closeInterface(InterfaceRef &intf);

    /// Sends the command in fCmd and reads the matching response back into it.
    IOReturn rndisCommand();
    IOReturn rndisQuery(uint32_t oid, uint32_t inLen, const uint8_t **reply, uint32_t *replyLen);

    InterfaceRef fControl = nullptr;
    InterfaceRef fData = nullptr;
    uint8_t  fControlIfNum = 0;
    uint8_t  fPipeIn = 0;
    uint8_t  fPipeOut = 0;
    uint16_t fOutMaxPacketSize = 0;

    uint32_t fXid = 1;
    uint32_t fMaxOutTransfer = 0;

    // uint32_t storage keeps the command buffer aligned for the RNDIS structs.
    uint32_t fCmd[RNDIS_CMD_BUF_SZ / sizeof(uint32_t)] = {};
    // One frame plus header plus the short-packet pad byte. Used only by the
    // single transmit thread.
    uint8_t  fOutBuf[OUT_BUF_SIZE] = {};
};
