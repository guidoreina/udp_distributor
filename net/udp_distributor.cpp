#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "net/udp_distributor.h"

bool net::udp_distributor::create(type t,
                                  size_t ring_size,
                                  unsigned ifindex,
                                  const struct sock_fprog* fprog,
                                  int fanout,
                                  size_t nworkers)
{
  // Sanity checks.
  if ((ring_size >= ring_buffer::min_size) &&
      (ring_size <= ring_buffer::max_size) &&
      (ifindex > 0) &&
      (nworkers >= min_workers) &&
      (nworkers <= max_workers)) {
    uint16_t fanout_id = static_cast<uint16_t>(getpid() & 0xffff);

    // For each worker...
    for (size_t i = 0; i < nworkers; i++) {
      // Create worker.
      if (!_M_workers[i].create(t,
                                TPACKET_V3,
                                ring_size,
                                ifindex,
                                fprog,
                                fanout,
                                nworkers,
                                fanout_id)) {
        return false;
      }
    }

    _M_type = t;

    _M_nworkers = nworkers;

    return true;
  }

  return false;
}

bool net::udp_distributor::add_interface(size_t ring_size,
                                         unsigned ifindex,
                                         const void* macaddr,
                                         const void* addr4,
                                         const void* addr6)
{
  // Sanity checks.
  if ((ring_size >= ring_buffer::min_size) &&
      (ring_size <= ring_buffer::max_size) &&
      (ifindex > 0)) {
    // For each worker...
    for (size_t i = 0; i < _M_nworkers; i++) {
      // Add interface.
      if (!_M_workers[i].add_interface(TPACKET_V2,
                                       ring_size,
                                       ifindex,
                                       macaddr,
                                       addr4,
                                       addr6)) {
        return false;
      }
    }

    return true;
  }

  return false;
}

bool net::udp_distributor::add_destination(unsigned ifindex,
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

bool net::udp_distributor::add_destination(unsigned ifindex,
                                           const void* macaddr,
                                           const void* addr,
                                           socklen_t addrlen,
                                           in_port_t port)
{
  // Sanity check.
  if (ifindex > 0) {
    if (_M_type == type::load_balancer) {
      // Add destination.
      if (_M_workers[_M_idx].add_destination(ifindex,
                                             macaddr,
                                             addr,
                                             addrlen,
                                             port)) {
        _M_idx = (_M_idx + 1) % _M_nworkers;

        return true;
      }
    } else {
      // For each worker...
      for (size_t i = 0; i < _M_nworkers; i++) {
        // Add destination.
        if (!_M_workers[i].add_destination(ifindex,
                                           macaddr,
                                           addr,
                                           addrlen,
                                           port)) {
          return false;
        }
      }

      return true;
    }
  }

  return false;
}

bool net::udp_distributor::start()
{
  // For each worker...
  for (size_t i = 0; i < _M_nworkers; i++) {
    // Start.
    if (!_M_workers[i].start()) {
      return false;
    }
  }

  return true;
}
