#ifndef NET_SOCKET_FILTER_H
#define NET_SOCKET_FILTER_H

#include <stdint.h>
#include <netinet/in.h>
#include <linux/filter.h>

namespace net {
  class socket_filter {
    public:
      // Constructor.
      socket_filter();

      // Destructor.
      ~socket_filter();

      // Clear.
      void clear();

      // Enable IPv4.
      void ipv4();

      // Enable IPv6.
      void ipv6();

      // Add port.
      bool port(in_port_t p);

      // Add port range.
      bool port_range(in_port_t from, in_port_t to);

      // Compile.
      bool compile(struct sock_fprog& fprog);

      // Print.
      void print() const;

  private:
    static const size_t max_port_ranges = 32;
    static const size_t max_filters = 255;

    bool _M_ipv4;
    bool _M_ipv6;

    struct portrange {
      in_port_t from;
      in_port_t to;
    };

    portrange _M_portranges[max_port_ranges];
    size_t _M_nportranges;

    struct sock_filter _M_filters[max_filters];
    size_t _M_nfilters;

    // Add socket filter.
    bool stmt(uint16_t code, uint32_t k);
    bool jump(uint16_t code, uint32_t k, uint8_t jt, uint8_t jf);
  };

  inline socket_filter::socket_filter()
  {
    clear();
  }

  inline socket_filter::~socket_filter()
  {
  }

  inline void socket_filter::ipv4()
  {
    _M_ipv4 = true;
  }

  inline void socket_filter::ipv6()
  {
    _M_ipv6 = true;
  }

  inline bool socket_filter::port(in_port_t p)
  {
    return port_range(p, p);
  }

  inline bool socket_filter::stmt(uint16_t code, uint32_t k)
  {
    if (_M_nfilters < max_filters) {
      struct sock_filter* f = _M_filters + _M_nfilters++;

      f->code = code;
      f->k = k;
      f->jt = 0;
      f->jf = 0;

      return true;
    } else {
      return false;
    }
  }

  inline bool socket_filter::jump(uint16_t code,
                                  uint32_t k,
                                  uint8_t jt,
                                  uint8_t jf)
  {
    if (_M_nfilters < max_filters) {
      struct sock_filter* f = _M_filters + _M_nfilters++;

      f->code = code;
      f->k = k;
      f->jt = jt;
      f->jf = jf;

      return true;
    } else {
      return false;
    }
  }
}

#endif // NET_SOCKET_FILTER_H
