/* FakeEthernet.cpp
 * tetherd -- userspace RNDIS USB tethering daemon for macOS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FakeEthernet.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <net/bpf.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "Log.h"

extern char **environ;

namespace {

const char *const kIfconfig = "/sbin/ifconfig";
const char *const kIpconfig = "/usr/sbin/ipconfig";

/// Runs a system tool by absolute path (no shell). Returns its exit status,
/// or -1 if it could not be run. If output is non-null it receives stdout.
int runTool(const std::vector<std::string> &args, std::string *output = nullptr)
{
    std::vector<char *> argv;
    std::string printable;
    for (const std::string &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
        printable += arg + " ";
    }
    argv.push_back(nullptr);
    LOG_DEBUG("running: %s", printable.c_str());

    int pipeFds[2] = {-1, -1};
    if (pipe(pipeFds) != 0) {
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (rc != 0) {
        close(pipeFds[0]);
        LOG_ERROR("cannot run %s: %s", argv[0], strerror(rc));
        return -1;
    }

    char buf[256];
    ssize_t n;
    while ((n = read(pipeFds[0], buf, sizeof(buf))) > 0) {
        if (output != nullptr) {
            output->append(buf, (size_t)n);
        }
    }
    close(pipeFds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool runOk(const std::vector<std::string> &args)
{
    return runTool(args) == 0;
}

/// "ifconfig feth create" picks a free unit and prints the new name.
std::string createFeth()
{
    std::string name;
    if (runTool({kIfconfig, "feth", "create"}, &name) != 0) {
        return "";
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) {
        name.pop_back();
    }
    return name;
}

} // namespace

bool FakeEthernet::create(const uint8_t mac[6], uint32_t mtu)
{
    destroy();

    fHostIf = createFeth();
    fIoIf = createFeth();
    if (fHostIf.empty() || fIoIf.empty()) {
        LOG_ERROR("cannot create the feth interface pair (are you root?)");
        destroy();
        return false;
    }
    if (!runOk({kIfconfig, fIoIf, "peer", fHostIf})) {
        LOG_ERROR("cannot join %s and %s as a pair", fIoIf.c_str(), fHostIf.c_str());
        destroy();
        return false;
    }

    // Use the address the phone assigned to the host side of the link. If
    // that fails the interface keeps the one macOS gave it, which also works.
    char macText[18];
    snprintf(macText, sizeof(macText), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!runOk({kIfconfig, fHostIf, "lladdr", macText})) {
        LOG_INFO("could not set %s to %s; keeping its default address",
                 fHostIf.c_str(), macText);
    }

    const std::string mtuText = std::to_string(mtu);
    runOk({kIfconfig, fHostIf, "mtu", mtuText});
    runOk({kIfconfig, fIoIf, "mtu", mtuText});

    if (!runOk({kIfconfig, fIoIf, "up"}) || !runOk({kIfconfig, fHostIf, "up"})) {
        LOG_ERROR("cannot bring the feth interfaces up");
        destroy();
        return false;
    }
    if (!openBpf()) {
        destroy();
        return false;
    }
    LOG_INFO("network interface %s is up (address %s, mtu %u; I/O side %s)",
             fHostIf.c_str(), macText, mtu, fIoIf.c_str());
    return true;
}

bool FakeEthernet::startAutoConfiguration()
{
    if (fHostIf.empty()) {
        return false;
    }
    // configd learns about a new interface asynchronously, so right after
    // the feth pair is created `ipconfig set` can fail with "interface
    // doesn't exist". Retry for a few seconds before giving up.
    bool v4 = false;
    int attempt = 0;
    for (; attempt < 20 && !v4; attempt++) {
        if (attempt > 0) {
            usleep(250 * 1000);
        }
        v4 = runOk({kIpconfig, "set", fHostIf, "DHCP"});
    }
    if (!v4) {
        LOG_ERROR("could not start DHCP on %s", fHostIf.c_str());
    } else if (attempt > 1) {
        LOG_INFO("started DHCP on %s after %d attempts", fHostIf.c_str(), attempt);
    }
    if (!runOk({kIpconfig, "set", fHostIf, "AUTOMATIC-V6"})) {
        LOG_DEBUG("could not start IPv6 autoconfiguration on %s", fHostIf.c_str());
    }
    return v4;
}

bool FakeEthernet::restartAutoConfiguration()
{
    if (fHostIf.empty()) {
        return false;
    }
    runOk({kIpconfig, "set", fHostIf, "NONE"});
    return startAutoConfiguration();
}

bool FakeEthernet::openBpf()
{
    for (int unit = 0; unit < 256 && fBpf < 0; unit++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/bpf%d", unit);
        fBpf = open(path, O_RDWR | O_CLOEXEC);
        if (fBpf < 0 && errno != EBUSY) {
            break;  // EBUSY means "in use, try the next one"; anything else is final.
        }
    }
    if (fBpf < 0) {
        LOG_ERROR("cannot open a BPF device: %s", strerror(errno));
        return false;
    }

    // Must be set before the interface is attached.
    u_int bufLen = 512 * 1024;
    if (ioctl(fBpf, BIOCSBLEN, &bufLen) != 0) {
        LOG_DEBUG("BIOCSBLEN failed, using the default buffer size");
    }

    struct ifreq ifr = {};
    strlcpy(ifr.ifr_name, fIoIf.c_str(), sizeof(ifr.ifr_name));
    if (ioctl(fBpf, BIOCSETIF, &ifr) != 0) {
        LOG_ERROR("cannot attach BPF to %s: %s", fIoIf.c_str(), strerror(errno));
        return false;
    }

    u_int on = 1, off = 0;
    struct timeval readTimeout = {0, 250 * 1000};
    bool ok = true;
    // Return frames as they arrive instead of waiting for a full buffer.
    ok &= ioctl(fBpf, BIOCIMMEDIATE, &on) == 0;
    // We supply complete Ethernet headers; do not rewrite the source address.
    ok &= ioctl(fBpf, BIOCSHDRCMPLT, &on) == 0;
    // Do not read back the frames we inject ourselves.
    ok &= ioctl(fBpf, BIOCSSEESENT, &off) == 0;
    // Lets the transmit thread notice a shutdown request.
    ok &= ioctl(fBpf, BIOCSRTIMEOUT, &readTimeout) == 0;
    if (!ok) {
        LOG_ERROR("cannot configure BPF on %s: %s", fIoIf.c_str(), strerror(errno));
        return false;
    }
    // The host's frames are addressed to the phone, not to the I/O side.
    if (ioctl(fBpf, BIOCPROMISC, nullptr) != 0) {
        LOG_DEBUG("BIOCPROMISC failed: %s", strerror(errno));
    }

    if (ioctl(fBpf, BIOCGBLEN, &bufLen) != 0 || bufLen == 0) {
        LOG_ERROR("cannot read the BPF buffer size");
        return false;
    }
    fReadBuf.resize(bufLen);
    return true;
}

void FakeEthernet::destroy()
{
    if (fBpf >= 0) {
        close(fBpf);
        fBpf = -1;
    }
    // Destroying either end also unpairs them.
    if (!fIoIf.empty()) {
        runOk({kIfconfig, fIoIf, "destroy"});
        fIoIf.clear();
    }
    if (!fHostIf.empty()) {
        runOk({kIfconfig, fHostIf, "destroy"});
        fHostIf.clear();
    }
}

bool FakeEthernet::inject(const uint8_t *frame, uint32_t length)
{
    if (fBpf < 0) {
        return false;
    }
    ssize_t n;
    do {
        n = write(fBpf, frame, length);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)length) {
        LOG_DEBUG("injecting a %u-byte frame failed: %s", length, strerror(errno));
        return false;
    }
    return true;
}

bool FakeEthernet::readFrames(const FrameHandler &handler)
{
    if (fBpf < 0) {
        return false;
    }
    ssize_t n = read(fBpf, fReadBuf.data(), fReadBuf.size());
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == ETIMEDOUT) {
            return true;
        }
        LOG_ERROR("reading from BPF failed: %s", strerror(errno));
        return false;
    }

    // One read returns any number of captured frames, each behind a header
    // and padded to BPF's word alignment.
    const uint8_t *p = fReadBuf.data();
    const uint8_t *end = p + n;
    while (p + sizeof(struct bpf_hdr) <= end) {
        struct bpf_hdr hdr;
        memcpy(&hdr, p, sizeof(hdr));
        const uint8_t *frame = p + hdr.bh_hdrlen;
        if (hdr.bh_hdrlen == 0 || frame + hdr.bh_caplen > end) {
            break;
        }
        // caplen < datalen would mean a truncated capture; skip those.
        if (hdr.bh_caplen == hdr.bh_datalen && hdr.bh_caplen > 0) {
            handler(frame, hdr.bh_caplen);
        }
        p += BPF_WORDALIGN(hdr.bh_hdrlen + hdr.bh_caplen);
    }
    return true;
}
