#pragma once
// Abstrakce virtuálního síťového rozhraní (TUN/TAP) pro modem_tap.
//
// Tun = L3 (IP pakety bez ethernetové hlavičky) — přenositelné, funguje
// na Linuxu i macOS (utun). Tap = L2 (celé ethernetové rámce) — pouze
// Linux, macOS nemá standardní tap bez kernel extension.
//
// Rozhraní je záměrně malé a neblokující: readPacket/writePacket se volají
// z hlavní smyčky modem_tap po poll(), žádné vlákno navíc.

#include <cstdint>
#include <span>
#include <string>

namespace am {

enum class NetifMode { Tun, Tap }; // Tun = L3 IP pakety (přenositelné), Tap = L2 Ethernet (jen Linux)

class TunTapDevice {
public:
    TunTapDevice() = default;
    ~TunTapDevice();
    TunTapDevice(const TunTapDevice&) = delete;
    TunTapDevice& operator=(const TunTapDevice&) = delete;

    // Otevře virtuální rozhraní. name_hint == "" znamená výchozí jméno
    // (Linux: "am0"; macOS: kernel přidělí utunN). Vrací false při chybě
    // (a vypíše důvod na stderr) — typicky chybějící oprávnění (root).
    bool open(NetifMode mode, const std::string& name_hint);
    void close();

    const std::string& name() const { return name_; }
    int fd() const { return fd_; }

    // Nonblocking čtení jednoho paketu do buf. Vrací -1, když nic není
    // k dispozici, jinak délku paketu v bajtech (může být 0).
    long readPacket(std::span<uint8_t> buf);

    // Zapíše jeden paket na rozhraní. Vrací false při chybě zápisu.
    bool writePacket(std::span<const uint8_t> pkt);

private:
    int fd_ = -1;
    std::string name_;
    NetifMode mode_ = NetifMode::Tun;
};

} // namespace am
