/* main.cpp
 * tetherd -- userspace RNDIS USB tethering daemon for macOS.
 *
 * Waits for an RNDIS function to appear on USB (an Android phone with USB
 * tethering switched on), then runs a session for it: RNDISDevice talks to
 * the phone, FakeEthernet presents an Ethernet interface to macOS, and two
 * threads copy frames between them.
 *
 * No kernel extension, DriverKit extension, entitlement or signature is
 * involved; the daemon only needs to run as root.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <atomic>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include "FakeEthernet.h"
#include "Log.h"
#include "RNDISDevice.h"
#include "RNDISProtocolCore.h"

bool gVerbose = false;

void logLine(const char *level, const char *fmt, ...)
{
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmNow);

    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s tetherd[%s]: %s\n", stamp, level, message);
}

// ---------------------------------------------------------------------------
// USB discovery
// ---------------------------------------------------------------------------

namespace {

const int kEthernetHeaderLength = 14;
const uint8_t kUSBClassCDCData = 10;

/// An RNDIS function: its control interface and the CDC data interface that
/// directly follows it. Owns a reference on both services.
struct RndisFunction {
    io_service_t control = IO_OBJECT_NULL;
    io_service_t data = IO_OBJECT_NULL;
    uint8_t controlIfNum = 0;
    std::string deviceName;

    void release()
    {
        if (control != IO_OBJECT_NULL) { IOObjectRelease(control); control = IO_OBJECT_NULL; }
        if (data != IO_OBJECT_NULL) { IOObjectRelease(data); data = IO_OBJECT_NULL; }
    }
};

bool intProperty(io_registry_entry_t entry, CFStringRef key, int *value)
{
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (ref == nullptr) {
        return false;
    }
    bool ok = CFGetTypeID(ref) == CFNumberGetTypeID() &&
              CFNumberGetValue((CFNumberRef)ref, kCFNumberIntType, value);
    CFRelease(ref);
    return ok;
}

std::string stringProperty(io_registry_entry_t entry, CFStringRef key)
{
    std::string result;
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (ref != nullptr) {
        char buf[256];
        if (CFGetTypeID(ref) == CFStringGetTypeID() &&
            CFStringGetCString((CFStringRef)ref, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            result = buf;
        }
        CFRelease(ref);
    }
    return result;
}

/// The interface class/subclass/protocol triples that mean "RNDIS control".
/// These are the same ones HoRNDIS and TetherKit match on.
bool isRndisControl(int cls, int subclass, int protocol)
{
    return (cls == 224 && subclass == 1 && protocol == 3)     // stock Android
        || (cls == 239 && subclass == 4 && protocol == 1)     // "RNDIS over Ethernet"
        || (cls == 2 && subclass == 2 && protocol == 255);    // Linux USB gadget
}

bool isRndisControl(io_service_t interface, int *interfaceNumber)
{
    int cls = 0, subclass = 0, protocol = 0;
    return intProperty(interface, CFSTR("bInterfaceClass"), &cls)
        && intProperty(interface, CFSTR("bInterfaceSubClass"), &subclass)
        && intProperty(interface, CFSTR("bInterfaceProtocol"), &protocol)
        && intProperty(interface, CFSTR("bInterfaceNumber"), interfaceNumber)
        && isRndisControl(cls, subclass, protocol);
}

/// Looks among the control interface's siblings for the CDC data interface
/// numbered one higher. Returns a retained service or IO_OBJECT_NULL.
io_service_t copyDataSibling(io_service_t control, int controlIfNum, std::string *deviceName)
{
    io_registry_entry_t device = IO_OBJECT_NULL;
    if (IORegistryEntryGetParentEntry(control, kIOServicePlane, &device) != KERN_SUCCESS) {
        return IO_OBJECT_NULL;
    }
    if (deviceName != nullptr) {
        *deviceName = stringProperty(device, CFSTR("USB Product Name"));
        if (deviceName->empty()) {
            *deviceName = "USB device";
        }
    }

    io_service_t found = IO_OBJECT_NULL;
    io_iterator_t children = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(device, kIOServicePlane, &children) == KERN_SUCCESS) {
        io_service_t child;
        while ((child = IOIteratorNext(children)) != IO_OBJECT_NULL) {
            int number = -1, cls = -1;
            if (found == IO_OBJECT_NULL &&
                IOObjectConformsTo(child, kIOUSBHostInterfaceClassName) &&
                intProperty(child, CFSTR("bInterfaceNumber"), &number) &&
                intProperty(child, CFSTR("bInterfaceClass"), &cls) &&
                number == controlIfNum + 1 && cls == kUSBClassCDCData) {
                found = child;  // keep the reference
            } else {
                IOObjectRelease(child);
            }
        }
        IOObjectRelease(children);
    }
    IOObjectRelease(device);
    return found;
}

/// Fills in `function` if `interface` is an RNDIS control interface with its
/// data interface next to it. Takes its own references.
bool describeFunction(io_service_t interface, RndisFunction *function)
{
    int number = 0;
    if (!isRndisControl(interface, &number)) {
        return false;
    }
    // The control interface can be announced a moment before its sibling.
    io_service_t data = IO_OBJECT_NULL;
    for (int attempt = 0; attempt < 10 && data == IO_OBJECT_NULL; attempt++) {
        if (attempt > 0) {
            usleep(100 * 1000);
        }
        data = copyDataSibling(interface, number, &function->deviceName);
    }
    if (data == IO_OBJECT_NULL) {
        LOG_ERROR("found an RNDIS control interface (%d) on '%s' but no CDC data "
                  "interface after it; ignoring", number, function->deviceName.c_str());
        return false;
    }
    IOObjectRetain(interface);
    function->control = interface;
    function->data = data;
    function->controlIfNum = (uint8_t)number;
    return true;
}

// ---------------------------------------------------------------------------
// Session: one connected phone
// ---------------------------------------------------------------------------

struct Session {
    RNDISDevice device;
    FakeEthernet ethernet;
    std::string deviceName;

    std::thread receiver;     // phone -> macOS
    std::thread transmitter;  // macOS -> phone
    std::atomic<bool> stop{false};
    std::atomic<bool> receiverDone{false};
    std::atomic<bool> transmitterDone{false};
    std::atomic<bool> deviceGone{false};

    std::atomic<uint64_t> framesIn{0};
    std::atomic<uint64_t> framesOut{0};
    std::atomic<uint64_t> usbReads{0};    // completed bulk IN transfers

    // Bulk IN: kReadsInFlight buffers, each always queued on the pipe.
    // Bulk OUT: a ring of kWritesInFlight buffers, used in order (the pipe
    // completes transfers in order); outSlotsFree counts the idle ones.
    // Completions for both run on the receiver thread's run loop.
    std::vector<std::vector<uint8_t>> inBufs;
    std::vector<std::vector<uint8_t>> outBufs;
    dispatch_semaphore_t outSlotsFree = nullptr;
    std::atomic<int> readsInFlight{0};
    std::atomic<int> writesInFlight{0};
    int readErrors = 0;          // receiver thread only
    unsigned outNext = 0;        // transmitter thread only

    // Address watch (main thread only): see watchAddress().
    dispatch_source_t watcher = nullptr;
    time_t lastDhcpStart = 0;
    int dhcpRestarts = 0;
    bool haveAddress = false;
    CFDictionaryRef publishedIPv4 = nullptr;   // what we last made primary
    CFDictionaryRef publishedDNS = nullptr;
};

Session *gSession = nullptr;   // main thread only
bool gConfigureAddress = true;
bool gMakePrimary = false;     // --primary
SCDynamicStoreRef gStore = nullptr;

// Phones often ignore the first DHCP request on a new link and the client
// then backs off, so ask again soon rather than waiting out its timer.
const int kDhcpRetrySeconds = 4;
const int kReadsInFlight = 8;
const int kWritesInFlight = 8;
const int kDhcpMaxRestarts = 3;

bool isDeviceGone(IOReturn rc)
{
    return rc == kIOReturnNoDevice || rc == kIOReturnNotResponding ||
           rc == kIOReturnNotOpen || rc == kIOReturnNotAttached;
}

void endSession(Session *session, const char *reason);

// ---------------------------------------------------------------------------
// Address watch and "main connection"
//
// macOS's DHCP client publishes what it learns under
// State:/Network/Service/DHCP-<if>/{IPv4,DNS}. A service there gets only a
// scoped route: the interface is not a configured network service, so it
// never outranks Wi-Fi or Ethernet. With --primary, tetherd publishes a copy
// as its own service with OverridePrimary set, the way VPN clients do, and
// configd then routes default traffic and DNS through the phone. The keys
// are "temporary": configd drops them if tetherd dies, so a crash cannot
// leave the Mac routing to a vanished interface.
// ---------------------------------------------------------------------------

CFStringRef createString(const std::string &text)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

CFStringRef createServiceKey(const std::string &service, const char *entity)
{
    return createString("State:/Network/Service/" + service + "/" + entity);
}

CFDictionaryRef copyDictionary(CFStringRef key)
{
    CFPropertyListRef value = SCDynamicStoreCopyValue(gStore, key);
    if (value != nullptr && CFGetTypeID(value) != CFDictionaryGetTypeID()) {
        CFRelease(value);
        return nullptr;
    }
    return (CFDictionaryRef)value;
}

bool sameDictionary(CFDictionaryRef a, CFDictionaryRef b)
{
    return (a == nullptr && b == nullptr) || (a != nullptr && b != nullptr && CFEqual(a, b));
}

std::string firstString(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (value != nullptr && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount((CFArrayRef)value) > 0) {
        value = CFArrayGetValueAtIndex((CFArrayRef)value, 0);
    }
    char buffer[64] = "?";
    if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)value, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    }
    return buffer;
}

/// Replaces a temporary key: AddTemporaryValue only adds missing keys.
void putTemporary(CFStringRef key, CFDictionaryRef value)
{
    SCDynamicStoreRemoveValue(gStore, key);
    if (!SCDynamicStoreAddTemporaryValue(gStore, key, value)) {
        LOG_ERROR("cannot publish network state: %s", SCErrorString(SCError()));
    }
}

void unpublishPrimary(Session *session)
{
    if (session->publishedIPv4 == nullptr) {
        return;
    }
    const std::string service = "tetherd-" + session->ethernet.hostInterface();
    for (const char *entity : {"IPv4", "DNS"}) {
        CFStringRef key = createServiceKey(service, entity);
        SCDynamicStoreRemoveValue(gStore, key);
        CFRelease(key);
    }
    CFRelease(session->publishedIPv4);
    session->publishedIPv4 = nullptr;
    if (session->publishedDNS != nullptr) {
        CFRelease(session->publishedDNS);
        session->publishedDNS = nullptr;
    }
    LOG_INFO("%s is no longer the main connection", session->ethernet.hostInterface().c_str());
}

/// Publishes (or refreshes) our primary copy of the DHCP service.
void publishPrimary(Session *session, CFDictionaryRef ipv4, CFDictionaryRef dns)
{
    if (!CFDictionaryContainsKey(ipv4, CFSTR("Router"))) {
        return;   // nothing to route through yet
    }
    if (session->publishedIPv4 != nullptr && CFEqual(ipv4, session->publishedIPv4) &&
        sameDictionary(dns, session->publishedDNS)) {
        return;   // unchanged
    }
    const bool first = session->publishedIPv4 == nullptr;
    const std::string service = "tetherd-" + session->ethernet.hostInterface();

    CFMutableDictionaryRef primary = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (CFStringRef key : {CFSTR("Addresses"), CFSTR("SubnetMasks"), CFSTR("Router"),
                            CFSTR("InterfaceName")}) {
        if (CFTypeRef value = CFDictionaryGetValue(ipv4, key)) {
            CFDictionarySetValue(primary, key, value);
        }
    }
    int one = 1;
    CFNumberRef yes = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    CFDictionarySetValue(primary, CFSTR("OverridePrimary"), yes);
    CFRelease(yes);

    CFStringRef key = createServiceKey(service, "IPv4");
    putTemporary(key, primary);
    CFRelease(key);
    CFRelease(primary);
    if (dns != nullptr) {
        key = createServiceKey(service, "DNS");
        putTemporary(key, dns);
        CFRelease(key);
    }

    if (session->publishedIPv4 != nullptr) CFRelease(session->publishedIPv4);
    if (session->publishedDNS != nullptr) CFRelease(session->publishedDNS);
    session->publishedIPv4 = (CFDictionaryRef)CFRetain(ipv4);
    session->publishedDNS = dns != nullptr ? (CFDictionaryRef)CFRetain(dns) : nullptr;
    if (first) {
        LOG_INFO("%s is now the main connection (router %s)",
                 session->ethernet.hostInterface().c_str(), firstString(ipv4, CFSTR("Router")).c_str());
    }
}

/// Main thread, once a second while a session runs. Logs when the phone
/// hands out an address, asks again when it does not, and keeps the
/// "main connection" entry in step with the DHCP lease.
void watchAddress(Session *session)
{
    const std::string service = "DHCP-" + session->ethernet.hostInterface();
    CFStringRef key = createServiceKey(service, "IPv4");
    CFDictionaryRef ipv4 = copyDictionary(key);
    CFRelease(key);
    const bool hasAddress = ipv4 != nullptr && CFDictionaryContainsKey(ipv4, CFSTR("Addresses"));

    if (hasAddress) {
        if (!session->haveAddress) {
            session->haveAddress = true;
            LOG_INFO("%s got address %s from the phone", session->ethernet.hostInterface().c_str(),
                     firstString(ipv4, CFSTR("Addresses")).c_str());
        }
        if (gMakePrimary) {
            key = createServiceKey(service, "DNS");
            CFDictionaryRef dns = copyDictionary(key);
            CFRelease(key);
            publishPrimary(session, ipv4, dns);
            if (dns != nullptr) CFRelease(dns);
        }
    } else {
        if (session->haveAddress) {
            session->haveAddress = false;
            LOG_INFO("%s lost its address", session->ethernet.hostInterface().c_str());
            unpublishPrimary(session);
        }
        const time_t now = time(nullptr);
        if (now - session->lastDhcpStart >= kDhcpRetrySeconds &&
            session->dhcpRestarts < kDhcpMaxRestarts) {
            session->dhcpRestarts++;
            session->lastDhcpStart = now;
            LOG_INFO("the phone has not answered DHCP; asking again (%d of %d)",
                     session->dhcpRestarts, kDhcpMaxRestarts);
            session->ethernet.restartAutoConfiguration();
        } else if (session->dhcpRestarts == kDhcpMaxRestarts &&
                   now - session->lastDhcpStart >= kDhcpRetrySeconds) {
            session->dhcpRestarts++;   // log once
            LOG_ERROR("the phone is not handing out an address; turn USB tethering off and on "
                      "on the phone");
        }
    }
    if (ipv4 != nullptr) CFRelease(ipv4);
}

void startAddressWatch(Session *session)
{
    session->lastDhcpStart = time(nullptr);
    session->watcher = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                              dispatch_get_main_queue());
    dispatch_source_set_timer(session->watcher, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(session->watcher, ^{
        watchAddress(session);
    });
    dispatch_resume(session->watcher);
}

void stopAddressWatch(Session *session)
{
    if (session->watcher != nullptr) {
        dispatch_source_cancel(session->watcher);
        dispatch_release(session->watcher);
        session->watcher = nullptr;
    }
    unpublishPrimary(session);
}

struct InTransfer {
    Session *session;
    int index;
};

void submitRead(InTransfer *transfer);

/// Receiver thread (run loop callback): one bulk IN transfer finished.
void onReadComplete(void *refcon, IOReturn rc, void *arg0)
{
    InTransfer *transfer = static_cast<InTransfer *>(refcon);
    Session *session = transfer->session;
    session->readsInFlight--;
    if (session->stop) {
        return;
    }
    if (rc == kIOReturnSuccess) {
        session->usbReads++;
        session->readErrors = 0;
        // One transfer may carry several RNDIS data messages.
        rndis::ParseResult result = rndis::parseInboundBuffer(
            session->inBufs[transfer->index].data(), (uint32_t)(uintptr_t)arg0,
            [](void *context, const uint8_t *frame, uint32_t frameLength) {
                Session *s = static_cast<Session *>(context);
                if (s->ethernet.inject(frame, frameLength)) {
                    s->framesIn++;
                }
            },
            session);
        if (result != rndis::ParseResult::OK) {
            LOG_DEBUG("malformed RNDIS data from the device (parse result %d)", (int)result);
        }
        submitRead(transfer);
        return;
    }
    if (isDeviceGone(rc)) {
        LOG_INFO("USB read: device gone (0x%08x)", rc);
        session->deviceGone = true;
        return;
    }
    if (rc == kIOUSBPipeStalled) {
        session->device.clearStallIn();
    } else if (rc != kIOReturnAborted) {
        LOG_ERROR("USB read failed: 0x%08x", rc);
    }
    // An unplug can also surface as repeated aborts or errors.
    if (++session->readErrors >= 8 * kReadsInFlight) {
        LOG_INFO("USB read: %d errors in a row (last 0x%08x)", session->readErrors, rc);
        session->deviceGone = true;
        return;
    }
    submitRead(transfer);
}

void submitRead(InTransfer *transfer)
{
    Session *session = transfer->session;
    std::vector<uint8_t> &buffer = session->inBufs[transfer->index];
    session->readsInFlight++;
    IOReturn rc = session->device.readAsync(buffer.data(), (uint32_t)buffer.size(),
                                            onReadComplete, transfer);
    if (rc != kIOReturnSuccess) {
        session->readsInFlight--;
        if (isDeviceGone(rc)) {
            session->deviceGone = true;
        } else {
            LOG_ERROR("cannot queue a USB read: 0x%08x", rc);
            session->deviceGone = true;
        }
    }
}

/// Receiver thread (run loop callback): one bulk OUT transfer finished.
void onWriteComplete(void *refcon, IOReturn rc, void *)
{
    Session *session = static_cast<Session *>(refcon);
    session->writesInFlight--;
    if (rc == kIOReturnSuccess) {
        session->framesOut++;
    } else if (isDeviceGone(rc)) {
        session->deviceGone = true;   // the receiver ends the session
    } else if (rc == kIOUSBPipeStalled) {
        session->device.clearStallOut();
    } else if (rc != kIOReturnAborted) {
        LOG_DEBUG("USB write failed: 0x%08x", rc);
    }
    dispatch_semaphore_signal(session->outSlotsFree);
}

/// Owns the run loop that every USB completion is delivered on, and keeps
/// kReadsInFlight reads queued so the phone never waits for the Mac.
void receiverMain(Session *session)
{
    std::vector<InTransfer> transfers(kReadsInFlight);
    if (session->device.addAsyncSource(CFRunLoopGetCurrent())) {
        for (int i = 0; i < kReadsInFlight && !session->deviceGone; i++) {
            transfers[i] = {session, i};
            submitRead(&transfers[i]);
        }
    } else {
        session->deviceGone = true;
    }

    while (!session->stop && !session->deviceGone) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }

    // Let aborted transfers report back so no completion outlives the
    // session (endSession aborts the pipes). Give up after a second.
    for (int i = 0; i < 40 && (session->readsInFlight > 0 || session->writesInFlight > 0); i++) {
        if (!session->stop) {
            session->device.abortIO();
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, false);
    }

    session->receiverDone = true;
    if (!session->stop) {
        dispatch_async(dispatch_get_main_queue(), ^{
            // Ignore if this session was already torn down for another reason.
            if (gSession == session) {
                endSession(session, "the device stopped responding or was unplugged");
            }
        });
    }
}

/// Copies frames macOS sends (read from BPF) to the phone, with up to
/// kWritesInFlight transfers queued at once.
void transmitterMain(Session *session)
{
    const uint32_t maxFrame = session->device.maxFrameLength();

    while (!session->stop) {
        bool ok = session->ethernet.readFrames([&](const uint8_t *frame, uint32_t length) {
            if (session->stop || session->deviceGone) {
                return;
            }
            if (length > maxFrame) {
                LOG_DEBUG("dropping a %u-byte frame (limit %u)", length, maxFrame);
                return;
            }
            // Wait for a free buffer, checking now and then for shutdown.
            while (dispatch_semaphore_wait(session->outSlotsFree,
                                           dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) {
                if (session->stop || session->deviceGone) {
                    return;
                }
            }
            std::vector<uint8_t> &buffer = session->outBufs[session->outNext];
            session->outNext = (session->outNext + 1) % kWritesInFlight;
            const uint32_t submit = session->device.buildDataMessage(buffer.data(), frame, length);
            if (submit == 0) {
                dispatch_semaphore_signal(session->outSlotsFree);
                return;
            }
            session->writesInFlight++;
            IOReturn rc = session->device.writeAsync(buffer.data(), submit, onWriteComplete, session);
            if (rc != kIOReturnSuccess) {
                session->writesInFlight--;
                dispatch_semaphore_signal(session->outSlotsFree);
                if (isDeviceGone(rc)) {
                    session->deviceGone = true;
                } else {
                    LOG_DEBUG("cannot queue a USB write: 0x%08x", rc);
                }
            }
        });
        if (!ok) {
            usleep(100 * 1000);
        }
    }
    session->transmitterDone = true;
}

/// Main thread. Returns the running session, or nullptr on failure.
Session *startSession(const RndisFunction &function)
{
    LOG_INFO("found RNDIS device '%s' (control interface %u)",
             function.deviceName.c_str(), function.controlIfNum);

    Session *session = new Session;
    session->deviceName = function.deviceName;

    uint8_t mac[6] = {};
    bool ok = session->device.open(function.control, function.data, function.controlIfNum)
           && session->device.rndisInit()
           && session->device.queryMacAddress(mac);
    if (ok) {
        uint32_t mtu = session->device.maxFrameLength() - kEthernetHeaderLength;
        if (mtu > ETHERNET_MTU) {
            mtu = ETHERNET_MTU;
        }
        ok = session->ethernet.create(mac, mtu);
    }
    // Tell the phone to start sending only once there is somewhere to put it.
    if (ok && !session->device.setPacketFilter(RNDIS_DEFAULT_FILTER)) {
        LOG_ERROR("cannot enable packet reception on the device");
        ok = false;
    }
    if (!ok) {
        LOG_ERROR("could not start tethering with '%s'", function.deviceName.c_str());
        delete session;
        return nullptr;
    }

    session->inBufs.assign(kReadsInFlight, std::vector<uint8_t>(IN_BUF_SIZE));
    session->outBufs.assign(kWritesInFlight,
                            std::vector<uint8_t>(session->device.maxFrameLength() + sizeof(rndis_data_hdr) + 1));
    session->outSlotsFree = dispatch_semaphore_create(kWritesInFlight);

    session->receiver = std::thread(receiverMain, session);
    session->transmitter = std::thread(transmitterMain, session);

    if (gConfigureAddress) {
        session->ethernet.startAutoConfiguration();
        startAddressWatch(session);
    }
    LOG_INFO("tethering through '%s' is running on %s",
             function.deviceName.c_str(), session->ethernet.hostInterface().c_str());
    return session;
}

/// Main thread. Stops the threads, releases the device and removes the
/// network interface.
void endSession(Session *session, const char *reason)
{
    if (gSession == session) {
        gSession = nullptr;
    }
    LOG_INFO("stopping tethering through '%s': %s", session->deviceName.c_str(), reason);
    stopAddressWatch(session);   // hand the default route back first

    session->stop = true;
    // A thread may be about to enter a blocking USB call just as we abort,
    // so keep aborting until both have actually left their loops.
    while (!session->receiverDone || !session->transmitterDone) {
        session->device.abortIO();
        usleep(20 * 1000);
    }
    session->receiver.join();
    session->transmitter.join();

    if (!session->deviceGone) {
        session->device.setPacketFilter(0);   // ask the phone to stop sending
    }
    const uint64_t reads = session->usbReads.load();
    LOG_INFO("session totals: %llu frames received in %llu USB transfers (%.1f per transfer), %llu sent",
             (unsigned long long)session->framesIn.load(), (unsigned long long)reads,
             reads ? (double)session->framesIn.load() / (double)reads : 0.0,
             (unsigned long long)session->framesOut.load());
    delete session;   // closes USB, destroys the feth pair
}

// ---------------------------------------------------------------------------
// Device arrival
// ---------------------------------------------------------------------------

void onInterfacesArrived(void *, io_iterator_t iterator)
{
    io_service_t interface;
    // The iterator must be drained completely or no further notifications come.
    while ((interface = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        RndisFunction function;
        if (describeFunction(interface, &function)) {
            if (gSession != nullptr) {
                LOG_INFO("ignoring RNDIS device '%s': already tethering through '%s'",
                         function.deviceName.c_str(), gSession->deviceName.c_str());
            } else {
                gSession = startSession(function);
            }
            function.release();
        }
        IOObjectRelease(interface);
    }
}

/// Talks RNDIS to each attached device without creating a network
/// interface: opens it, initialises it and reads its Ethernet address.
/// A way to check the USB half on its own.
int probeFunction(const RndisFunction &function)
{
    RNDISDevice device;
    uint8_t mac[6] = {};
    if (!device.open(function.control, function.data, function.controlIfNum)
        || !device.rndisInit() || !device.queryMacAddress(mac)) {
        printf("  probe FAILED (see messages above)\n");
        return 1;
    }
    printf("  probe OK: address %02x:%02x:%02x:%02x:%02x:%02x, largest frame %u bytes\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], device.maxFrameLength());
    return 0;
}

int listDevices(bool probe)
{
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL,
                                     IOServiceMatching(kIOUSBHostInterfaceClassName),
                                     &iterator) != KERN_SUCCESS) {
        fprintf(stderr, "cannot enumerate USB interfaces\n");
        return 1;
    }
    int scanned = 0, found = 0, failures = 0;
    io_service_t interface;
    while ((interface = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        scanned++;
        RndisFunction function;
        if (describeFunction(interface, &function)) {
            found++;
            printf("%s: RNDIS control interface %u, data interface %u\n",
                   function.deviceName.c_str(), function.controlIfNum,
                   function.controlIfNum + 1);
            if (probe) {
                failures += probeFunction(function);
            }
            function.release();
        }
        IOObjectRelease(interface);
    }
    IOObjectRelease(iterator);
    printf("%d USB interface(s) scanned, %d RNDIS device(s) found\n", scanned, found);
    if (found == 0) {
        printf("Plug the phone in and switch on USB tethering, then try again.\n");
    }
    return failures == 0 ? 0 : 1;
}

void usage()
{
    fprintf(stderr,
        "usage: tetherd [options]\n"
        "\n"
        "Shares an Android phone's connection over USB (RNDIS tethering).\n"
        "Run as root; it waits for a phone and sets up a network interface.\n"
        "\n"
        "  -l, --list       list attached RNDIS devices and exit (no root needed)\n"
        "  -p, --probe      like --list, and also talk RNDIS to each device\n"
        "                   (initialise it, read its address) without networking\n"
        "  -n, --no-dhcp    create the interface but do not configure an address\n"
        "  -v, --verbose    log more detail\n"
        "      --primary    make the phone the Mac's main connection, ahead of\n"
        "                   Wi-Fi and Ethernet, while tethering\n"
        "      --stdin-eof  stop when standard input is closed (used by Tether.app)\n"
        "  -h, --help       show this help\n");
}

/// Main thread. Ends any session (removing its network interface) and exits.
[[noreturn]] void shutdownAndExit(const char *reason)
{
    if (gSession != nullptr) {
        endSession(gSession, reason);
    }
    LOG_INFO("exiting");
    exit(0);
}

void installSignalHandler(int signalNumber)
{
    signal(signalNumber, SIG_IGN);
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signalNumber, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{
        shutdownAndExit("tetherd is shutting down");
    });
    dispatch_resume(source);
}

/// For --stdin-eof: exit cleanly once standard input reaches end of file.
/// The menu bar app holds the write end of a pipe; closing it, or the app
/// quitting or crashing, stops tethering. This works through sudo, which
/// does not always relay signals sent by the process that started it.
void installStdinWatcher()
{
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_READ, STDIN_FILENO, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{
        char buffer[256];
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            shutdownAndExit("the controlling app closed standard input");
        }
    });
    dispatch_resume(source);
}

} // namespace

int main(int argc, char **argv)
{
    bool list = false, probe = false, stdinEof = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "-l" || arg == "--list") {
            list = true;
        } else if (arg == "-p" || arg == "--probe") {
            list = probe = true;
        } else if (arg == "-n" || arg == "--no-dhcp") {
            gConfigureAddress = false;
        } else if (arg == "--primary") {
            gMakePrimary = true;
        } else if (arg == "--stdin-eof") {
            stdinEof = true;
        } else if (arg == "-v" || arg == "--verbose") {
            gVerbose = true;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            fprintf(stderr, "tetherd: unknown option '%s'\n\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (list) {
        return listDevices(probe);
    }
    if (geteuid() != 0) {
        fprintf(stderr, "tetherd: must run as root (it creates a network interface "
                        "and opens a packet filter device).\n"
                        "Try: sudo %s\n", argv[0]);
        return 1;
    }
    setvbuf(stderr, nullptr, _IOLBF, 0);

    gStore = SCDynamicStoreCreate(kCFAllocatorDefault, CFSTR("tetherd"), nullptr, nullptr);
    if (gStore == nullptr) {
        LOG_ERROR("cannot open the network configuration store: %s", SCErrorString(SCError()));
        return 1;
    }

    installSignalHandler(SIGINT);
    installSignalHandler(SIGTERM);
    if (stdinEof) {
        installStdinWatcher();
    }

    IONotificationPortRef port = IONotificationPortCreate(MACH_PORT_NULL);
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceAddMatchingNotification(
        port, kIOFirstMatchNotification, IOServiceMatching(kIOUSBHostInterfaceClassName),
        onInterfacesArrived, nullptr, &iterator);
    if (kr != KERN_SUCCESS) {
        LOG_ERROR("cannot watch for USB devices: 0x%08x", kr);
        return 1;
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port),
                       kCFRunLoopDefaultMode);

    LOG_INFO("waiting for an RNDIS device (plug in the phone and switch on USB tethering)");
    // Handles devices that are already attached, and arms the notification.
    onInterfacesArrived(nullptr, iterator);

    CFRunLoopRun();
    return 0;
}
