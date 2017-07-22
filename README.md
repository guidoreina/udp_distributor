UDP load balancer / broadcaster
===============================
UDP load balancer / broadcaster for Linux using packet mmap.

It attaches to a network interface (reception interface) for receiving UDP datagrams and can distribute them to several destinations using one or more interfaces (transmission interfaces).

Usage:
```
Usage: ./udp_distributor <parameters>

Parameters:
  [Mandatory] --rx <interface-name>[,<ring-size>]
    Ring size in bytes, KiB (K), MiB (M) or GiB (G)
    (1 MB .. 16 GB, default: 256 MB)

  [Mandatory] --tx <interface-name>,<mac-address>,<ipv4-address>,<ipv6-address>[,<ring-size>]
    <mac-address> ::= <hex><hex>:<hex><hex>:<hex><hex>:<hex><hex>:<hex><hex>:<hex><hex>

  [Mandatory] --dest <interface-name>,<mac-address>,<ip-address>,<port>

  [Optional] --type "load-balancer" | "broadcaster" (default: "load-balancer")

  [Optional] --ports <port-definition>[,<port-definition>]*
    <port-definition> ::= <port>|<port-range>
    <port> ::= 1 .. 65535
    <port-range> ::= <port>"-"<port>

  [Optional] --number-workers <number-workers> (1 .. 32, default: 1)

```

Parameters:
* `--rx <interface-name>[,<ring-size>]`
    - `<interface-name>` is the reception interface.
    - `<ring-size>` is the size of the ring buffer (optional, default: 256 MB).

  This parameter is mandatory.

  Examples:
    - `--rx eth0`
    - `--rx eth1,16M`

* `--tx <interface-name>,<mac-address>,<ipv4-address>,<ipv6-address>[,<ring-size>]`
    - `<interface-name>` is the transmission interface.
    - `<mac-address>` is the MAC address of the interface, which will be used as source MAC address.
    - `<ipv4-address>` is the IPv4 address of the interface, which will be used as source IPv4 address.
    - `<ipv6-address>` is the IPv6 address of the interface, which will be used as source IPv6 address.
    - `<ring-size>` is the size of the ring buffer (optional, default: 256 MB).

  This parameter is mandatory and can appear several times.

  Example:
    - `--tx eth1,00:01:02:03:04:05,192.168.0.1,2001:83:e21:f282:95b7:d3e0:e3c8:eb30,32M`

* `--dest <interface-name>,<mac-address>,<ip-address>,<port>`
    - `<interface-name>` is the interface used for transmission (it should be one defined with the parameter `--tx`).
    - `<mac-address>` is the MAC address of the destination, which will be used as destination MAC address.
    - `<ip-address>` is the IP address of the destination (either IPv4 or IPv6).
    - `<port>` is the port of the destination.

  This parameter is mandatory and can appear several times.

  Example:
    - `--dest eth1,aa:bb:cc:dd:ee:ff,192.168.0.2,2000`

* `--type "load-balancer" | "broadcaster"`

  Should the packets be load balanced or sent to all destinations?

  This parameter is optional. When not specified, `load-balancer` is assumed.

* `--ports <port-definition>[,<port-definition>]*`

  List of reception ports. Only the packets which come to one of these ports will be processed.

  This parameter is optional and can appear several times. When not specified, all the ports are assumed.

  Examples
    - `--ports 3000,4000-5000,6000`

* `--number-workers <number-workers>`

  Number of worker threads.

  This parameter is optional. When not specified, `1` is assumed.
