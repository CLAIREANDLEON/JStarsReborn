// JStarsReborn launcher - online helpers: room codes and UPnP port mapping (docs/ADR-007-online.md).
//
// Online play is peer-to-peer: the two players' runtimes talk directly. Nothing here contacts a server. A host makes its
// two UDP ports reachable (UPnP on the router, or a port forwarded by hand) and gives its room code to the other player;
// the code is just the host's public address and directory port, written so it is easy to read out and type.
#pragma once
#include <cstdint>
#include <string>

namespace online {

constexpr uint16_t kDirectoryPort = 47391;   // the runtime's room directory (--lobby-port)
constexpr uint16_t kGamePort = 3658;         // the port the game binds for its own P2P traffic (seen in run 20261102-013701)

// "ABCDE-FGHJKM": Crockford base32 of the IPv4 address and port (48 bits, 10 characters) plus one check character.
std::string encode_room_code(uint32_t ip_host_order, uint16_t port);
// Accepts a room code (case, spaces and dashes ignored; O/I/L read as 0/1/1) or "a.b.c.d[:port]".
bool decode_peer(const std::string& text, uint32_t* ip_host_order, uint16_t* port);
std::string ipv4_text(uint32_t ip_host_order);
// Turns what the player typed (codes or addresses, separated by commas/spaces) into "ip:port,ip:port" for --lobby-peer.
// Entries that are not valid are dropped and reported in *rejected (if given).
std::string peers_to_lobby_arg(const std::string& typed, std::string* rejected = nullptr);

// The IPv4 address of the interface this PC uses to reach other machines (no packet is sent). Empty if none.
std::string local_ipv4();

// ---- UPnP Internet Gateway Device (the router) ------------------------------------------------------------------------
struct Igd {
    std::string control_url;    // absolute http:// URL of the WANIPConnection / WANPPPConnection control endpoint
    std::string service_type;   // e.g. urn:schemas-upnp-org:service:WANIPConnection:1
};
// Parsing, separately testable.
bool parse_ssdp_location(const std::string& response, std::string* location);
bool parse_igd_description(const std::string& xml, const std::string& location, Igd* out);
bool parse_soap_value(const std::string& body, const std::string& element, std::string* value);
// Network operations. All return false with a readable reason in *log on failure; timeouts are short.
bool http_request(const std::string& url, const std::string& method, const std::string& extra_headers, const std::string& body,
                  int* status, std::string* response_body, std::string* log);
bool igd_find(Igd* out, std::string* log, const std::string& location_override = "");
bool igd_external_ip(const Igd& igd, std::string* ip, std::string* log);
bool igd_add_udp_mapping(const Igd& igd, uint16_t port, const std::string& client_ip, const std::string& description, std::string* log);
bool igd_delete_udp_mapping(const Igd& igd, uint16_t port, std::string* log);

}  // namespace online
