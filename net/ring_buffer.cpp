#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/if_ether.h>
#include <arpa/inet.h>
#include "net/ring_buffer.h"

void net::ring_buffer::clear()
{
  if (_M_buf != MAP_FAILED) {
    munmap(_M_buf, _M_ring_size);
    _M_buf = MAP_FAILED;
  }

  if (_M_fd != -1) {
    close(_M_fd);
    _M_fd = -1;
  }

  if (_M_rx_frames) {
    free(_M_rx_frames);
    _M_rx_frames = nullptr;
  }

  if (_M_tx_frames) {
    free(_M_tx_frames);
    _M_tx_frames = nullptr;
  }

  _M_rx_idx = 0;
  _M_tx_idx = 0;
}

bool net::ring_buffer::create(tpacket_versions version,
                              type t,
                              size_t ring_size,
                              unsigned ifindex,
                              const struct sock_fprog* fprog,
                              int fanout,
                              size_t fanout_size,
                              uint16_t fanout_id)
{
  if ((ring_size >= min_size) && (ring_size <= max_size) && (ifindex > 0)) {
    if ((setup_socket(version, t)) &&
        (setup_ring(version, t, ring_size)) &&
        (mmap_ring(t)) &&
        (bind_ring(ifindex, fprog))) {
      if ((t != type::tx) && (fanout_size > 0)) {
        // Create fanout group.
        int optval = (fanout << 16) | fanout_id;

        if (setsockopt(_M_fd,
                       SOL_PACKET,
                       PACKET_FANOUT,
                       &optval,
                       sizeof(int)) < 0) {
          return false;
        }
      }

      _M_version = version;
      _M_type = t;

      return true;
    }
  }

  return false;
}

bool net::ring_buffer::show_statistics()
{
  if (_M_version == TPACKET_V3) {
    struct tpacket_stats_v3 stats;
    socklen_t optlen = static_cast<socklen_t>(sizeof(struct tpacket_stats_v3));

    if (getsockopt(_M_fd,
                   SOL_PACKET,
                   PACKET_STATISTICS,
                   &stats,
                   &optlen) == 0) {
      printf("%u packets received.\n", stats.tp_packets);
      printf("%u packets dropped by kernel.\n", stats.tp_drops);

      return true;
    }
  } else {
    struct tpacket_stats stats;
    socklen_t optlen = static_cast<socklen_t>(sizeof(struct tpacket_stats));

    if (getsockopt(_M_fd,
                   SOL_PACKET,
                   PACKET_STATISTICS,
                   &stats,
                   &optlen) == 0) {
      printf("%u packets received.\n", stats.tp_packets);
      printf("%u packets dropped by kernel.\n", stats.tp_drops);

      return true;
    }
  }

  return false;
}

bool net::ring_buffer::setup_socket(tpacket_versions version, type t)
{
  // Create socket.
  if ((_M_fd = socket(PF_PACKET, SOCK_RAW, 0)) != -1) {
    if (t != type::rx) {
      int optval = 1;
      if (setsockopt(_M_fd,
                     SOL_PACKET,
                     PACKET_QDISC_BYPASS,
                     &optval,
                     sizeof(int)) < 0) {
        return false;
      }
    }

    // Set packet version.
    int optval = static_cast<int>(version);
    return (setsockopt(_M_fd,
                       SOL_PACKET,
                       PACKET_VERSION,
                       &optval,
                       sizeof(int)) == 0);
  }

  return false;
}

bool net::ring_buffer::setup_ring(tpacket_versions version,
                                  type t,
                                  size_t ring_size)
{
  union {
    struct tpacket_req req;
    struct tpacket_req3 req3;
  } req;

  socklen_t optlen;

  if (version == TPACKET_V3) {
    config_v3(t, ring_size, req.req3);

    optlen = static_cast<socklen_t>(sizeof(struct tpacket_req3));
  } else {
    if ((t != type::rx) && (!discard_packet_loss())) {
      return false;
    }

    config_v1_v2(version, ring_size, req.req);

    optlen = static_cast<socklen_t>(sizeof(struct tpacket_req));
  }

  switch (t) {
    case type::rx:
      return (setsockopt(_M_fd,
                         SOL_PACKET,
                         PACKET_RX_RING,
                         &req,
                         optlen) == 0);
    case type::tx:
      return (setsockopt(_M_fd,
                         SOL_PACKET,
                         PACKET_TX_RING,
                         &req,
                         optlen) == 0);
    case type::rxtx:
      return ((setsockopt(_M_fd,
                          SOL_PACKET,
                          PACKET_RX_RING,
                          &req,
                          optlen) == 0) &&
              (setsockopt(_M_fd,
                          SOL_PACKET,
                          PACKET_TX_RING,
                          &req,
                          optlen) == 0));
    default:
      return false;
  }
}

bool net::ring_buffer::mmap_ring(type t)
{
  size_t size = (t != type::rxtx) ? _M_ring_size : 2 * _M_ring_size;

  // Map ring into memory.
  if ((_M_buf = mmap(nullptr,
                     size,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_LOCKED | MAP_POPULATE,
                     _M_fd,
                     0)) != MAP_FAILED) {
    // Allocate frames.

    if (t != type::tx) {
      if ((_M_rx_frames = reinterpret_cast<struct iovec*>(
                            malloc(_M_count * sizeof(struct iovec))
                          )) != nullptr) {
        uint8_t* buf = reinterpret_cast<uint8_t*>(_M_buf);

        for (size_t i = 0; i < _M_count; i++) {
          _M_rx_frames[i].iov_base = buf;
          _M_rx_frames[i].iov_len = _M_size;

          buf += _M_size;
        }
      } else {
        return false;
      }
    }

    if (t != type::rx) {
      if ((_M_tx_frames = reinterpret_cast<struct iovec*>(
                            malloc(_M_count * sizeof(struct iovec))
                          )) != nullptr) {
        uint8_t* buf = reinterpret_cast<uint8_t*>(_M_buf);

        if (t == type::rxtx) {
          buf += _M_ring_size;
        }

        for (size_t i = 0; i < _M_count; i++) {
          _M_tx_frames[i].iov_base = buf;
          _M_tx_frames[i].iov_len = _M_size;

          buf += _M_size;
        }
      } else {
        return false;
      }
    }

    _M_ring_size = size;

    return true;
  }

  return false;
}

bool net::ring_buffer::bind_ring(unsigned ifindex,
                                 const struct sock_fprog* fprog)
{
  if (fprog) {
    if (setsockopt(_M_fd,
                   SOL_SOCKET,
                   SO_ATTACH_FILTER,
                   fprog,
                   sizeof(struct sock_fprog)) < 0) {
      return false;
    }
  }

  // Bind.
  struct sockaddr_ll addr;
  memset(&addr, 0, sizeof(struct sockaddr_ll));
  addr.sll_family = PF_PACKET;
  addr.sll_protocol = htons(ETH_P_ALL);
  addr.sll_ifindex = ifindex;

  return (bind(_M_fd,
               reinterpret_cast<struct sockaddr*>(&addr),
               static_cast<socklen_t>(sizeof(struct sockaddr_ll))) == 0);
}

void net::ring_buffer::config_v1_v2(tpacket_versions version,
                                    size_t ring_size,
                                    struct tpacket_req& req)
{
  size_t block_size = getpagesize() << 2;
  _M_frame_size = TPACKET_ALIGNMENT << 7;

  // Calculate number of blocks.
  size_t nblocks = ring_size / block_size;

  _M_ring_size = nblocks * block_size;
  _M_nframes = _M_ring_size / _M_frame_size;

  memset(&req, 0, sizeof(struct tpacket_req));

  req.tp_block_nr = nblocks;
  req.tp_block_size = block_size;
  req.tp_frame_nr = _M_nframes;
  req.tp_frame_size = _M_frame_size;

  _M_count = _M_nframes;
  _M_size = _M_frame_size;

  if (version == TPACKET_V1) {
    _M_recv = &ring_buffer::recv_v1;
    _M_send = &ring_buffer::send_v1;
    _M_sendv = &ring_buffer::sendv_v1;
    _M_sendmmsg = &ring_buffer::sendmmsg_v1;
  } else {
    _M_recv = &ring_buffer::recv_v2;
    _M_send = &ring_buffer::send_v2;
    _M_sendv = &ring_buffer::sendv_v2;
    _M_sendmmsg = &ring_buffer::sendmmsg_v2;
  }
}

void net::ring_buffer::config_v3(type t,
                                 size_t ring_size,
                                 struct tpacket_req3& req)
{
  size_t block_size = getpagesize() << 2;
  _M_frame_size = TPACKET_ALIGNMENT << 7;

  // Calculate number of blocks.
  size_t nblocks = ring_size / block_size;

  _M_ring_size = nblocks * block_size;
  _M_nframes = _M_ring_size / _M_frame_size;

  memset(&req, 0, sizeof(struct tpacket_req3));

  req.tp_block_nr = nblocks;
  req.tp_block_size = block_size;
  req.tp_frame_nr = _M_nframes;
  req.tp_frame_size = _M_frame_size;

  if (t != type::tx) {
    req.tp_retire_blk_tov = 64;
    req.tp_feature_req_word = TP_FT_REQ_FILL_RXHASH;
  }

  _M_count = nblocks;
  _M_size = block_size;

  _M_recv = &ring_buffer::recv_v3;
  _M_send = &ring_buffer::send_v3;
  _M_sendv = &ring_buffer::sendv_v3;
  _M_sendmmsg = &ring_buffer::sendmmsg_v3;
}

bool net::ring_buffer::discard_packet_loss()
{
  // Discard packet loss.
  int optval = 1;
  return (setsockopt(_M_fd,
                     SOL_PACKET,
                     PACKET_LOSS,
                     &optval,
                     sizeof(int)) == 0);
}

bool net::ring_buffer::recv_v1()
{
  struct tpacket_hdr* hdr = reinterpret_cast<struct tpacket_hdr*>(
                              _M_rx_frames[_M_rx_idx].iov_base
                            );

  // If there is a new packet...
  if ((hdr->tp_status & TP_STATUS_USER) == TP_STATUS_USER) {
    // Process packet.
    _M_fnpacket(reinterpret_cast<const uint8_t*>(hdr) + hdr->tp_mac,
                hdr->tp_snaplen,
                _M_user);

    // Mark frame as free.
    hdr->tp_status = TP_STATUS_KERNEL;

    __sync_synchronize();

    _M_rx_idx = (_M_rx_idx + 1) % _M_count;

    return true;
  } else {
    return false;
  }
}

bool net::ring_buffer::recv_v2()
{
  struct tpacket2_hdr* hdr = reinterpret_cast<struct tpacket2_hdr*>(
                               _M_rx_frames[_M_rx_idx].iov_base
                             );

  // If there is a new packet...
  if ((hdr->tp_status & TP_STATUS_USER) == TP_STATUS_USER) {
    // Process packet.
    _M_fnpacket(reinterpret_cast<const uint8_t*>(hdr) + hdr->tp_mac,
                hdr->tp_snaplen,
                _M_user);

    // Mark frame as free.
    hdr->tp_status = TP_STATUS_KERNEL;

    __sync_synchronize();

    _M_rx_idx = (_M_rx_idx + 1) % _M_count;

    return true;
  } else {
    return false;
  }
}

bool net::ring_buffer::recv_v3()
{
  static const size_t max_pkts = 1024;

  struct tpacket_block_desc* block_desc =
                             reinterpret_cast<struct tpacket_block_desc*>(
                               _M_rx_frames[_M_rx_idx].iov_base
                             );

  // If there is a new block...
  if ((block_desc->hdr.bh1.block_status & TP_STATUS_USER) != 0) {
    struct tpacket3_hdr* hdr = reinterpret_cast<struct tpacket3_hdr*>(
                                 reinterpret_cast<uint8_t*>(block_desc) +
                                 block_desc->hdr.bh1.offset_to_first_pkt
                               );

    struct iovec pkts[max_pkts];
    size_t npkts = 0;

    uint32_t num_pkts = block_desc->hdr.bh1.num_pkts;

    // Process packets in the block.
    for (uint32_t i = 0; i < num_pkts; i++) {
      if (npkts == max_pkts) {
        // Process packets.
        _M_fnpackets(pkts, npkts, _M_user);

        npkts = 0;
      }

      pkts[npkts].iov_base = reinterpret_cast<uint8_t*>(hdr) + hdr->tp_mac;
      pkts[npkts++].iov_len = hdr->tp_snaplen;

      hdr = reinterpret_cast<struct tpacket3_hdr*>(
              reinterpret_cast<uint8_t*>(hdr) + hdr->tp_next_offset
            );

      __sync_synchronize();
    }

    // Process packets.
    _M_fnpackets(pkts, npkts, _M_user);

    // Mark block as free.
    block_desc->hdr.bh1.block_status = TP_STATUS_KERNEL;

    __sync_synchronize();

    _M_rx_idx = (_M_rx_idx + 1) % _M_count;

    return true;
  } else {
    return false;
  }
}

bool net::ring_buffer::send_v1(const void* pkt, size_t pktlen)
{
  struct tpacket_hdr* hdr = reinterpret_cast<struct tpacket_hdr*>(
                              _M_tx_frames[_M_tx_idx].iov_base
                            );

  // If there is a packet available...
  if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
    // Set packet length.
    hdr->tp_snaplen = pktlen;
    hdr->tp_len = pktlen;

    // Copy packet.
    memcpy(reinterpret_cast<uint8_t*>(hdr) +
           TPACKET_HDRLEN -
           sizeof(struct sockaddr_ll),
           pkt,
           pktlen);

    // Mark packet as ready to be sent.
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    __sync_synchronize();

    _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

    return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
  } else {
    errno = EAGAIN;
    return false;
  }
}

bool net::ring_buffer::sendv_v1(const struct iovec* iov, size_t iovcnt)
{
  struct tpacket_hdr* hdr = reinterpret_cast<struct tpacket_hdr*>(
                              _M_tx_frames[_M_tx_idx].iov_base
                            );

  // If there is a packet available...
  if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
    // Copy packet.
    uint8_t* begin = reinterpret_cast<uint8_t*>(hdr) +
                     TPACKET_HDRLEN -
                     sizeof(struct sockaddr_ll);

    uint8_t* p = begin;

    for (size_t i = 0; i < iovcnt; i++, iov++) {
      memcpy(p, iov->iov_base, iov->iov_len);
      p += iov->iov_len;
    }

    size_t pktlen = p - begin;

    // Set packet length.
    hdr->tp_snaplen = pktlen;
    hdr->tp_len = pktlen;

    // Mark packet as ready to be sent.
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    __sync_synchronize();

    _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

    return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
  } else {
    errno = EAGAIN;
    return false;
  }
}

bool net::ring_buffer::sendmmsg_v1(const struct iovec* pkts,
                                   size_t npkts,
                                   int timeout)
{
  // For each packet...
  for (size_t i = 0; i < npkts; i++, pkts++) {
    struct tpacket_hdr* hdr = reinterpret_cast<struct tpacket_hdr*>(
                                _M_tx_frames[_M_tx_idx].iov_base
                              );

    do {
      // If there is a packet available...
      if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
        // Set packet length.
        hdr->tp_snaplen = pkts->iov_len;
        hdr->tp_len = pkts->iov_len;

        // Copy packet.
        memcpy(reinterpret_cast<uint8_t*>(hdr) +
               TPACKET_HDRLEN -
               sizeof(struct sockaddr_ll),
               pkts->iov_base,
               pkts->iov_len);

        // Mark packet as ready to be sent.
        hdr->tp_status = TP_STATUS_SEND_REQUEST;

        __sync_synchronize();

        _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

        break;
      } else if (!wait_writable(timeout)) {
        errno = EAGAIN;
        return false;
      }
    } while (true);
  }

  return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
}

bool net::ring_buffer::send_v2(const void* pkt, size_t pktlen)
{
  struct tpacket2_hdr* hdr = reinterpret_cast<struct tpacket2_hdr*>(
                               _M_tx_frames[_M_tx_idx].iov_base
                             );

  // If there is a packet available...
  if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
    // Set packet length.
    hdr->tp_snaplen = pktlen;
    hdr->tp_len = pktlen;

    // Copy packet.
    memcpy(reinterpret_cast<uint8_t*>(hdr) +
           TPACKET2_HDRLEN -
           sizeof(struct sockaddr_ll),
           pkt,
           pktlen);

    // Mark packet as ready to be sent.
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    __sync_synchronize();

    _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

    return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
  } else {
    errno = EAGAIN;
    return false;
  }
}

bool net::ring_buffer::sendv_v2(const struct iovec* iov, size_t iovcnt)
{
  struct tpacket2_hdr* hdr = reinterpret_cast<struct tpacket2_hdr*>(
                               _M_tx_frames[_M_tx_idx].iov_base
                             );

  // If there is a packet available...
  if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
    // Copy packet.
    uint8_t* begin = reinterpret_cast<uint8_t*>(hdr) +
                     TPACKET2_HDRLEN -
                     sizeof(struct sockaddr_ll);

    uint8_t* p = begin;

    for (size_t i = 0; i < iovcnt; i++, iov++) {
      memcpy(p, iov->iov_base, iov->iov_len);
      p += iov->iov_len;
    }

    size_t pktlen = p - begin;

    // Set packet length.
    hdr->tp_snaplen = pktlen;
    hdr->tp_len = pktlen;

    // Mark packet as ready to be sent.
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    __sync_synchronize();

    _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

    return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
  } else {
    errno = EAGAIN;
    return false;
  }
}

bool net::ring_buffer::sendmmsg_v2(const struct iovec* pkts,
                                   size_t npkts,
                                   int timeout)
{
  // For each packet...
  for (size_t i = 0; i < npkts; i++, pkts++) {
    struct tpacket2_hdr* hdr = reinterpret_cast<struct tpacket2_hdr*>(
                                 _M_tx_frames[_M_tx_idx].iov_base
                               );

    do {
      // If there is a packet available...
      if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
        // Set packet length.
        hdr->tp_snaplen = pkts->iov_len;
        hdr->tp_len = pkts->iov_len;

        // Copy packet.
        memcpy(reinterpret_cast<uint8_t*>(hdr) +
               TPACKET2_HDRLEN -
               sizeof(struct sockaddr_ll),
               pkts->iov_base,
               pkts->iov_len);

        // Mark packet as ready to be sent.
        hdr->tp_status = TP_STATUS_SEND_REQUEST;

        __sync_synchronize();

        _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

        break;
      } else if (!wait_writable(timeout)) {
        errno = EAGAIN;
        return false;
      }
    } while (true);
  }

  return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
}

bool net::ring_buffer::send_v3(const void* pkt, size_t pktlen)
{
  struct tpacket3_hdr* hdr = reinterpret_cast<struct tpacket3_hdr*>(
                               reinterpret_cast<uint8_t*>(
                                 _M_tx_frames[0].iov_base
                               ) +
                               (_M_tx_idx * _M_frame_size)
                             );

  // If there is a packet available...
  if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
    // Set packet length.
    hdr->tp_snaplen = pktlen;
    hdr->tp_len = pktlen;
    hdr->tp_next_offset = 0;

    // Copy packet.
    memcpy(reinterpret_cast<uint8_t*>(hdr) +
           TPACKET3_HDRLEN -
           sizeof(struct sockaddr_ll),
           pkt,
           pktlen);

    // Mark packet as ready to be sent.
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    __sync_synchronize();

    _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

    return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
  } else {
    errno = EAGAIN;
    return false;
  }
}

bool net::ring_buffer::sendv_v3(const struct iovec* iov, size_t iovcnt)
{
  struct tpacket3_hdr* hdr = reinterpret_cast<struct tpacket3_hdr*>(
                               reinterpret_cast<uint8_t*>(
                                 _M_tx_frames[0].iov_base
                               ) +
                               (_M_tx_idx * _M_frame_size)
                             );

  // If there is a packet available...
  if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
    // Copy packet.
    uint8_t* begin = reinterpret_cast<uint8_t*>(hdr) +
                     TPACKET3_HDRLEN -
                     sizeof(struct sockaddr_ll);

    uint8_t* p = begin;

    for (size_t i = 0; i < iovcnt; i++, iov++) {
      memcpy(p, iov->iov_base, iov->iov_len);
      p += iov->iov_len;
    }

    size_t pktlen = p - begin;

    // Set packet length.
    hdr->tp_snaplen = pktlen;
    hdr->tp_len = pktlen;
    hdr->tp_next_offset = 0;

    // Mark packet as ready to be sent.
    hdr->tp_status = TP_STATUS_SEND_REQUEST;

    __sync_synchronize();

    _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

    return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
  } else {
    errno = EAGAIN;
    return false;
  }
}

bool net::ring_buffer::sendmmsg_v3(const struct iovec* pkts,
                                   size_t npkts,
                                   int timeout)
{
  // For each packet...
  for (size_t i = 0; i < npkts; i++, pkts++) {
    struct tpacket3_hdr* hdr = reinterpret_cast<struct tpacket3_hdr*>(
                                 reinterpret_cast<uint8_t*>(
                                   _M_tx_frames[0].iov_base
                                 ) +
                                 (_M_tx_idx * _M_frame_size)
                               );

    do {
      // If there is a packet available...
      if (!(hdr->tp_status & (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING))) {
        // Set packet length.
        hdr->tp_snaplen = pkts->iov_len;
        hdr->tp_len = pkts->iov_len;
        hdr->tp_next_offset = 0;

        // Copy packet.
        memcpy(reinterpret_cast<uint8_t*>(hdr) +
               TPACKET3_HDRLEN -
               sizeof(struct sockaddr_ll),
               pkts->iov_base,
               pkts->iov_len);

        // Mark packet as ready to be sent.
        hdr->tp_status = TP_STATUS_SEND_REQUEST;

        __sync_synchronize();

        _M_tx_idx = (_M_tx_idx + 1) % _M_nframes;

        break;
      } else if (!wait_writable(timeout)) {
        errno = EAGAIN;
        return false;
      }
    } while (true);
  }

  return (sendto(_M_fd, nullptr, 0, 0, nullptr, 0) != -1);
}

bool net::ring_buffer::wait_readable(int timeout)
{
  struct pollfd pfd;
  pfd.fd = _M_fd;
  pfd.events = POLLIN | POLLERR;
  pfd.revents = 0;

  return (poll(&pfd, 1, timeout) == 1);
}

bool net::ring_buffer::wait_writable(int timeout)
{
  struct pollfd pfd;
  pfd.fd = _M_fd;
  pfd.events = POLLOUT | POLLERR;
  pfd.revents = 0;

  return (poll(&pfd, 1, timeout) == 1);
}
