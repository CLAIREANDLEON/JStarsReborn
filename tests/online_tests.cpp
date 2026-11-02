// Tests for src/launcher/online.cpp: room codes, peer parsing, UPnP parsing, and the UPnP calls against a fake router on
// localhost (no real router or network is touched).
//   g++ -std=c++20 -Isrc/launcher tests/online_tests.cpp src/launcher/online.cpp -pthread -o online_tests && ./online_tests
#include "online.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

static int g_pass, g_fail;
#define CHECK(c, what) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

// A minimal HTTP server answering like a router: GET /desc.xml, POST /ctl (SOAP). Records the last SOAP action.
static std::atomic<int> g_port{0};
static std::string g_last_action, g_last_body;
static void fake_router(int listener, int requests) {
    const std::string desc =
        "<?xml version=\"1.0\"?><root><device><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/l3f</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl</controlURL></service>"
        "</serviceList></device></root>";
    for (int i = 0; i < requests; ++i) {
        int c = accept(listener, nullptr, nullptr);
        if (c < 0) return;
        std::string req;
        char buf[4096];
        for (;;) {
            const int n = static_cast<int>(recv(c, buf, sizeof buf, 0));
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            const auto h = req.find("\r\n\r\n");
            if (h == std::string::npos) continue;
            const auto cl = req.find("Content-Length: ");
            const std::size_t need = cl == std::string::npos ? 0 : std::stoul(req.substr(cl + 16));
            if (req.size() >= h + 4 + need) break;
        }
        std::string body, status = "200 OK";
        if (req.rfind("GET /desc.xml", 0) == 0) body = desc;
        else if (req.rfind("POST /ctl", 0) == 0) {
            const auto a = req.find("#"), q = req.find('"', a);
            g_last_action = req.substr(a + 1, q - a - 1);
            g_last_body = req.substr(req.find("\r\n\r\n") + 4);
            if (g_last_action == "GetExternalIPAddress")
                body = "<s:Envelope><s:Body><u:GetExternalIPAddressResponse><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>"
                       "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
            else if (g_last_action == "AddPortMapping" && g_last_body.find("<NewExternalPort>1</NewExternalPort>") != std::string::npos) {
                status = "500 Internal Server Error";
                body = "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>718</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
            } else body = "<s:Envelope><s:Body/></s:Envelope>";
        } else status = "404 Not Found";
        const std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: text/xml\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, resp.data(), resp.size(), 0);
        close(c);
    }
}

int main() {
    using namespace online;
    // room codes
    const uint32_t ip = (203u << 24) | (0u << 16) | (113u << 8) | 7u;
    const std::string code = encode_room_code(ip, 47391);
    uint32_t dip = 0;
    uint16_t dport = 0;
    CHECK(code.size() == 12 && code[5] == '-', "a room code is 11 characters in two groups");
    CHECK(decode_peer(code, &dip, &dport) && dip == ip && dport == 47391, "a room code decodes to the address and port");
    std::string lowered = code;
    for (auto& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    CHECK(decode_peer(" " + lowered + " ", &dip, &dport) && dip == ip, "codes are case-insensitive and spaces are ignored");
    std::string typo = code;
    typo[2] = typo[2] == '0' ? '1' : '0';
    CHECK(!decode_peer(typo, &dip, &dport), "a mistyped code is rejected by its check character");
    CHECK(decode_peer("192.168.1.20", &dip, &dport) && dport == kDirectoryPort && dip == ((192u << 24) | (168u << 16) | (1u << 8) | 20u), "a plain address uses the directory port");
    CHECK(decode_peer("10.0.0.2:5000", &dip, &dport) && dport == 5000, "an address with a port");
    CHECK(!decode_peer("300.1.1.1", &dip, &dport) && !decode_peer("hello", &dip, &dport) && !decode_peer("1.2.3.4:0", &dip, &dport), "bad addresses are rejected");
    std::string bad;
    CHECK(peers_to_lobby_arg(code + ", 192.168.1.20 ; nope", &bad) == "203.0.113.7:47391,192.168.1.20:47391" && bad == "nope",
          "codes and addresses become the runtime's --lobby-peer list; invalid entries are reported");
    // SSDP and description parsing
    std::string loc;
    CHECK(parse_ssdp_location("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nLocation: http://192.168.1.1:5000/rootDesc.xml\r\nST: x\r\n\r\n", &loc) &&
          loc == "http://192.168.1.1:5000/rootDesc.xml", "the router's description URL is read from its SSDP answer");
    Igd igd;
    CHECK(parse_igd_description("<root><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType><controlURL>ctl/ppp</controlURL></service></root>",
                                "http://192.168.1.1:5000/desc/root.xml", &igd) && igd.control_url == "http://192.168.1.1:5000/desc/ctl/ppp",
          "a relative control URL is resolved against the description's location");
    CHECK(parse_igd_description("<root><URLBase>http://10.0.0.1:80</URLBase><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType><controlURL>/ip</controlURL></service></root>",
                                "http://10.0.0.1:1900/x.xml", &igd) && igd.control_url == "http://10.0.0.1:80/ip",
          "URLBase wins over the location when present");
    CHECK(!parse_igd_description("<root><service><serviceType>urn:other</serviceType><controlURL>/a</controlURL></service></root>", "http://1.2.3.4/", &igd),
          "a device without a WAN connection service is not a gateway");
    // Against a fake router on localhost
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t al = sizeof a;
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &al);
    listen(ls, 8);
    const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port));
    std::thread srv(fake_router, ls, 5);
    std::string log, ext;
    Igd r;
    CHECK(igd_find(&r, &log, base + "/desc.xml") && r.control_url == base + "/ctl", "the gateway's control URL is found from its description");
    CHECK(igd_external_ip(r, &ext, &log) && ext == "203.0.113.7", "the router's public address is read");
    CHECK(igd_add_udp_mapping(r, 47391, "192.168.1.65", "JStarsReborn", &log) && g_last_action == "AddPortMapping" &&
          g_last_body.find("<NewInternalClient>192.168.1.65</NewInternalClient>") != std::string::npos && g_last_body.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos,
          "a UDP port mapping is requested for this PC");
    log.clear();
    CHECK(!igd_add_udp_mapping(r, 1, "192.168.1.65", "JStarsReborn", &log) && log.find("718") != std::string::npos, "a refused mapping is reported with the router's error code");
    CHECK(igd_delete_udp_mapping(r, 47391, &log) && g_last_action == "DeletePortMapping", "the mapping is removed again");
    srv.join();
    close(ls);
    std::printf("online_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
