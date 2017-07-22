#include <stddef.h>
#include <string.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <limits.h>
#include "net/worker.h"
#include "macros/macros.h"

#define CALCULATE_UDP_CHECKSUM 1

bool net::worker::create(type t,
                         tpacket_versions version,
                         size_t ring_size,
                         unsigned ifindex,
                         const struct sock_fprog* fprog,
                         int fanout,
                         size_t fanout_size,
                         uint16_t fanout_id)
{
  // Create RX ring buffer.
  if (_M_rx.create(version,
                   ring_buffer::type::rx,
                   ring_size,
                   ifindex,
                   fprog,
                   fanout,
                   fanout_size,
                   fanout_id)) {
    _M_ipv4_destinations.init(t);
    _M_ipv6_destinations.init(t);

    return true;
  }

  return false;
}

bool net::worker::add_interface(tpacket_versions version,
                                size_t ring_size,
                                unsigned ifindex,
                                const void* macaddr,
                                const void* addr4,
                                const void* addr6)
{
  // Search interface.
  for (size_t i = 0; i < _M_ninterfaces; i++) {
    if (ifindex == _M_interfaces[i].index) {
      // Already added.
      return true;
    }
  }

  // If there are not too many interfaces...
  if (_M_ninterfaces < max_interfaces) {
    struct interface* iface = _M_interfaces + _M_ninterfaces;

    // Create TX ring buffer.
    if (iface->tx.create(version,
                         ring_buffer::type::tx,
                         ring_size,
                         ifindex,
                         nullptr,
                         0,
                         0,
                         0)) {
      iface->index = ifindex;

      memcpy(iface->macaddr, macaddr, ETHER_ADDR_LEN);
      memcpy(iface->addr4, addr4, sizeof(struct in_addr));
      memcpy(iface->addr6, addr6, sizeof(struct in6_addr));

      _M_ninterfaces++;

      return true;
    }
  }

  return false;
}

bool net::worker::add_destination(unsigned ifindex,
                                  const void* macaddr,
                                  const char* host,
                                  in_port_t port)
{
  uint8_t buf[sizeof(struct in6_addr)];

  // Try first with IPv4.
  if (inet_pton(AF_INET, host, buf) == 1) {
    return add_destination(ifindex,
                           macaddr,
                           buf,
                           static_cast<socklen_t>(sizeof(struct in_addr)),
                           port);
  } else if (inet_pton(AF_INET6, host, buf) == 1) {
    return add_destination(ifindex,
                           macaddr,
                           buf,
                           static_cast<socklen_t>(sizeof(struct in6_addr)),
                           port);
  } else {
    return false;
  }
}

bool net::worker::add_destination(unsigned ifindex,
                                  const void* macaddr,
                                  const void* addr,
                                  socklen_t addrlen,
                                  in_port_t port)
{
  // Search interface.
  for (size_t i = 0; i < _M_ninterfaces; i++) {
    if (ifindex == _M_interfaces[i].index) {
      switch (addrlen) {
        case sizeof(struct in_addr):
          return _M_ipv4_destinations.add(macaddr,
                                          addr,
                                          addrlen,
                                          port,
                                          _M_interfaces + i);
        case sizeof(struct in6_addr):
          return _M_ipv6_destinations.add(macaddr,
                                          addr,
                                          addrlen,
                                          port,
                                          _M_interfaces + i);
        default:
          return false;
      }
    }
  }

  return false;
}

bool net::worker::start()
{
  _M_running = true;

  if (pthread_create(&_M_thread, nullptr, run, this) == 0) {
    return true;
  }

  _M_running = false;

  return false;
}

bool net::worker::destinations::add(const void* macaddr,
                                    const void* addr,
                                    socklen_t addrlen,
                                    in_port_t port,
                                    struct interface* iface)
{
  if (_M_used == _M_size) {
    size_t size = (_M_size > 0) ? _M_size * 2 : 4;

    struct destination* dest;
    if ((dest = reinterpret_cast<struct destination*>(
                  realloc(_M_destinations, size * sizeof(struct destination))
                )) != nullptr) {
      _M_destinations = dest;
      _M_size = size;
    } else {
      return false;
    }
  }

  struct destination* dest = _M_destinations + _M_used++;

  memcpy(dest->macaddr, macaddr, ETHER_ADDR_LEN);

  memcpy(dest->addr, addr, addrlen);
  dest->addrlen = addrlen;

  dest->port = htons(port);

  dest->iface = iface;

  return true;
}

void net::worker::destinations::send_ipv4(struct destination* dest,
                                          const void* pkt,
                                          size_t pktlen)
{
  const uint8_t* ip = reinterpret_cast<const uint8_t*>(pkt) +
                      sizeof(struct ether_header);

  size_t iphdrlen = reinterpret_cast<const struct iphdr*>(ip)->ihl << 2;

  // Sanity checks.
  if (sizeof(struct ether_header) + iphdrlen + sizeof(struct udphdr) <=
      pktlen) {
    const struct udphdr* udphdr = reinterpret_cast<const struct udphdr*>(
                                    ip + iphdrlen
                                  );

    size_t udplen = ntohs(udphdr->len);

    if (sizeof(struct ether_header) + iphdrlen + udplen == pktlen) {
      // Calculate checksum of the IPv4 header.
      uint32_t sum = 0;

      for (size_t i = 0; i < offsetof(struct iphdr, check); i += 2) {
        sum += ntohs(*reinterpret_cast<const uint16_t*>(ip + i));
      }

      // Source address.
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->iface->addr4));
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->iface->addr4 + 2));

      // Destination address.
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->addr));
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->addr + 2));

      // Checksum of the options (if any).
      for (size_t i = sizeof(struct iphdr); i < iphdrlen; i += 2) {
        sum += ntohs(*reinterpret_cast<const uint16_t*>(ip + i));
      }

      while (sum > USHRT_MAX) {
        sum = (sum >> 16) + (sum & 0xffff);
      }

      uint16_t ipv4_checksum = htons(static_cast<uint16_t>(~sum));

      const uint8_t* udpdata = reinterpret_cast<const uint8_t*>(udphdr) +
                               sizeof(struct udphdr);

      size_t udpdatalen = udplen - sizeof(struct udphdr);

#if CALCULATE_UDP_CHECKSUM
      // Calculate UDP checksum (optional for IPv4).

      // Source address.
      sum = ntohs(*reinterpret_cast<const uint16_t*>(dest->iface->addr4));
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->iface->addr4 + 2));

      // Destination address.
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->addr));
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->addr + 2));

      // Protocol.
      sum += IPPROTO_UDP;

      // UDP length.
      sum += udplen;

      // The source port of the packet to be sent is the destination port of the
      // received packet.
      sum += ntohs(udphdr->dest);

      // Destination port.
      sum += ntohs(dest->port);

      // Length.
      sum += udplen;

      // Data.
      for (size_t i = 0; i + 1 < udpdatalen; i += 2) {
        sum += ntohs(*reinterpret_cast<const uint16_t*>(udpdata + i));
      }

      // If the length of the data is odd...
      if ((udpdatalen & 0x01) != 0) {
        sum += ntohs(udpdata[udpdatalen - 1]);
      }

      while (sum > USHRT_MAX) {
        sum = (sum >> 16) + (sum & 0xffff);
      }

      uint16_t udp_checksum = htons(static_cast<uint16_t>(~sum));
#else
      uint16_t udp_checksum = 0;
#endif

      // Compose UDP packet.

      struct iovec vec[] = {
        // Destination ethernet address.
        {dest->macaddr, ETHER_ADDR_LEN},

        // Source ethernet address.
        {dest->iface->macaddr, ETHER_ADDR_LEN},

        // Packet type ID and IPv4 header until IPv4 checksum.
        {const_cast<uint8_t*>(
           reinterpret_cast<const uint8_t*>(pkt) +
           offsetof(struct ether_header, ether_type)
         ),
         2 + offsetof(struct iphdr, check)},

        // Header checksum.
        {&ipv4_checksum, 2},

        // Source IPv4 address.
        {dest->iface->addr4, sizeof(struct in_addr)},

        // Destination IPv4 address.
        {dest->addr, sizeof(struct in_addr)},

        // IPv4 options (if any).
        {const_cast<uint8_t*>(ip) + sizeof(struct iphdr),
         iphdrlen - sizeof(struct iphdr)},

        // Source port (destination port of the received packet).
        {const_cast<uint16_t*>(&udphdr->dest), 2},

        // Destination port.
        {&dest->port, 2},

        // Length.
        {const_cast<uint16_t*>(&udphdr->len), 2},

        // Checksum.
        {&udp_checksum, 2},

        // Data (if present).
        {const_cast<uint8_t*>(udpdata), udpdatalen}
      };

      // Send packet.
      dest->iface->tx.send(vec, ARRAY_SIZE(vec), send_timeout);
    }
  }
}

void net::worker::destinations::send_ipv6(struct destination* dest,
                                          const void* pkt,
                                          size_t pktlen)
{
  const uint8_t* ip = reinterpret_cast<const uint8_t*>(pkt) +
                      sizeof(struct ether_header);

  const struct udphdr* udphdr = reinterpret_cast<const struct udphdr*>(
                                  ip + sizeof(struct ip6_hdr)
                                );

  size_t udplen = ntohs(udphdr->len);

  // Sanity check.
  if (sizeof(struct ether_header) + sizeof(struct ip6_hdr) + udplen == pktlen) {
    // Calculate UDP checksum (mandatory for IPv6).
    uint32_t sum = 0;

    // Source and destination addresses.
    for (size_t i = 0; i < sizeof(struct in6_addr); i += 2) {
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->iface->addr6 + i));
      sum += ntohs(*reinterpret_cast<const uint16_t*>(dest->addr + i));
    }

    // UDP length.
    sum += udplen;

    // Next header.
    sum += IPPROTO_UDP;

    // The source port of the packet to be sent is the destination port of the
    // received packet.
    sum += ntohs(udphdr->dest);

    // Destination port.
    sum += ntohs(dest->port);

    // Length.
    sum += udplen;

    // Data.
    const uint8_t* udpdata = reinterpret_cast<const uint8_t*>(udphdr) +
                             sizeof(struct udphdr);

    size_t udpdatalen = udplen - sizeof(struct udphdr);

    for (size_t i = 0; i + 1 < udpdatalen; i += 2) {
      sum += ntohs(*reinterpret_cast<const uint16_t*>(udpdata + i));
    }

    // If the length of the data is odd...
    if ((udpdatalen & 0x01) != 0) {
      sum += ntohs(udpdata[udpdatalen - 1]);
    }

    while (sum > USHRT_MAX) {
      sum = (sum >> 16) + (sum & 0xffff);
    }

    uint16_t udp_checksum = htons(static_cast<uint16_t>(~sum));

    // Compose UDP packet.

    struct iovec vec[] = {
      // Destination ethernet address.
      {dest->macaddr, ETHER_ADDR_LEN},

      // Source ethernet address.
      {dest->iface->macaddr, ETHER_ADDR_LEN},

      // Packet type ID and IPv6 header until IPv6 source address.
      {const_cast<uint8_t*>(
         reinterpret_cast<const uint8_t*>(pkt) +
         offsetof(struct ether_header, ether_type)
       ),
       2 + offsetof(struct ip6_hdr, ip6_src)},

      // Source IPv6 address.
      {dest->iface->addr6, sizeof(struct in6_addr)},

      // Destination IPv6 address.
      {dest->addr, sizeof(struct in6_addr)},

      // Source port (destination port of the received packet).
      {const_cast<uint16_t*>(&udphdr->dest), 2},

      // Destination port.
      {&dest->port, 2},

      // Length.
      {const_cast<uint16_t*>(&udphdr->len), 2},

      // Checksum.
      {&udp_checksum, 2},

      // Data (if present).
      {const_cast<uint8_t*>(udpdata), udpdatalen}
    };

    // Send packet.
    dest->iface->tx.send(vec, ARRAY_SIZE(vec), send_timeout);
  }
}

void net::worker::run()
{
  static const int timeout = 250; // Milliseconds.

  do {
    _M_rx.recv(timeout);
  } while (_M_running);
}
