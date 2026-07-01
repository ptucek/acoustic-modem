// Implementace TUN/TAP rozhraní — platformově specifická.
//
// Linux: klasické /dev/net/tun + ioctl(TUNSETIFF). Rámce/pakety bez
// dalšího obalu (IFF_NO_PI).
//
// macOS: kernelové utun rozhraní přes PF_SYSTEM/SYSPROTO_CONTROL socket.
// Podporuje jen L3 (Tun) — Tap na macOS vyžaduje kernel extension (tuntap
// kext), který novější macOS verze nepodporují bez dalšího nastavení, tak
// to nemá smysl implementovat. utun navíc obaluje každý paket 4bajtovou
// hlavičkou s protokolovou rodinou (AF_INET/AF_INET6), kterou je nutné
// při čtení odstranit a při zápisu doplnit.

#include "link/tap_device.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#endif

namespace am {

TunTapDevice::~TunTapDevice() { close(); }

void TunTapDevice::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    name_.clear();
}

#if defined(__linux__)

bool TunTapDevice::open(NetifMode mode, const std::string& name_hint) {
    mode_ = mode;

    int fd = ::open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        std::fprintf(stderr,
                      "tap_device: nepodařilo se otevřít /dev/net/tun (%s) — "
                      "je potřeba root (sudo)?\n",
                      std::strerror(errno));
        return false;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = (mode == NetifMode::Tun ? IFF_TUN : IFF_TAP) | IFF_NO_PI;

    std::string hint = name_hint.empty() ? "am0" : name_hint;
    if (hint.size() >= IFNAMSIZ) hint.resize(IFNAMSIZ - 1);
    std::memcpy(ifr.ifr_name, hint.c_str(), hint.size() + 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        std::fprintf(stderr, "tap_device: ioctl(TUNSETIFF) selhal (%s)\n",
                      std::strerror(errno));
        ::close(fd);
        return false;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        std::fprintf(stderr, "tap_device: fcntl(O_NONBLOCK) selhal (%s)\n",
                      std::strerror(errno));
        ::close(fd);
        return false;
    }

    fd_ = fd;
    name_ = ifr.ifr_name;
    return true;
}

long TunTapDevice::readPacket(std::span<uint8_t> buf) {
    ssize_t n = ::read(fd_, buf.data(), buf.size());
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -1; // jiná chyba — pro hlavní smyčku ekvivalentní "nic"
    }
    return long(n);
}

bool TunTapDevice::writePacket(std::span<const uint8_t> pkt) {
    ssize_t n = ::write(fd_, pkt.data(), pkt.size());
    return n == static_cast<ssize_t>(pkt.size());
}

#elif defined(__APPLE__)

namespace {
constexpr int kUtunOptIfname = 2; // UTUN_OPT_IFNAME
} // namespace

bool TunTapDevice::open(NetifMode mode, const std::string& /*name_hint*/) {
    mode_ = mode;

    if (mode == NetifMode::Tap) {
        std::fprintf(stderr,
                      "tap_device: TAP (L2) není na macOS bez kernel extension "
                      "podporován — použij --mode tun\n");
        return false;
    }

    int fd = ::socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) {
        std::fprintf(stderr,
                      "tap_device: socket(PF_SYSTEM) selhal (%s) — je potřeba "
                      "root (sudo)?\n",
                      std::strerror(errno));
        return false;
    }

    struct ctl_info info;
    std::memset(&info, 0, sizeof(info));
    std::strncpy(info.ctl_name, "com.apple.net.utun_control", sizeof(info.ctl_name) - 1);

    if (ioctl(fd, CTLIOCGINFO, &info) < 0) {
        std::fprintf(stderr, "tap_device: ioctl(CTLIOCGINFO) selhal (%s)\n",
                      std::strerror(errno));
        ::close(fd);
        return false;
    }

    struct sockaddr_ctl addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sc_id = info.ctl_id;
    addr.sc_len = sizeof(addr);
    addr.sc_family = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;
    addr.sc_unit = 0; // 0 = necháme kernel přidělit první volný utunN

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "tap_device: connect(utun_control) selhal (%s)\n",
                      std::strerror(errno));
        ::close(fd);
        return false;
    }

    char ifname[64] = {0};
    socklen_t ifname_len = sizeof(ifname);
    if (getsockopt(fd, SYSPROTO_CONTROL, kUtunOptIfname, ifname, &ifname_len) < 0) {
        std::fprintf(stderr, "tap_device: getsockopt(UTUN_OPT_IFNAME) selhal (%s)\n",
                      std::strerror(errno));
        ::close(fd);
        return false;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        std::fprintf(stderr, "tap_device: fcntl(O_NONBLOCK) selhal (%s)\n",
                      std::strerror(errno));
        ::close(fd);
        return false;
    }

    fd_ = fd;
    name_ = ifname;
    return true;
}

long TunTapDevice::readPacket(std::span<uint8_t> buf) {
    // utun rámec = 4B protokolová rodina (network byte order) + IP paket.
    uint8_t raw[4 + 65536];
    ssize_t n = ::read(fd_, raw, sizeof(raw));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -1;
    }
    if (n < 4) return -1; // příliš krátké na platný utun rámec

    size_t payload_len = size_t(n) - 4;
    if (payload_len > buf.size()) payload_len = buf.size();
    std::memcpy(buf.data(), raw + 4, payload_len);
    return long(payload_len);
}

bool TunTapDevice::writePacket(std::span<const uint8_t> pkt) {
    if (pkt.empty()) return false;

    // Rozhodnutí AF podle horního nibblu prvního bajtu IP hlavičky
    // (verze 4 nebo 6; cokoliv jiného bereme jako IPv4 — bezpečný default).
    const uint8_t version_nibble = uint8_t(pkt[0] >> 4);
    const uint32_t af = (version_nibble == 6) ? AF_INET6 : AF_INET;
    const uint32_t af_net = htonl(af);

    std::vector<uint8_t> frame;
    frame.reserve(pkt.size() + 4);
    const uint8_t* af_bytes = reinterpret_cast<const uint8_t*>(&af_net);
    frame.insert(frame.end(), af_bytes, af_bytes + 4);
    frame.insert(frame.end(), pkt.begin(), pkt.end());

    ssize_t n = ::write(fd_, frame.data(), frame.size());
    return n == static_cast<ssize_t>(frame.size());
}

#else // jiné platformy — nepodporováno

bool TunTapDevice::open(NetifMode /*mode*/, const std::string& /*name_hint*/) {
    std::fprintf(stderr, "tap_device: TUN/TAP není na této platformě podporován\n");
    return false;
}

long TunTapDevice::readPacket(std::span<uint8_t> /*buf*/) { return -1; }

bool TunTapDevice::writePacket(std::span<const uint8_t> /*pkt*/) { return false; }

#endif

} // namespace am
