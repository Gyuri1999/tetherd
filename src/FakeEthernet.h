/* FakeEthernet.h
 * tetherd -- userspace RNDIS USB tethering daemon for macOS.
 *
 * The network side. macOS has no way for a process to register a real
 * Ethernet controller, so this uses a pair of "feth" (fake Ethernet)
 * interfaces, which behave like two ports joined by a cable:
 *
 *      phone <-USB-> tetherd <-BPF-> fethB ==== fethA <-> macOS network stack
 *                                  (I/O side)  (host side)
 *
 * The host side is an ordinary Ethernet interface as far as macOS is
 * concerned (it runs DHCP, ARP and IPv6 on it itself). The daemon attaches a
 * BPF device to the I/O side: frames it writes there come out of the host
 * side, and frames the host sends arrive there to be read.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

class FakeEthernet {
public:
    FakeEthernet() = default;
    ~FakeEthernet() { destroy(); }
    FakeEthernet(const FakeEthernet &) = delete;
    FakeEthernet &operator=(const FakeEthernet &) = delete;

    /// Creates the interface pair, gives the host side the MAC and MTU,
    /// brings both up and attaches BPF to the I/O side. Needs root.
    bool create(const uint8_t mac[6], uint32_t mtu);
    /// Asks macOS to configure the host side by DHCP (and IPv6 automatically).
    bool startAutoConfiguration();
    void destroy();

    const std::string &hostInterface() const { return fHostIf; }

    /// Drops the DHCP lease attempt and starts over (the phone did not answer).
    bool restartAutoConfiguration();

    /// Delivers one Ethernet frame to the macOS network stack.
    bool inject(const uint8_t *frame, uint32_t length);

    typedef std::function<void(const uint8_t *frame, uint32_t length)> FrameHandler;
    /// Waits briefly for frames the host is sending and hands each to the
    /// handler. Returns false on a fatal error, true otherwise (including
    /// when the wait timed out with nothing to deliver).
    bool readFrames(const FrameHandler &handler);

private:
    bool openBpf();

    std::string fHostIf;
    std::string fIoIf;
    int fBpf = -1;
    std::vector<uint8_t> fReadBuf;
};
