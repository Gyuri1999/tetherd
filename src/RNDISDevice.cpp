/* RNDISDevice.cpp
 * tetherd -- userspace RNDIS USB tethering daemon for macOS.
 *
 * RNDIS control logic follows HoRNDIS (Copyright (c) 2012 Joshua Wise,
 * 2018 Mikhail Iakhiaev), itself after linux/drivers/net/usb/rndis_host.c
 * (Copyright (c) 2005 David Brownell).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "RNDISDevice.h"

#include <string.h>
#include <unistd.h>

#include "Log.h"
#include "RNDISProtocolCore.h"

namespace {

const uint32_t kControlTimeoutMs = 5000;
const int      kResponseAttempts = 10;
const useconds_t kResponseRetryDelayUs = 20 * 1000;

} // namespace

RNDISDevice::InterfaceRef RNDISDevice::openInterface(io_service_t service, const char *what)
{
    IOCFPlugInInterface **plugin = nullptr;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != kIOReturnSuccess || plugin == nullptr) {
        LOG_ERROR("cannot create a user client for the %s interface: 0x%08x", what, kr);
        return nullptr;
    }

    InterfaceRef intf = nullptr;
    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&intf);
    (*plugin)->Release(plugin);
    if (hr != S_OK || intf == nullptr) {
        LOG_ERROR("cannot get the USB interface object for the %s interface", what);
        return nullptr;
    }

    kr = (*intf)->USBInterfaceOpen(intf);
    if (kr != kIOReturnSuccess) {
        if (kr == kIOReturnExclusiveAccess) {
            LOG_ERROR("the %s interface is already in use by another driver "
                      "(is TetherKit.kext, HoRNDIS or another tethering driver loaded?)", what);
        } else {
            LOG_ERROR("cannot open the %s interface: 0x%08x", what, kr);
        }
        (*intf)->Release(intf);
        return nullptr;
    }
    return intf;
}

void RNDISDevice::closeInterface(InterfaceRef &intf)
{
    if (intf != nullptr) {
        (*intf)->USBInterfaceClose(intf);
        (*intf)->Release(intf);
        intf = nullptr;
    }
}

bool RNDISDevice::open(io_service_t controlService, io_service_t dataService, uint8_t controlIfNum)
{
    close();
    fControlIfNum = controlIfNum;

    fControl = openInterface(controlService, "RNDIS control");
    if (fControl == nullptr) {
        return false;
    }
    fData = openInterface(dataService, "RNDIS data");
    if (fData == nullptr) {
        close();
        return false;
    }

    UInt8 numEndpoints = 0;
    if ((*fData)->GetNumEndpoints(fData, &numEndpoints) != kIOReturnSuccess) {
        LOG_ERROR("cannot read the data interface's endpoints");
        close();
        return false;
    }
    // Pipe reference 0 is the default control pipe; endpoints start at 1.
    for (UInt8 ref = 1; ref <= numEndpoints; ref++) {
        UInt8 direction = 0, number = 0, transferType = 0, interval = 0;
        UInt16 maxPacketSize = 0;
        if ((*fData)->GetPipeProperties(fData, ref, &direction, &number, &transferType,
                                        &maxPacketSize, &interval) != kIOReturnSuccess) {
            continue;
        }
        if (transferType != kUSBBulk) {
            continue;
        }
        if (direction == kUSBIn && fPipeIn == 0) {
            fPipeIn = ref;
        } else if (direction == kUSBOut && fPipeOut == 0) {
            fPipeOut = ref;
            fOutMaxPacketSize = maxPacketSize;
        }
    }
    if (fPipeIn == 0 || fPipeOut == 0) {
        LOG_ERROR("the data interface has no bulk IN/OUT endpoint pair");
        close();
        return false;
    }
    LOG_DEBUG("data pipes: in=%u out=%u (out max packet %u)", fPipeIn, fPipeOut, fOutMaxPacketSize);
    return true;
}

void RNDISDevice::close()
{
    closeInterface(fData);
    closeInterface(fControl);
    fPipeIn = fPipeOut = 0;
    fOutMaxPacketSize = 0;
    fMaxOutTransfer = 0;
}

IOReturn RNDISDevice::rndisCommand()
{
    if (fControl == nullptr) {
        return kIOReturnNotOpen;
    }
    rndis_msg_hdr *hdr = reinterpret_cast<rndis_msg_hdr *>(fCmd);

    if (hdr->msg_type != RNDIS_MSG_HALT && hdr->msg_type != RNDIS_MSG_RESET) {
        uint32_t xid = fXid++;
        if (xid == 0) {
            xid = fXid++;
        }
        hdr->request_id = cpu_to_le32(xid);
    }
    const uint32_t requestType = hdr->msg_type;
    const uint32_t requestId = hdr->request_id;
    const uint32_t requestLen = le32_to_cpu(hdr->msg_len);
    if (requestLen > RNDIS_CMD_BUF_SZ) {
        return kIOReturnBadArgument;
    }

    IOUSBDevRequestTO request = {};
    request.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface);
    request.bRequest = USB_CDC_SEND_ENCAPSULATED_COMMAND;
    request.wValue = 0;
    request.wIndex = fControlIfNum;
    request.wLength = (UInt16)requestLen;
    request.pData = fCmd;
    request.noDataTimeout = kControlTimeoutMs;
    request.completionTimeout = kControlTimeoutMs;

    IOReturn rc = (*fControl)->ControlRequestTO(fControl, 0, &request);
    if (rc != kIOReturnSuccess) {
        LOG_DEBUG("SEND_ENCAPSULATED_COMMAND failed: 0x%08x", rc);
        return rc;
    }
    if (request.wLenDone != requestLen) {
        LOG_DEBUG("SEND_ENCAPSULATED_COMMAND was cut short");
        return kIOReturnUnderrun;
    }

    // The device answers on the same control pipe once it is ready; until
    // then it returns a short or unrelated message. Per the RNDIS-over-USB
    // mapping the host should wait for a notification on the interrupt
    // endpoint first; like HoRNDIS, this polls instead, which is what
    // Android and Linux gadget devices are happy with.
    for (int attempt = 0; attempt < kResponseAttempts; attempt++) {
        request = {};
        request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface);
        request.bRequest = USB_CDC_GET_ENCAPSULATED_RESPONSE;
        request.wValue = 0;
        request.wIndex = fControlIfNum;
        request.wLength = (UInt16)RNDIS_CMD_BUF_SZ;
        request.pData = fCmd;
        request.noDataTimeout = kControlTimeoutMs;
        request.completionTimeout = kControlTimeoutMs;

        rc = (*fControl)->ControlRequestTO(fControl, 0, &request);
        if (rc != kIOReturnSuccess) {
            return rc;
        }
        const uint32_t received = request.wLenDone;
        if (received < sizeof(rndis_msg_hdr) - sizeof(uint32_t)) {
            usleep(kResponseRetryDelayUs);
            continue;
        }
        if (hdr->msg_type != (requestType | RNDIS_MSG_COMPLETION)) {
            LOG_DEBUG("skipping RNDIS message type 0x%08x while waiting for a response",
                      le32_to_cpu(hdr->msg_type));
            usleep(kResponseRetryDelayUs);
            continue;
        }
        if (hdr->request_id != requestId) {
            LOG_DEBUG("RNDIS response carries the wrong request id");
            usleep(kResponseRetryDelayUs);
            continue;
        }
        if (received < sizeof(rndis_msg_hdr)) {
            return kIOReturnUnderrun;
        }
        if (hdr->status != RNDIS_STATUS_SUCCESS) {
            LOG_ERROR("RNDIS command 0x%08x returned status 0x%08x",
                      le32_to_cpu(requestType), le32_to_cpu(hdr->status));
            return kIOReturnError;
        }
        if (le32_to_cpu(hdr->msg_len) != received) {
            LOG_ERROR("RNDIS response length mismatch: header says %u, received %u",
                      le32_to_cpu(hdr->msg_len), received);
            return kIOReturnError;
        }
        return kIOReturnSuccess;
    }
    LOG_ERROR("RNDIS command 0x%08x timed out", le32_to_cpu(requestType));
    return kIOReturnTimeout;
}

bool RNDISDevice::rndisInit()
{
    rndis_init *init = reinterpret_cast<rndis_init *>(fCmd);
    memset(fCmd, 0, sizeof(*init));
    init->msg_type = RNDIS_MSG_INIT;
    init->msg_len = cpu_to_le32((uint32_t)sizeof(*init));
    init->major_version = cpu_to_le32(1);
    init->minor_version = cpu_to_le32(0);
    // The largest USB transfer the device may send to us.
    init->max_transfer_size = cpu_to_le32(IN_BUF_SIZE);

    if (rndisCommand() != kIOReturnSuccess) {
        LOG_ERROR("RNDIS initialisation failed");
        return false;
    }
    const rndis_init_c *reply = reinterpret_cast<const rndis_init_c *>(fCmd);
    if (le32_to_cpu(reply->msg_len) < sizeof(*reply)) {
        LOG_ERROR("RNDIS initialisation reply is too short");
        return false;
    }
    const uint32_t deviceMax = le32_to_cpu(reply->max_transfer_size);
    LOG_INFO("RNDIS %u.%u: max_packets_per_transfer=%u max_transfer_size=%u",
             le32_to_cpu(reply->major_version), le32_to_cpu(reply->minor_version),
             le32_to_cpu(reply->max_packets_per_transfer), deviceMax);

    // One frame per transfer, so cap at our output buffer; and never go
    // below one full-size Ethernet frame even if the device reports nonsense.
    fMaxOutTransfer = rndis::clampMaxTransferSize(deviceMax, OUT_BUF_PAYLOAD_MAX);
    return true;
}

IOReturn RNDISDevice::rndisQuery(uint32_t oid, uint32_t inLen,
                                 const uint8_t **reply, uint32_t *replyLen)
{
    rndis_query *query = reinterpret_cast<rndis_query *>(fCmd);
    if (sizeof(*query) + inLen > RNDIS_CMD_BUF_SZ) {
        return kIOReturnBadArgument;
    }
    memset(fCmd, 0, sizeof(*query) + inLen);
    query->msg_type = RNDIS_MSG_QUERY;
    query->msg_len = cpu_to_le32((uint32_t)sizeof(*query) + inLen);
    query->oid = oid;
    query->len = cpu_to_le32(inLen);
    query->offset = cpu_to_le32(20);

    IOReturn rc = rndisCommand();
    if (rc != kIOReturnSuccess) {
        return rc;
    }
    const rndis_query_c *response = reinterpret_cast<const rndis_query_c *>(fCmd);
    const uint32_t msgLen = le32_to_cpu(response->msg_len);
    const uint64_t offset = le32_to_cpu(response->offset);
    const uint64_t length = le32_to_cpu(response->len);
    // The offset counts from the request_id field, 8 bytes into the message.
    if (msgLen < sizeof(*response) || 8 + offset + length > msgLen) {
        LOG_ERROR("malformed RNDIS query response");
        return kIOReturnError;
    }
    *reply = reinterpret_cast<const uint8_t *>(fCmd) + 8 + offset;
    *replyLen = (uint32_t)length;
    return kIOReturnSuccess;
}

bool RNDISDevice::queryMacAddress(uint8_t mac[6])
{
    const uint8_t *reply = nullptr;
    uint32_t replyLen = 0;

    // The 48-byte request payload is what Linux and HoRNDIS send.
    IOReturn rc = rndisQuery(OID_802_3_PERMANENT_ADDRESS, 48, &reply, &replyLen);
    if (rc != kIOReturnSuccess || replyLen < 6) {
        LOG_DEBUG("permanent address query failed, trying the current address");
        rc = rndisQuery(OID_802_3_CURRENT_ADDRESS, 48, &reply, &replyLen);
    }
    if (rc != kIOReturnSuccess || replyLen < 6) {
        LOG_ERROR("cannot read the device's Ethernet address");
        return false;
    }
    memcpy(mac, reply, 6);
    if (rndis::fixupMacMulticastBit(mac)) {
        LOG_INFO("device reported a multicast Ethernet address; adjusted it");
    }
    return true;
}

bool RNDISDevice::setPacketFilter(uint32_t filter)
{
    rndis_set *set = reinterpret_cast<rndis_set *>(fCmd);
    memset(fCmd, 0, sizeof(*set) + sizeof(uint32_t));
    set->msg_type = RNDIS_MSG_SET;
    set->msg_len = cpu_to_le32((uint32_t)(sizeof(*set) + sizeof(uint32_t)));
    set->oid = OID_GEN_CURRENT_PACKET_FILTER;
    set->len = cpu_to_le32(4);
    set->offset = cpu_to_le32((uint32_t)sizeof(*set) - 8);
    memcpy(set + 1, &filter, sizeof(filter));

    return rndisCommand() == kIOReturnSuccess;
}

IOReturn RNDISDevice::readBulk(void *buffer, uint32_t *length)
{
    if (fData == nullptr) {
        return kIOReturnNotOpen;
    }
    UInt32 size = *length;
    IOReturn rc = (*fData)->ReadPipe(fData, fPipeIn, buffer, &size);
    *length = (rc == kIOReturnSuccess) ? size : 0;
    return rc;
}

IOReturn RNDISDevice::writeFrame(const uint8_t *frame, uint32_t length)
{
    if (fData == nullptr || fMaxOutTransfer == 0) {
        return kIOReturnNotOpen;
    }
    const uint32_t total = length + (uint32_t)sizeof(rndis_data_hdr);
    if (length == 0 || total > fMaxOutTransfer) {
        return kIOReturnMessageTooLarge;
    }

    rndis_data_hdr header = {};
    header.msg_type = RNDIS_MSG_PACKET;
    header.msg_len = cpu_to_le32(total);
    header.data_offset = RNDIS_DATA_HDR_DATA_OFFSET;
    header.data_len = cpu_to_le32(length);
    memcpy(fOutBuf, &header, sizeof(header));
    memcpy(fOutBuf + sizeof(header), frame, length);

    // A bulk transfer whose length is an exact multiple of the endpoint's
    // packet size is not terminated by a short packet, so the device keeps
    // waiting for more. Append one zero byte (what Linux usbnet does for
    // rndis_host); the device goes by msg_len and ignores it. fOutBuf is
    // OUT_BUF_PAYLOAD_MAX + 1 bytes to leave room.
    uint32_t submit = total;
    if (fOutMaxPacketSize != 0 && (total % fOutMaxPacketSize) == 0) {
        fOutBuf[total] = 0;
        submit++;
    }

    IOReturn rc = (*fData)->WritePipe(fData, fPipeOut, fOutBuf, submit);
    if (rc == kIOUSBPipeStalled) {
        (*fData)->ClearPipeStallBothEnds(fData, fPipeOut);
        rc = (*fData)->WritePipe(fData, fPipeOut, fOutBuf, submit);
    }
    return rc;
}

bool RNDISDevice::addAsyncSource(CFRunLoopRef runLoop)
{
    if (fData == nullptr) {
        return false;
    }
    CFRunLoopSourceRef source = nullptr;
    if ((*fData)->CreateInterfaceAsyncEventSource(fData, &source) != kIOReturnSuccess || source == nullptr) {
        LOG_ERROR("cannot create the USB completion source");
        return false;
    }
    CFRunLoopAddSource(runLoop, source, kCFRunLoopDefaultMode);
    return true;
}

IOReturn RNDISDevice::readAsync(void *buffer, uint32_t length, IOAsyncCallback1 callback, void *refcon)
{
    if (fData == nullptr) {
        return kIOReturnNotOpen;
    }
    return (*fData)->ReadPipeAsync(fData, fPipeIn, buffer, length, callback, refcon);
}

uint32_t RNDISDevice::buildDataMessage(uint8_t *out, const uint8_t *frame, uint32_t length) const
{
    const uint32_t total = length + (uint32_t)sizeof(rndis_data_hdr);
    if (fMaxOutTransfer == 0 || length == 0 || total > fMaxOutTransfer) {
        return 0;
    }
    rndis_data_hdr header = {};
    header.msg_type = RNDIS_MSG_PACKET;
    header.msg_len = cpu_to_le32(total);
    header.data_offset = RNDIS_DATA_HDR_DATA_OFFSET;
    header.data_len = cpu_to_le32(length);
    memcpy(out, &header, sizeof(header));
    memcpy(out + sizeof(header), frame, length);
    // See writeFrame: avoid a transfer that is an exact multiple of the
    // packet size, which the device would not see as finished.
    if (fOutMaxPacketSize != 0 && (total % fOutMaxPacketSize) == 0) {
        out[total] = 0;
        return total + 1;
    }
    return total;
}

IOReturn RNDISDevice::writeAsync(void *buffer, uint32_t length, IOAsyncCallback1 callback, void *refcon)
{
    if (fData == nullptr) {
        return kIOReturnNotOpen;
    }
    return (*fData)->WritePipeAsync(fData, fPipeOut, buffer, length, callback, refcon);
}

void RNDISDevice::clearStallOut()
{
    if (fData != nullptr && fPipeOut != 0) {
        (*fData)->ClearPipeStallBothEnds(fData, fPipeOut);
    }
}

void RNDISDevice::clearStallIn()
{
    if (fData != nullptr && fPipeIn != 0) {
        (*fData)->ClearPipeStallBothEnds(fData, fPipeIn);
    }
}

void RNDISDevice::abortIO()
{
    if (fData == nullptr) {
        return;
    }
    if (fPipeIn != 0) {
        (*fData)->AbortPipe(fData, fPipeIn);
    }
    if (fPipeOut != 0) {
        (*fData)->AbortPipe(fData, fPipeOut);
    }
}
