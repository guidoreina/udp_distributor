#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include "net/socket_filter.h"

void net::socket_filter::clear()
{
  _M_ipv4 = false;
  _M_ipv6 = false;

  _M_nportranges = 0;

  _M_nfilters = 0;
}

bool net::socket_filter::port_range(in_port_t from, in_port_t to)
{
  if ((from > 0) && (from <= to)) {
    size_t i;
    for (i = 0;
         (i < _M_nportranges) &&
         (from > static_cast<size_t>(_M_portranges[i].to) + 1);
         i++);

    // Beyond the last position?
    if (i == _M_nportranges) {
      if (_M_nportranges < max_port_ranges) {
        _M_portranges[i].from = from;
        _M_portranges[i].to = to;

        _M_nportranges++;

        return true;
      } else {
        return false;
      }
    }

    size_t j;
    for (j = i;
         (j + 1 < _M_nportranges) &&
         (static_cast<size_t>(to) + 1 >= _M_portranges[j + 1].from);
         j++);

    if (i == j) {
      if (static_cast<size_t>(to) + 1 < _M_portranges[i].from) {
        if (_M_nportranges < max_port_ranges) {
          memmove(_M_portranges + i + 1,
                  _M_portranges + i,
                  (_M_nportranges - i) * sizeof(struct portrange));

          _M_portranges[i].from = from;
          _M_portranges[i].to = to;

          _M_nportranges++;

          return true;
        }
      } else {
        if (from < _M_portranges[i].from) {
          _M_portranges[i].from = from;
        }

        if (to > _M_portranges[i].to) {
          _M_portranges[i].to = to;
        }

        return true;
      }
    } else {
      if (from < _M_portranges[i].from) {
        _M_portranges[i].from = from;
      }

      _M_portranges[i].to = (to > _M_portranges[j].to) ? to :
                                                         _M_portranges[j].to;

      i++;

      if (++j < _M_nportranges) {
        memmove(_M_portranges + i,
                _M_portranges + j,
                (_M_nportranges - j) * sizeof(struct portrange));
      }

      _M_nportranges -= (j - i);

      return true;
    }
  }

  return false;
}

bool net::socket_filter::compile(struct sock_fprog& fprog)
{
  // Clear filters.
  _M_nfilters = 0;

  if ((!_M_ipv4) && (!_M_ipv6)) {
    _M_ipv4 = true;
    _M_ipv6 = true;
  }

  struct jmp {
    size_t idx;
    bool jt;
  };

  jmp accepts[max_filters];
  size_t naccepts = 0;

  jmp ignores[max_filters];
  size_t nignores = 0;

  static const size_t minlenipv4 = sizeof(struct ether_header) +
                                   sizeof(struct iphdr) +
                                   sizeof(struct udphdr);

  static const size_t minlenipv6 = sizeof(struct ether_header) +
                                   sizeof(struct ip6_hdr) +
                                   sizeof(struct udphdr);

  // Calculate minimum packet length.
  size_t minlen = _M_ipv4 ? minlenipv4 : minlenipv6;

  // A <- len.
  stmt(BPF_LD + BPF_W + BPF_LEN, 0);

  ignores[nignores].idx = _M_nfilters;
  ignores[nignores++].jt = false;

  // Ignore packet if too small.
  jump(BPF_JMP + BPF_JGE + BPF_K, minlen, 0, 0);

  // A <- ethernet type.
  stmt(BPF_LD + BPF_H + BPF_ABS, offsetof(struct ether_header, ether_type));

  size_t next = 0;

  // If there is IPv6...
  if (_M_ipv6) {
    next = _M_nfilters;

    jump(BPF_JMP + BPF_JEQ + BPF_K, ETHERTYPE_IPV6, 0, 0);

    if (_M_ipv4) {
      // A <- len.
      stmt(BPF_LD + BPF_W + BPF_LEN, 0);

      ignores[nignores].idx = _M_nfilters;
      ignores[nignores++].jt = false;

      // Ignore packet if too small.
      jump(BPF_JMP + BPF_JGE + BPF_K, minlenipv6, 0, 0);
    }

    // A <- next header.
    stmt(BPF_LD + BPF_B + BPF_ABS,
         sizeof(struct ether_header) +
         offsetof(struct ip6_hdr, ip6_nxt));

    ignores[nignores].idx = _M_nfilters;
    ignores[nignores++].jt = false;

    jump(BPF_JMP + BPF_JEQ + BPF_K, IPPROTO_UDP, 0, 0);

    // If there are destination ports...
    if (_M_nportranges > 0) {
      // Load destination port.
      stmt(BPF_LD + BPF_H + BPF_ABS,
           sizeof(struct ether_header) +
           sizeof(struct ip6_hdr) +
           offsetof(struct udphdr, dest));

      // Add destination ports.
      for (size_t i = 0; i < _M_nportranges; i++) {
        if (_M_portranges[i].from == _M_portranges[i].to) {
          accepts[naccepts].idx = _M_nfilters;
          accepts[naccepts++].jt = true;

          if (!jump(BPF_JMP + BPF_JEQ + BPF_K, _M_portranges[i].from, 0, 0)) {
            return false;
          }
        } else {
          if (jump(BPF_JMP + BPF_JGE + BPF_K, _M_portranges[i].from, 0, 1)) {
            accepts[naccepts].idx = _M_nfilters;
            accepts[naccepts++].jt = false;

            if (!jump(BPF_JMP + BPF_JGT + BPF_K, _M_portranges[i].to, 0, 0)) {
              return false;
            }
          } else {
            return false;
          }
        }
      }

      // If we are still here, it didn't match the filter.
      if (!stmt(BPF_RET + BPF_K, 0)) {
        return false;
      }
    } else {
      stmt(BPF_RET + BPF_K, 0x40000);
    }
  }

  // If there is IPv4...
  if (_M_ipv4) {
    // If there is IPv6...
    if (next != 0) {
      // Jump here.
      _M_filters[next].jf = _M_nfilters - next - 1;
    }

    ignores[nignores].idx = _M_nfilters;
    ignores[nignores++].jt = false;

    jump(BPF_JMP + BPF_JEQ + BPF_K, ETHERTYPE_IP, 0, 0);

    // A <- protocol.
    stmt(BPF_LD + BPF_B + BPF_ABS,
         sizeof(struct ether_header) +
         offsetof(struct iphdr, protocol));

    ignores[nignores].idx = _M_nfilters;
    ignores[nignores++].jt = false;

    jump(BPF_JMP + BPF_JEQ + BPF_K, IPPROTO_UDP, 0, 0);

    // A <- flags + fragment offset.
    stmt(BPF_LD + BPF_H + BPF_ABS,
         sizeof(struct ether_header) +
         offsetof(struct iphdr, frag_off));

    ignores[nignores].idx = _M_nfilters;
    ignores[nignores++].jt = true;

    // Ignore fragmented packets.
    jump(BPF_JMP + BPF_JSET + BPF_K, 0x3fff, 0, 0);

    // If there are destination ports...
    if (_M_nportranges > 0) {
      // Load destination port.
      stmt(BPF_LD + BPF_H + BPF_ABS,
           sizeof(struct ether_header) +
           sizeof(struct iphdr) +
           offsetof(struct udphdr, dest));

      // Add destination ports.
      for (size_t i = 0; i < _M_nportranges; i++) {
        if (_M_portranges[i].from == _M_portranges[i].to) {
          accepts[naccepts].idx = _M_nfilters;
          accepts[naccepts++].jt = true;

          if (!jump(BPF_JMP + BPF_JEQ + BPF_K, _M_portranges[i].from, 0, 0)) {
            return false;
          }
        } else {
          if (jump(BPF_JMP + BPF_JGE + BPF_K, _M_portranges[i].from, 0, 1)) {
            accepts[naccepts].idx = _M_nfilters;
            accepts[naccepts++].jt = false;

            if (!jump(BPF_JMP + BPF_JGT + BPF_K, _M_portranges[i].to, 0, 0)) {
              return false;
            }
          } else {
            return false;
          }
        }
      }
    } else {
      stmt(BPF_RET + BPF_K, 0x40000);
    }
  }

  for (size_t i = 0; i < nignores; i++) {
    if (ignores[i].jt) {
      _M_filters[ignores[i].idx].jt = _M_nfilters - ignores[i].idx - 1;
    } else {
      _M_filters[ignores[i].idx].jf = _M_nfilters - ignores[i].idx - 1;
    }
  }

  if (!stmt(BPF_RET + BPF_K, 0)) {
    return false;
  }

  for (size_t i = 0; i < naccepts; i++) {
    if (accepts[i].jt) {
      _M_filters[accepts[i].idx].jt = _M_nfilters - accepts[i].idx - 1;
    } else {
      _M_filters[accepts[i].idx].jf = _M_nfilters - accepts[i].idx - 1;
    }
  }

  if (stmt(BPF_RET + BPF_K, 0x40000)) {
    fprog.filter = _M_filters;
    fprog.len = static_cast<unsigned short>(_M_nfilters);

    return true;
  }

  return false;
}

void net::socket_filter::print() const
{
  static const int widths[] = {8, 16};

  for (size_t i = 0; i < _M_nfilters; i++) {
    const sock_filter* f = _M_filters + i;

    printf("(%03zu) ", i);

    switch (f->code) {
      case BPF_LD | BPF_W | BPF_LEN:
        printf("%-*s #pktlen\n", widths[0], "ld");
        break;
      case BPF_LD | BPF_W | BPF_ABS:
        printf("%-*s [%u]\n", widths[0], "ld", f->k);
        break;
      case BPF_LD | BPF_H | BPF_ABS:
        printf("%-*s [%u]\n", widths[0], "ldh", f->k);
        break;
      case BPF_LD | BPF_B | BPF_ABS:
        printf("%-*s [%u]\n", widths[0], "ldb", f->k);
        break;
      case BPF_JMP | BPF_JGE | BPF_K:
        printf("%-*s #0x%-*x%s%-3zu%s%zu\n",
               widths[0],
               "jge",
               widths[1],
               f->k,
               "jt ",
               i + 1 + f->jt,
               "jf ",
               i + 1 + f->jf);

        break;
      case BPF_JMP | BPF_JEQ | BPF_K:
        printf("%-*s #0x%-*x%s%-3zu%s%zu\n",
               widths[0],
               "jeq",
               widths[1],
               f->k,
               "jt ",
               i + 1 + f->jt,
               "jf ",
               i + 1 + f->jf);

        break;
      case BPF_JMP | BPF_JGT | BPF_K:
        printf("%-*s #0x%-*x%s%-3zu%s%zu\n",
               widths[0],
               "jgt",
               widths[1],
               f->k,
               "jt ",
               i + 1 + f->jt,
               "jf ",
               i + 1 + f->jf);

        break;
      case BPF_JMP | BPF_JSET | BPF_K:
        printf("%-*s #0x%-*x%s%-3zu%s%zu\n",
               widths[0],
               "jset",
               widths[1],
               f->k,
               "jt ",
               i + 1 + f->jt,
               "jf ",
               i + 1 + f->jf);

        break;
      case BPF_RET | BPF_K:
        printf("%-*s #%u\n", widths[0], "ret", f->k);
        break;
    }
  }
}
