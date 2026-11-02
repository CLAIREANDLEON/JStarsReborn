// JStarsReborn launcher - online helpers (see online.hpp).
#include "online.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static constexpr sock_t kBad = INVALID_SOCKET;
static void close_sock(sock_t s) { closesocket(s); }
static void set_timeout(sock_t s, int ms) {
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}
static bool net_init() { static bool ok = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }(); return ok; }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t kBad = -1;
static void close_sock(sock_t s) { close(s); }
static void set_timeout(sock_t s, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
static bool net_init() { return true; }
#endif
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace online {

namespace {
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // Crockford base32: no I, L, O, U
int b32_value(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char* p = std::strchr(kAlphabet, c);
    return (p && c) ? static_cast<int>(p - kAlphabet) : -1;
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// Text of the first <tag>...</tag> inside `xml` at or after `from`, case-insensitive tag match. npos if none.
std::string::size_type find_tag(const std::string& xml, const std::string& tag, std::string::size_type from, std::string* text) {
    const std::string lx = lower(xml), open = "<" + lower(tag) + ">", close = "</" + lower(tag) + ">";
    const auto a = lx.find(open, from);
    if (a == std::string::npos) return a;
    const auto b = lx.find(close, a + open.size());
    if (b == std::string::npos) return std::string::npos;
    if (text) *text = trim(xml.substr(a + open.size(), b - a - open.size()));
    return b + close.size();
}
bool split_url(const std::string& url, std::string* host, uint16_t* port, std::string* path) {
    if (lower(url.substr(0, 7)) != "http://") return false;
    const auto rest = url.substr(7);
    const auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    *path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = hostport.find(':');
    int p = 80;
    if (colon != std::string::npos) { p = std::atoi(hostport.substr(colon + 1).c_str()); hostport = hostport.substr(0, colon); }
    if (hostport.empty() || p <= 0 || p > 65535) return false;
    *host = hostport;
    *port = static_cast<uint16_t>(p);
    return true;
}
std::string join_url(const std::string& base, const std::string& rel) {
    if (lower(rel.substr(0, 7)) == "http://") return rel;
    std::string host, path;
    uint16_t port = 0;
    if (!split_url(base, &host, &port, &path)) return rel;
    const std::string root = "http://" + host + ":" + std::to_string(port);
    if (!rel.empty() && rel[0] == '/') return root + rel;
    const auto last = path.rfind('/');
    return root + path.substr(0, last + 1) + rel;
}
std::string soap_call(const Igd& igd, const std::string& action, const std::string& args, int* status, std::string* log) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + igd.service_type + "\">" +
        args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + igd.service_type + "#" + action + "\"\r\n";
    std::string resp;
    if (!http_request(igd.control_url, "POST", headers, body, status, &resp, log)) return std::string();
    return resp;
}
}  // namespace

std::string ipv4_text(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}

std::string encode_room_code(uint32_t ip, uint16_t port) {
    const uint64_t v = (static_cast<uint64_t>(ip) << 16) | port;   // 48 bits
    std::string s;
    int sum = 0;
    for (int i = 9; i >= 0; --i) {
        const int d = static_cast<int>((v >> (5 * i)) & 31);
        s += kAlphabet[d];
        sum += d * (i + 1);
    }
    s += kAlphabet[sum % 32];
    return s.substr(0, 5) + "-" + s.substr(5);
}

bool decode_peer(const std::string& text_in, uint32_t* ip, uint16_t* port) {
    const std::string text = trim(text_in);
    // a.b.c.d[:port]
    unsigned a, b, c, d, p = kDirectoryPort;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &p, &tail) == 5 ||
        (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && (p = kDirectoryPort))) {
        if (a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535) return false;
        *ip = (a << 24) | (b << 16) | (c << 8) | d;
        *port = static_cast<uint16_t>(p);
        return true;
    }
    std::vector<int> digits;
    for (char ch : text) {
        if (ch == '-' || ch == ' ') continue;
        const int v = b32_value(ch);
        if (v < 0) return false;
        digits.push_back(v);
    }
    if (digits.size() != 11) return false;
    uint64_t v = 0;
    int sum = 0;
    for (int i = 0; i < 10; ++i) { v = (v << 5) | static_cast<uint64_t>(digits[i]); sum += digits[i] * (10 - i); }
    if (digits[10] != sum % 32 || (v >> 48) != 0) return false;
    *ip = static_cast<uint32_t>(v >> 16);
    *port = static_cast<uint16_t>(v & 0xffff);
    return *port != 0 && *ip != 0;
}

std::string peers_to_lobby_arg(const std::string& typed, std::string* rejected) {
    std::string out, cur, bad;
    int kept = 0;
    auto flush = [&] {
        const std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        uint32_t ip = 0;
        uint16_t port = 0;
        if (decode_peer(t, &ip, &port) && kept < 8) {
            if (!out.empty()) out += ',';
            out += ipv4_text(ip) + ":" + std::to_string(port);
            ++kept;
        } else {
            if (!bad.empty()) bad += ", ";
            bad += t;
        }
    };
    for (char ch : typed) {
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r') flush();
        else cur += ch;
    }
    flush();
    if (rejected) *rejected = bad;
    return out;
}

std::string local_ipv4() {
    if (!net_init()) return std::string();
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return std::string();
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(0xC0000201u);   // 192.0.2.1 (TEST-NET-1): only selects the route; nothing is sent
    std::string r;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr != 0) r = ipv4_text(ntohl(me.sin_addr.s_addr));
    }
    close_sock(s);
    return r;
}

bool parse_ssdp_location(const std::string& response, std::string* location) {
    std::string::size_type pos = 0;
    while (pos < response.size()) {
        auto end = response.find('\n', pos);
        if (end == std::string::npos) end = response.size();
        const std::string line = trim(response.substr(pos, end - pos));
        if (lower(line.substr(0, 9)) == "location:") { *location = trim(line.substr(9)); return !location->empty(); }
        pos = end + 1;
    }
    return false;
}

bool parse_igd_description(const std::string& xml, const std::string& location, Igd* out) {
    std::string base;
    find_tag(xml, "URLBase", 0, &base);
    const std::string root = base.empty() ? location : base;
    // Prefer WANIPConnection (any version), then WANPPPConnection.
    for (const char* want : {"urn:schemas-upnp-org:service:wanipconnection:", "urn:schemas-upnp-org:service:wanpppconnection:"}) {
        std::string::size_type pos = 0;
        std::string block;
        while ((pos = find_tag(xml, "service", pos, &block)) != std::string::npos) {
            std::string type, control;
            find_tag(block, "serviceType", 0, &type);
            find_tag(block, "controlURL", 0, &control);
            if (lower(type).rfind(want, 0) == 0 && !control.empty()) {
                out->service_type = type;
                out->control_url = join_url(root, control);
                return true;
            }
        }
    }
    return false;
}

bool parse_soap_value(const std::string& body, const std::string& element, std::string* value) {
    return find_tag(body, element, 0, value) != std::string::npos;
}

bool http_request(const std::string& url, const std::string& method, const std::string& extra_headers, const std::string& body,
                  int* status, std::string* response_body, std::string* log) {
    std::string host, path;
    uint16_t port = 0;
    if (!split_url(url, &host, &port, &path)) { if (log) *log += "not an http:// URL: " + url + "\n"; return false; }
    if (!net_init()) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) { if (log) *log += "router address is not IPv4: " + host + "\n"; return false; }
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return false;
    set_timeout(s, 3000);
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) != 0) {
        if (log) *log += "could not connect to " + host + ":" + std::to_string(port) + "\n";
        close_sock(s);
        return false;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\nConnection: close\r\n" + extra_headers;
    if (method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    for (std::size_t sent = 0; sent < req.size();) {
        const int n = static_cast<int>(send(s, req.data() + sent, static_cast<int>(req.size() - sent), 0));
        if (n <= 0) { close_sock(s); if (log) *log += "send failed\n"; return false; }
        sent += static_cast<std::size_t>(n);
    }
    std::string resp;
    char buf[4096];
    for (;;) {
        const int n = static_cast<int>(recv(s, buf, sizeof buf, 0));
        if (n <= 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
        if (resp.size() > (1u << 20)) break;
    }
    close_sock(s);
    const auto sp = resp.find(' ');
    const auto hdr_end = resp.find("\r\n\r\n");
    if (resp.rfind("HTTP/", 0) != 0 || sp == std::string::npos || hdr_end == std::string::npos) {
        if (log) *log += "no HTTP answer from " + host + "\n";
        return false;
    }
    *status = std::atoi(resp.c_str() + sp + 1);
    std::string payload = resp.substr(hdr_end + 4);
    if (lower(resp.substr(0, hdr_end)).find("transfer-encoding: chunked") != std::string::npos) {   // de-chunk
        std::string plain;
        std::size_t p = 0;
        while (p < payload.size()) {
            const auto eol = payload.find("\r\n", p);
            if (eol == std::string::npos) break;
            const unsigned long n = std::strtoul(payload.substr(p, eol - p).c_str(), nullptr, 16);
            if (n == 0) break;
            plain += payload.substr(eol + 2, n);
            p = eol + 2 + n + 2;
        }
        payload = plain;
    }
    if (response_body) *response_body = payload;
    return true;
}

bool igd_find(Igd* out, std::string* log, const std::string& location_override) {
    std::string location = location_override;
    if (location.empty()) {
        if (!net_init()) return false;
        sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kBad) return false;
        set_timeout(s, 700);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(1900);
        inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
        for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                               "urn:schemas-upnp-org:service:WANIPConnection:1"}) {
            const std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
            sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        }
        for (int i = 0; i < 6 && location.empty(); ++i) {
            char buf[2048];
            const int n = static_cast<int>(recv(s, buf, sizeof buf - 1, 0));
            if (n <= 0) continue;
            buf[n] = 0;
            parse_ssdp_location(buf, &location);
        }
        close_sock(s);
        if (location.empty()) { if (log) *log += "no UPnP router answered on this network (UPnP may be switched off in the router)\n"; return false; }
    }
    int status = 0;
    std::string xml;
    if (!http_request(location, "GET", "", "", &status, &xml, log) || status != 200) {
        if (log) *log += "the router's UPnP description could not be read (" + location + ")\n";
        return false;
    }
    if (!parse_igd_description(xml, location, out)) { if (log) *log += "the router offers no WANIPConnection/WANPPPConnection service\n"; return false; }
    return true;
}

bool igd_external_ip(const Igd& igd, std::string* ip, std::string* log) {
    int status = 0;
    const std::string resp = soap_call(igd, "GetExternalIPAddress", "", &status, log);
    if (status != 200 || !parse_soap_value(resp, "NewExternalIPAddress", ip) || ip->empty()) {
        if (log) *log += "the router did not report its public address (HTTP " + std::to_string(status) + ")\n";
        return false;
    }
    return true;
}

bool igd_add_udp_mapping(const Igd& igd, uint16_t port, const std::string& client_ip, const std::string& description, std::string* log) {
    const std::string p = std::to_string(port);
    const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p + "</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                             "<NewInternalPort>" + p + "</NewInternalPort><NewInternalClient>" + client_ip + "</NewInternalClient>"
                             "<NewEnabled>1</NewEnabled><NewPortMappingDescription>" + description + "</NewPortMappingDescription>"
                             "<NewLeaseDuration>0</NewLeaseDuration>";
    int status = 0;
    const std::string resp = soap_call(igd, "AddPortMapping", args, &status, log);
    if (status != 200) {
        std::string code;
        parse_soap_value(resp, "errorCode", &code);
        if (log) *log += "the router refused to open UDP port " + p + " (HTTP " + std::to_string(status) + (code.empty() ? "" : ", UPnP error " + code) + ")\n";
        return false;
    }
    return true;
}

bool igd_delete_udp_mapping(const Igd& igd, uint16_t port, std::string* log) {
    const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
    int status = 0;
    soap_call(igd, "DeletePortMapping", args, &status, log);
    return status == 200;
}

}  // namespace online
