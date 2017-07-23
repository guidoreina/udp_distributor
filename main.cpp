#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <arpa/inet.h>
#include "net/udp_distributor.h"
#include "net/socket_filter.h"
#include "macros/macros.h"

struct reception {
  unsigned ifindex;
  size_t ring_size;
};

struct interface {
  size_t ring_size;

  char name[IF_NAMESIZE];
  unsigned ifindex;

  uint8_t macaddr[ETHER_ADDR_LEN];
  uint8_t addr4[sizeof(struct in_addr)];
  uint8_t addr6[sizeof(struct in6_addr)];
};

struct destination {
  unsigned ifindex;

  uint8_t macaddr[ETHER_ADDR_LEN];

  uint8_t addr[sizeof(struct in6_addr)];
  socklen_t addrlen;

  in_port_t port;
};

static void usage(const char* program);

static bool parse_reception(const char* s, struct reception& reception);
static bool parse_interface(const char* s, struct interface& interface);
static bool parse_destination(const char* s,
                              const struct interface* interfaces,
                              size_t ninterfaces,
                              struct destination& dest);

static bool parse_port_list(const char* s, net::socket_filter& filter);
static bool parse_interface_name(const char* s, size_t len, unsigned& ifindex);
static bool parse_mac_address(const char* s, size_t len, uint8_t* macaddr);
static bool parse_ipv4_address(const char* s, size_t len, uint8_t* addr);
static bool parse_ipv6_address(const char* s, size_t len, uint8_t* addr);
static bool parse_address(const char* s,
                          size_t len,
                          uint8_t* addr,
                          socklen_t& addrlen);

static bool parse_size(const char* s, size_t min, size_t max, size_t& size);
static bool parse_number(const char* s,
                         uint64_t min,
                         uint64_t max,
                         uint64_t& n);

static int hex2bin(char c);

int main(int argc, const char** argv)
{
  net::udp_distributor::type type = net::udp_distributor::type::load_balancer;

  struct reception reception;
  reception.ifindex = 0;

  struct interface interfaces[net::worker::max_interfaces];
  size_t ninterfaces = 0;

  size_t ndests = 0;

  net::socket_filter filter;

  size_t nworkers = net::udp_distributor::default_workers;

  int i = 1;

  while (i < argc) {
    if (strcasecmp(argv[i], "--rx") == 0) {
      // If not the last argument...
      if (i + 1 < argc) {
        if (parse_reception(argv[i + 1], reception)) {
          i += 2;
        } else {
          return -1;
        }
      } else {
        usage(argv[0]);
        return -1;
      }
    } else if (strcasecmp(argv[i], "--tx") == 0) {
      // If not the last argument...
      if (i + 1 < argc) {
        if (ninterfaces < net::worker::max_interfaces) {
          if (parse_interface(argv[i + 1], interfaces[ninterfaces])) {
            ninterfaces++;

            i += 2;
          } else {
            return -1;
          }
        } else {
          fprintf(stderr,
                  "Cannot define more interfaces (%zu).\n",
                  ninterfaces);

          return -1;
        }
      } else {
        usage(argv[0]);
        return -1;
      }
    } else if (strcasecmp(argv[i], "--dest") == 0) {
      // If not the last argument...
      if (i + 1 < argc) {
        // Process it later.
        ndests++;

        i += 2;
      } else {
        usage(argv[0]);
        return -1;
      }
    } else if (strcasecmp(argv[i], "--type") == 0) {
      // If not the last argument...
      if (i + 1 < argc) {
        if (strcasecmp(argv[i + 1], "load-balancer") == 0) {
          type = net::udp_distributor::type::load_balancer;
        } else if (strcasecmp(argv[i + 1], "broadcaster") == 0) {
          type = net::udp_distributor::type::broadcaster;
        } else {
          fprintf(stderr, "Invalid type '%s'.\n", argv[i + 1]);
          return -1;
        }

        i += 2;
      } else {
        usage(argv[0]);
        return -1;
      }
    } else if (strcasecmp(argv[i], "--ports") == 0) {
      // If not the last argument...
      if (i + 1 < argc) {
        if (parse_port_list(argv[i + 1], filter)) {
          i += 2;
        } else {
          fprintf(stderr, "Invalid port list or too many ports defined.\n");
          return -1;
        }
      } else {
        usage(argv[0]);
        return -1;
      }
    } else if (strcasecmp(argv[i], "--number-workers") == 0) {
      // If not the last argument...
      if (i + 1 < argc) {
        if (parse_number(argv[i + 1],
                         net::udp_distributor::min_workers,
                         net::udp_distributor::max_workers,
                         nworkers)) {
          i += 2;
        } else {
          fprintf(stderr, "Invalid number of workers '%s'.\n", argv[i + 1]);
          return -1;
        }
      } else {
        usage(argv[0]);
        return -1;
      }
    } else {
      usage(argv[0]);
      return -1;
    }
  }

  if ((reception.ifindex > 0) && (ninterfaces > 0) && (ndests > 0)) {
    struct sock_fprog fprog;
    if (filter.compile(fprog)) {
      // Check destinations.
      i = 1;

      while (i < argc) {
        if (strcasecmp(argv[i], "--dest") == 0) {
          // Just check if the destination is valid.
          struct destination dest;
          if (!parse_destination(argv[i + 1],
                                 interfaces,
                                 ninterfaces,
                                 dest)) {
            return -1;
          }
        }

        i += 2;
      }

      // Block signals SIGINT and SIGTERM.
      sigset_t set;
      sigemptyset(&set);
      sigaddset(&set, SIGINT);
      sigaddset(&set, SIGTERM);
      if (pthread_sigmask(SIG_BLOCK, &set, NULL) == 0) {
        if ((nworkers > ndests) &&
            (type == net::udp_distributor::type::load_balancer)) {
          nworkers = ndests;
        }

        // Create UDP distributor.
        net::udp_distributor udp_distributor;
        if (udp_distributor.create(type,
                                   reception.ring_size,
                                   reception.ifindex,
                                   &fprog,
                                   PACKET_FANOUT_HASH,
                                   nworkers)) {
          // Add interfaces.
          for (size_t i = 0; i < ninterfaces; i++) {
            if (!udp_distributor.add_interface(interfaces[i].ring_size,
                                               interfaces[i].ifindex,
                                               interfaces[i].macaddr,
                                               interfaces[i].addr4,
                                               interfaces[i].addr6)) {
              fprintf(stderr,
                      "Error adding interface '%s'.\n",
                      interfaces[i].name);

              return -1;
            }
          }

          // Add destinations.
          i = 1;

          while (i < argc) {
            if (strcasecmp(argv[i], "--dest") == 0) {
              struct destination dest;
              parse_destination(argv[i + 1], interfaces, ninterfaces, dest);

              // Add destination.
              if (!udp_distributor.add_destination(dest.ifindex,
                                                   dest.macaddr,
                                                   dest.addr,
                                                   dest.addrlen,
                                                   dest.port)) {
                fprintf(stderr, "Error adding destination.\n");
                return -1;
              }
            }

            i += 2;
          }

          // Start UDP distributor.
          if (udp_distributor.start()) {
            // Wait for signal to arrive.
            int sig;
            while (sigwait(&set, &sig) != 0);

            udp_distributor.stop();

            printf("Exiting...\n");

            return 0;
          } else {
            fprintf(stderr, "Error starting UDP distributor.\n");
          }
        } else {
          fprintf(stderr, "Error creating UDP distributor.\n");
        }
      } else {
        fprintf(stderr, "Error blocking signals SIGINT and SIGTERM.\n");
      }
    } else {
      fprintf(stderr, "Error compiling socket filter.\n");
    }
  } else {
    usage(argv[0]);
  }

  return -1;
}

void usage(const char* program)
{
  fprintf(stderr, "Usage: %s <parameters>\n", program);
  fprintf(stderr, "\n");

  fprintf(stderr, "Parameters:\n");

  fprintf(stderr,
          "  [Mandatory] --rx <interface-name>[,<ring-size>]\n"
          "    Ring size in bytes, KiB (K), MiB (M) or GiB (G)\n"
          "    (%llu MB .. %llu GB, default: %llu MB)\n",
          net::ring_buffer::min_size / (1024ULL * 1024ULL),
          net::ring_buffer::max_size / (1024ULL * 1024ULL * 1024ULL),
          net::ring_buffer::default_size / (1024ULL * 1024ULL));

  fprintf(stderr, "\n");

  fprintf(stderr,
          "  [Mandatory] --tx <interface-name>,<mac-address>,<ipv4-address>,"
          "<ipv6-address>[,<ring-size>]\n"
          "    <mac-address> ::= <hex><hex>:<hex><hex>:<hex><hex>:<hex><hex>:"
          "<hex><hex>:<hex><hex>\n");

  fprintf(stderr, "\n");

  fprintf(stderr,
          "  [Mandatory] --dest <interface-name>,<mac-address>,<ip-address>,"
          "<port>\n");

  fprintf(stderr, "\n");

  fprintf(stderr,
          "  [Optional] --type \"load-balancer\" | \"broadcaster\" "
          "(default: \"load-balancer\")\n");

  fprintf(stderr, "\n");

  fprintf(stderr,
          "  [Optional] --ports <port-definition>[,<port-definition>]*\n"
          "    <port-definition> ::= <port>|<port-range>\n"
          "    <port> ::= 1 .. 65535\n"
          "    <port-range> ::= <port>\"-\"<port>\n");

  fprintf(stderr, "\n");

  fprintf(stderr,
          "  [Optional] --number-workers <number-workers> "
          "(%zu .. %zu, default: %zu)\n",
          net::udp_distributor::min_workers,
          net::udp_distributor::max_workers,
          net::udp_distributor::default_workers);

  fprintf(stderr, "\n");
}

bool parse_reception(const char* s, struct reception& reception)
{
  // Format:
  // <interface-name>[,<ring-size>]

  const char* ptr;
  if ((ptr = strchr(s, ',')) != nullptr) {
    if (parse_interface_name(s, ptr - s, reception.ifindex)) {
      if (parse_size(ptr + 1,
                     net::ring_buffer::min_size,
                     net::ring_buffer::max_size,
                     reception.ring_size)) {
        return true;
      }
    }
  } else {
    if (parse_interface_name(s, strlen(s), reception.ifindex)) {
      reception.ring_size = net::ring_buffer::default_size;
      return true;
    }
  }

  fprintf(stderr, "Invalid reception definition '%s'.\n", s);

  return false;
}

bool parse_interface(const char* s, struct interface& interface)
{
  // Format:
  // <interface-name>,<mac-address>,<ipv4-address>,<ipv6-address>[,<ring-size>]

  const char* const begin = s;

  const char* ptr;
  if ((ptr = strchr(s, ',')) != nullptr) {
    size_t len = ptr - s;
    if (parse_interface_name(s, len, interface.ifindex)) {
      // Save name.
      memcpy(interface.name, s, len);
      interface.name[len] = 0;

      s = ptr + 1;

      if ((ptr = strchr(s, ',')) != nullptr) {
        if (parse_mac_address(s, ptr - s, interface.macaddr)) {
          s = ptr + 1;

          if ((ptr = strchr(s, ',')) != nullptr) {
            if (parse_ipv4_address(s, ptr - s, interface.addr4)) {
              s = ptr + 1;

              if ((ptr = strchr(s, ',')) != nullptr) {
                if (parse_ipv6_address(s, ptr - s, interface.addr6)) {
                  if (parse_size(ptr + 1,
                                 net::ring_buffer::min_size,
                                 net::ring_buffer::max_size,
                                 interface.ring_size)) {
                    return true;
                  }
                }
              } else {
                if (parse_ipv6_address(s, strlen(s), interface.addr6)) {
                  interface.ring_size = net::ring_buffer::default_size;
                  return true;
                }
              }
            }
          }
        }
      }
    }
  }

  fprintf(stderr, "Invalid interface definition '%s'.\n", begin);

  return false;
}

bool parse_destination(const char* s,
                       const struct interface* interfaces,
                       size_t ninterfaces,
                       struct destination& dest)
{
  // Format:
  // <interface-name>,<mac-address>,<ip-address>,<port>

  const char* const begin = s;

  const char* ptr;
  if ((ptr = strchr(s, ',')) != nullptr) {
    size_t len = ptr - s;
    if (parse_interface_name(s, len, dest.ifindex)) {
      // Search interface.
      for (size_t i = 0; i < ninterfaces; i++) {
        if (dest.ifindex == interfaces[i].ifindex) {
          s = ptr + 1;

          if ((ptr = strchr(s, ',')) != nullptr) {
            if (parse_mac_address(s, ptr - s, dest.macaddr)) {
              s = ptr + 1;

              if ((ptr = strchr(s, ',')) != nullptr) {
                if (parse_address(s, ptr - s, dest.addr, dest.addrlen)) {
                  uint64_t port;
                  if (parse_number(ptr + 1, 1, 65535, port)) {
                    dest.port = static_cast<in_port_t>(port);
                    return true;
                  }
                }
              }
            }
          }

          fprintf(stderr, "Invalid destination definition '%s'.\n", begin);
          return false;
        }
      }

      fprintf(stderr,
              "Interface '%.*s' not defined in the interface list.\n",
              static_cast<int>(len),
              begin);

      return false;
    }
  }

  fprintf(stderr, "Invalid destination definition '%s'.\n", begin);

  return false;
}

bool parse_port_list(const char* s, net::socket_filter& filter)
{
  unsigned from = 0;
  unsigned to = 0;

  int state = 0; // Initial state.

  while (*s) {
    switch (state) {
      case 0: // Initial state.
        if (IS_DIGIT(*s)) {
          from = *s - '0';

          state = 1; // Parsing start port.
        } else {
          return false;
        }

        break;
      case 1: // Parsing start port.
        if (IS_DIGIT(*s)) {
          if ((from = (from * 10) + (*s - '0')) > 65535) {
            return false;
          }
        } else {
          switch (*s) {
            case ',':
              to = from;

              state = 4; // After port definition.
              break;
            case '-':
              state = 2; // Before end port.
              break;
            default:
              return false;
          }
        }

        break;
      case 2: // Before end port.
        if (IS_DIGIT(*s)) {
          to = *s - '0';

          state = 3; // Parsing end port.
        } else {
          return false;
        }

        break;
      case 3: // Parsing end port.
        if (IS_DIGIT(*s)) {
          if ((to = (to * 10) + (*s - '0')) > 65535) {
            return false;
          }
        } else if (*s == ',') {
          state = 4; // After port definition.
        } else {
          return false;
        }

        break;
      case 4: // After port definition.
        if ((IS_DIGIT(*s)) && (filter.port_range(from, to))) {
          from = *s - '0';

          state = 1; // Parsing start port.
        } else {
          return false;
        }

        break;
    }

    s++;
  }

  switch (state) {
    case 0: // Initial state.
    case 2: // Before end port.
    case 4: // After port definition.
      return false;
    case 1: // Parsing start port.
      return filter.port(from);
    case 3: // Parsing end port.
      return filter.port_range(from, to);
    default:
      return false;
  }
}

bool parse_interface_name(const char* s, size_t len, unsigned& ifindex)
{
  if ((len > 0) && (len < IF_NAMESIZE)) {
    char name[IF_NAMESIZE];
    memcpy(name, s, len);
    name[len] = 0;

    if ((ifindex = if_nametoindex(name)) > 0) {
      return true;
    }

    fprintf(stderr, "Interface '%s' not found.\n", name);
  } else {
    fprintf(stderr,
            "Invalid interface name '%.*s'.\n",
            static_cast<int>(len),
            s);
  }

  return false;
}

bool parse_mac_address(const char* s, size_t len, uint8_t* macaddr)
{
  if ((len == 17) &&
      (IS_XDIGIT(s[0])) &&
      (IS_XDIGIT(s[1])) &&
      (s[2] == ':') &&
      (IS_XDIGIT(s[3])) &&
      (IS_XDIGIT(s[4])) &&
      (s[5] == ':') &&
      (IS_XDIGIT(s[6])) &&
      (IS_XDIGIT(s[7])) &&
      (s[8] == ':') &&
      (IS_XDIGIT(s[9])) &&
      (IS_XDIGIT(s[10])) &&
      (s[11] == ':') &&
      (IS_XDIGIT(s[12])) &&
      (IS_XDIGIT(s[13])) &&
      (s[14] == ':') &&
      (IS_XDIGIT(s[15])) &&
      (IS_XDIGIT(s[16]))) {
    static const size_t offsets[] = {0, 3, 6, 9, 12, 15};

    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
      macaddr[i] = static_cast<uint8_t>(
                     (hex2bin(s[offsets[i]]) << 4) |
                     (hex2bin(s[offsets[i] + 1]))
                   );
    }

    return true;
  }

  fprintf(stderr, "Invalid MAC address '%.*s'.\n", static_cast<int>(len), s);

  return false;
}

bool parse_ipv4_address(const char* s, size_t len, uint8_t* addr)
{
  if ((len > 0) && (len < INET_ADDRSTRLEN)) {
    char host[INET_ADDRSTRLEN];
    memcpy(host, s, len);
    host[len] = 0;

    if (inet_pton(AF_INET, host, addr) == 1) {
      return true;
    }
  }

  fprintf(stderr, "Invalid IPv4 address '%.*s'.\n", static_cast<int>(len), s);

  return false;
}

bool parse_ipv6_address(const char* s, size_t len, uint8_t* addr)
{
  if ((len > 0) && (len < INET6_ADDRSTRLEN)) {
    char host[INET6_ADDRSTRLEN];
    memcpy(host, s, len);
    host[len] = 0;

    if (inet_pton(AF_INET6, host, addr) == 1) {
      return true;
    }
  }

  fprintf(stderr, "Invalid IPv6 address '%.*s'.\n", static_cast<int>(len), s);

  return false;
}

bool parse_address(const char* s, size_t len, uint8_t* addr, socklen_t& addrlen)
{
  if ((len > 0) && (len < INET6_ADDRSTRLEN)) {
    char host[INET6_ADDRSTRLEN];
    memcpy(host, s, len);
    host[len] = 0;

    // Try first with IPv4.
    if (inet_pton(AF_INET, host, addr) == 1) {
      addrlen = static_cast<socklen_t>(sizeof(struct in_addr));
      return true;
    } else if (inet_pton(AF_INET6, host, addr) == 1) {
      addrlen = static_cast<socklen_t>(sizeof(struct in6_addr));
      return true;
    }
  }

  fprintf(stderr, "Invalid IP address '%.*s'.\n", static_cast<int>(len), s);

  return false;
}

bool parse_size(const char* s, size_t min, size_t max, size_t& size)
{
  const char* const begin = s;

  uint64_t res = 0;

  while (*s) {
    if (IS_DIGIT(*s)) {
      uint64_t tmp;
      if ((tmp = (res * 10) + (*s - '0')) >= res) {
        res = tmp;

        s++;
      } else {
        // Overflow.
        return false;
      }
    } else if (*s == 'K') {
      uint64_t tmp;
      if ((s > begin) &&
          ((tmp = res * 1024ULL) >= res) &&
          (!*(s + 1)) &&
          (tmp >= min) &&
          (tmp <= max)) {
        size = static_cast<size_t>(tmp);
        return true;
      }

      return false;
    } else if (*s == 'M') {
      uint64_t tmp;
      if ((s > begin) &&
          ((tmp = res * (1024ULL * 1024ULL)) >= res) &&
          (!*(s + 1)) &&
          (tmp >= min) &&
          (tmp <= max)) {
        size = static_cast<size_t>(tmp);
        return true;
      }

      return false;
    } else if (*s == 'G') {
      uint64_t tmp;
      if ((s > begin) &&
          ((tmp = res * (1024ULL * 1024ULL * 1024ULL)) >= res) &&
          (!*(s + 1)) &&
          (tmp >= min) &&
          (tmp <= max)) {
        size = static_cast<size_t>(tmp);
        return true;
      }

      return false;
    } else {
      return false;
    }
  }

  if ((s > begin) && (res >= min) && (res <= max)) {
    size = static_cast<size_t>(res);
    return true;
  }

  return false;
}

bool parse_number(const char* s, uint64_t min, uint64_t max, uint64_t& n)
{
  const char* const begin = s;

  uint64_t res = 0;

  while (*s) {
    if (IS_DIGIT(*s)) {
      uint64_t tmp;
      if ((tmp = (res * 10) + (*s - '0')) >= res) {
        res = tmp;

        s++;
      } else {
        // Overflow.
        return false;
      }
    } else {
      return false;
    }
  }

  if ((s > begin) && (res >= min) && (res <= max)) {
    n = res;
    return true;
  }

  return false;
}

int hex2bin(char c)
{
  switch (c) {
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return (c - '0');
    case 'a':
    case 'b':
    case 'c':
    case 'd':
    case 'e':
    case 'f':
      return (c - 'a' + 10);
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'E':
    case 'F':
      return (c - 'A' + 10);
    default:
      return -1;
  }
}
