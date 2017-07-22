#ifndef NET_UDP_DISTRIBUTOR_H
#define NET_UDP_DISTRIBUTOR_H

#include "net/worker.h"

namespace net {
  class udp_distributor {
    public:
      static const size_t min_workers = 1;
      static const size_t max_workers = 32;
      static const size_t default_workers = 1;

      typedef worker::type type;

      // Constructor.
      udp_distributor();

      // Destructor.
      ~udp_distributor();

      // Create.
      bool create(type t,
                  size_t ring_size,
                  unsigned ifindex,
                  const struct sock_fprog* fprog,
                  int fanout,
                  size_t nworkers);

      // Add interface for TX.
      bool add_interface(size_t ring_size,
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

    private:
      type _M_type;

      worker _M_workers[max_workers];
      size_t _M_nworkers;

      size_t _M_idx;

      // Disable copy constructor and assignment operator.
      udp_distributor(const udp_distributor&) = delete;
      udp_distributor& operator=(const udp_distributor&) = delete;
  };

  inline udp_distributor::udp_distributor()
    : _M_nworkers(0),
      _M_idx(0)
  {
  }

  inline udp_distributor::~udp_distributor()
  {
    stop();
  }

  inline void udp_distributor::stop()
  {
    // Stop workers.
    for (size_t i = 0; i < _M_nworkers; i++) {
      _M_workers[i].stop();
    }
  }
}

#endif // NET_UDP_DISTRIBUTOR_H
