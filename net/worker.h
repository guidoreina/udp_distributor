#ifndef NET_WORKER_H
#define NET_WORKER_H

#include <stdlib.h>
#include <pthread.h>
#include <netinet/in.h>
#include <net/ethernet.h>
#include "net/ring_buffer.h"

namespace net {
  class worker {
    public:
      static const size_t max_interfaces = 32;

      enum class type {
        load_balancer,
        broadcaster
      };

      // Constructor.
      worker();

      // Destructor.
      ~worker();

      // Create RX ring buffer.
      bool create(type t,
                  tpacket_versions version,
                  size_t ring_size,
                  unsigned ifindex,
                  const struct sock_fprog* fprog,
                  int fanout,
                  size_t fanout_size,
                  uint16_t fanout_id);

      // Add interface for TX.
      bool add_interface(tpacket_versions version,
                         size_t ring_size,
                         unsigned ifindex,
                         const void* macaddr,
                         const void* addr4,
                         const void* addr6);

      // Add destination.
      bool add_destination(unsigned ifindex,
                           const void* macaddr,
                           const char* host,
                           in_port_t port);

      bool add_destination(unsigned ifindex,
                           const void* macaddr,
                           const void* addr,
                           socklen_t addrlen,
                           in_port_t port);

      // Start.
      bool start();

      // Stop.
      void stop();

      // Receive packet.
      static void fnpacket(const void* pkt, size_t pktlen, void* user);

      // Receive packets.
      static void fnpackets(const struct iovec* pkts, size_t npkts, void* user);

    private:
      static const int send_timeout = 100; // Milliseconds.

      ring_buffer _M_rx;

      struct interface {
        unsigned index;
        uint8_t macaddr[ETHER_ADDR_LEN];

        uint8_t addr4[sizeof(struct in_addr)];
        uint8_t addr6[sizeof(struct in6_addr)];

        ring_buffer tx;
      };

      struct interface _M_interfaces[max_interfaces];
      size_t _M_ninterfaces;

      struct destination {
        uint8_t macaddr[ETHER_ADDR_LEN];

        uint8_t addr[sizeof(struct in6_addr)];
        socklen_t addrlen;

        in_port_t port;

        struct interface* iface;
      };

      enum class family {
        ipv4,
        ipv6
      };

      class destinations {
        public:
          // Constructor.
          destinations(family af);

          // Destructor.
          ~destinations();

          // Initialize.
          void init(type t);

          // Add destination.
          bool add(const void* macaddr,
                   const void* addr,
                   socklen_t addrlen,
                   in_port_t port,
                   struct interface* iface);

          // Process packet.
          void process(const void* pkt, size_t pktlen);

        private:
          struct destination* _M_destinations;
          size_t _M_size;
          size_t _M_used;

          size_t _M_idx;

          typedef void (destinations::*fnprocess)(const void* pkt,
                                                  size_t pktlen);

          typedef void (*fnsend)(struct destination* dest,
                                 const void* pkt,
                                 size_t pktlen);

          fnprocess _M_process;
          fnsend _M_send;

          // Forward packet.
          void forward(const void* pkt, size_t pktlen);

          // Broadcast packet.
          void broadcast(const void* pkt, size_t pktlen);

          // Send packet for IPv4.
          static void send_ipv4(struct destination* dest,
                                const void* pkt,
                                size_t pktlen);

          // Send packet for IPv6.
          static void send_ipv6(struct destination* dest,
                                const void* pkt,
                                size_t pktlen);

          // Disable copy constructor and assignment operator.
          destinations(const destinations&) = delete;
          destinations& operator=(const destinations&) = delete;
      };

      destinations _M_ipv4_destinations;
      destinations _M_ipv6_destinations;

      pthread_t _M_thread;

      bool _M_running;

      // Run.
      static void* run(void* arg);
      void run();

      // Disable copy constructor and assignment operator.
      worker(const worker&) = delete;
      worker& operator=(const worker&) = delete;
  };

  inline worker::worker()
    : _M_ninterfaces(0),
      _M_ipv4_destinations(family::ipv4),
      _M_ipv6_destinations(family::ipv6),
      _M_running(false)
  {
    _M_rx.callbacks(fnpacket, fnpackets, this);
  }

  inline worker::~worker()
  {
    stop();
  }

  inline void worker::stop()
  {
    if (_M_running) {
      _M_running = false;
      pthread_join(_M_thread, nullptr);
    }
  }

  inline void worker::fnpacket(const void* pkt, size_t pktlen, void* user)
  {
    switch (reinterpret_cast<const uint8_t*>(
              pkt
            )[sizeof(struct ether_header)] & 0xf0) {
      case 0x40: // IPv4.
        reinterpret_cast<worker*>(user)->_M_ipv4_destinations.process(pkt,
                                                                      pktlen);

        break;
      case 0x60: // IPv6.
        reinterpret_cast<worker*>(user)->_M_ipv6_destinations.process(pkt,
                                                                      pktlen);

        break;
    }
  }

  inline void worker::fnpackets(const struct iovec* pkts,
                                size_t npkts,
                                void* user)
  {
    // For each packet...
    for (size_t i = 0; i < npkts; i++) {
      fnpacket(pkts[i].iov_base, pkts[i].iov_len, user);
    }
  }

  inline worker::destinations::destinations(family af)
    : _M_destinations(nullptr),
      _M_size(0),
      _M_used(0),
      _M_idx(0),
      _M_send((af == family::ipv4) ? send_ipv4 : send_ipv6)
  {
  }

  inline worker::destinations::~destinations()
  {
    if (_M_destinations) {
      free(_M_destinations);
    }
  }

  inline void worker::destinations::init(type t)
  {
    if (t == type::load_balancer) {
      _M_process = &destinations::forward;
    } else {
      _M_process = &destinations::broadcast;
    }
  }

  inline void worker::destinations::process(const void* pkt, size_t pktlen)
  {
    (this->*_M_process)(pkt, pktlen);
  }

  inline void worker::destinations::forward(const void* pkt, size_t pktlen)
  {
    _M_send(_M_destinations + _M_idx, pkt, pktlen);
    _M_idx = (_M_idx + 1) % _M_used;
  }

  inline void worker::destinations::broadcast(const void* pkt, size_t pktlen)
  {
    for (size_t i = 0; i < _M_used; i++) {
      _M_send(_M_destinations + i, pkt, pktlen);
    }
  }

  inline void* worker::run(void* arg)
  {
    reinterpret_cast<worker*>(arg)->run();
    return nullptr;
  }
}

#endif // NET_WORKER_H
