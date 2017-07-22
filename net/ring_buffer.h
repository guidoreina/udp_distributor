#ifndef NET_RING_BUFFER_H
#define NET_RING_BUFFER_H

#include <stdint.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <errno.h>

namespace net {
  class ring_buffer {
    public:
      enum class type {
        rx,
        tx,
        rxtx
      };

      static const size_t min_size = 1024 * 1024; // 1 MB.

#if __WORDSIZE == 64
      static const size_t max_size = 16ULL *
                                     1024ULL *
                                     1024ULL *
                                     1024ULL; // 16 GB.
#else
      static const size_t max_size = 1024 * 1024 * 1024; // 1 GB.
#endif

      static const size_t default_size = 256 * 1024 * 1024; // 256 MB.

      typedef void (*fnpacket_t)(const void* pkt, size_t pktlen, void* user);
      typedef void (*fnpackets_t)(const struct iovec* pkts,
                                  size_t npkts,
                                  void* user);

      // Constructor.
      ring_buffer();

      // Destructor.
      ~ring_buffer();

      // Clear.
      void clear();

      // Create.
      bool create(tpacket_versions version,
                  type t,
                  size_t ring_size,
                  const char* interface,
                  const struct sock_fprog* fprog,
                  int fanout,
                  size_t fanout_size,
                  uint16_t fanout_id);

      bool create(tpacket_versions version,
                  type t,
                  size_t ring_size,
                  unsigned ifindex,
                  const struct sock_fprog* fprog,
                  int fanout,
                  size_t fanout_size,
                  uint16_t fanout_id);

      // Receive packet.
      bool recv(int timeout);

      // Send packet.
      bool send(const void* pkt, size_t pktlen, int timeout);

      // Send packet.
      bool send(const struct iovec* iov, size_t iovcnt, int timeout);

      // Send packets.
      bool sendmmsg(const struct iovec* pkts, size_t npkts, int timeout);

      // Set callbacks.
      void callbacks(fnpacket_t fnpacket, fnpackets_t fnpackets, void* user);

      // Show statistics.
      bool show_statistics();

    private:
      tpacket_versions _M_version;
      type _M_type;

      int _M_fd;

      void* _M_buf;
      size_t _M_ring_size;

      // For TPACKET_V1 / TPACKET_V2:
      //   _M_count = req.tp_frame_nr
      //   _M_size = req.tp_frame_size
      //
      // For TPACKET_V3:
      //   _M_count = req3.tp_block_nr
      //   _M_size = req3.tp_block_size
      size_t _M_count;
      size_t _M_size;

      size_t _M_nframes;
      size_t _M_frame_size;

      struct iovec* _M_rx_frames;
      struct iovec* _M_tx_frames;

      size_t _M_rx_idx;
      size_t _M_tx_idx;

      typedef bool (ring_buffer::*fnrecv)(int timeout);
      typedef bool (ring_buffer::*fnsend)(const void* pkt,
                                          size_t pktlen,
                                          int timeout);

      typedef bool (ring_buffer::*fnsendv)(const struct iovec* iov,
                                           size_t iovcnt,
                                           int timeout);

      typedef bool (ring_buffer::*fnsendmmsg)(const struct iovec* pkts,
                                              size_t npkts,
                                              int timeout);

      fnrecv _M_recv;
      fnsend _M_send;
      fnsendv _M_sendv;
      fnsendmmsg _M_sendmmsg;

      fnpacket_t _M_fnpacket;
      fnpackets_t _M_fnpackets;
      void* _M_user;

      // Set up socket.
      bool setup_socket(tpacket_versions version, type t);

      // Set up packet ring.
      bool setup_ring(tpacket_versions version, type t, size_t ring_size);

      // Set up mmap packet ring.
      bool mmap_ring(type t);

      // Bind packet ring.
      bool bind_ring(unsigned ifindex, const struct sock_fprog* fprog);

      // Configure for TPACKET_V1 or TPACKET_V2.
      void config_v1_v2(tpacket_versions version,
                        size_t ring_size,
                        struct tpacket_req& req);

      // Configure for TPACKET_V3.
      void config_v3(type t, size_t ring_size, struct tpacket_req3& req);

      // Discard packet loss.
      bool discard_packet_loss();

      // Receive packet for TPACKET_V1.
      bool recv_v1();
      bool recv_v1(int timeout);

      // Receive packet for TPACKET_V2.
      bool recv_v2();
      bool recv_v2(int timeout);

      // Receive packet for TPACKET_V3.
      bool recv_v3();
      bool recv_v3(int timeout);

      // Send packet for TPACKET_V1.
      bool send_v1(const void* pkt, size_t pktlen);
      bool send_v1(const void* pkt, size_t pktlen, int timeout);

      bool sendv_v1(const struct iovec* iov, size_t iovcnt);
      bool sendv_v1(const struct iovec* iov, size_t iovcnt, int timeout);

      // Send packets for TPACKET_V1.
      bool sendmmsg_v1(const struct iovec* pkts, size_t npkts, int timeout);

      // Send packet for TPACKET_V2.
      bool send_v2(const void* pkt, size_t pktlen);
      bool send_v2(const void* pkt, size_t pktlen, int timeout);

      bool sendv_v2(const struct iovec* iov, size_t iovcnt);
      bool sendv_v2(const struct iovec* iov, size_t iovcnt, int timeout);

      // Send packets for TPACKET_V2.
      bool sendmmsg_v2(const struct iovec* pkts, size_t npkts, int timeout);

      // Send packet for TPACKET_V3.
      bool send_v3(const void* pkt, size_t pktlen);
      bool send_v3(const void* pkt, size_t pktlen, int timeout);

      bool sendv_v3(const struct iovec* iov, size_t iovcnt);
      bool sendv_v3(const struct iovec* iov, size_t iovcnt, int timeout);

      // Send packets for TPACKET_V3.
      bool sendmmsg_v3(const struct iovec* pkts, size_t npkts, int timeout);

      // Wait readable.
      bool wait_readable(int timeout);

      // Wait writable.
      bool wait_writable(int timeout);

      // Disable copy constructor and assignment operator.
      ring_buffer(const ring_buffer&) = delete;
      ring_buffer& operator=(const ring_buffer&) = delete;
  };

  inline ring_buffer::ring_buffer()
    : _M_fd(-1),
      _M_buf(MAP_FAILED),
      _M_rx_frames(nullptr),
      _M_tx_frames(nullptr),
      _M_rx_idx(0),
      _M_tx_idx(0),
      _M_fnpacket(nullptr),
      _M_fnpackets(nullptr),
      _M_user(nullptr)
  {
  }

  inline ring_buffer::~ring_buffer()
  {
    clear();
  }

  inline bool ring_buffer::create(tpacket_versions version,
                                  type t,
                                  size_t ring_size,
                                  const char* interface,
                                  const struct sock_fprog* fprog,
                                  int fanout,
                                  size_t fanout_size,
                                  uint16_t fanout_id)
  {
    return create(version,
                  t,
                  ring_size,
                  if_nametoindex(interface),
                  fprog,
                  fanout,
                  fanout_size,
                  fanout_id);
  }

  inline bool ring_buffer::recv(int timeout)
  {
    return (this->*_M_recv)(timeout);
  }

  inline bool ring_buffer::send(const void* pkt, size_t pktlen, int timeout)
  {
    return (this->*_M_send)(pkt, pktlen, timeout);
  }

  inline bool ring_buffer::send(const struct iovec* iov,
                                size_t iovcnt,
                                int timeout)
  {
    return (this->*_M_sendv)(iov, iovcnt, timeout);
  }

  inline bool ring_buffer::sendmmsg(const struct iovec* pkts,
                                    size_t npkts,
                                    int timeout)
  {
    return (this->*_M_sendmmsg)(pkts, npkts, timeout);
  }

  inline void ring_buffer::callbacks(fnpacket_t fnpacket,
                                     fnpackets_t fnpackets,
                                     void* user)
  {
    _M_fnpacket = fnpacket;
    _M_fnpackets = fnpackets;

    _M_user = user;
  }

  inline bool ring_buffer::recv_v1(int timeout)
  {
    return ((recv_v1()) || ((wait_readable(timeout)) && (recv_v1())));
  }

  inline bool ring_buffer::recv_v2(int timeout)
  {
    return ((recv_v2()) || ((wait_readable(timeout)) && (recv_v2())));
  }

  inline bool ring_buffer::recv_v3(int timeout)
  {
    return ((recv_v3()) || ((wait_readable(timeout)) && (recv_v3())));
  }

  inline bool ring_buffer::send_v1(const void* pkt, size_t pktlen, int timeout)
  {
    return ((send_v1(pkt, pktlen)) ||
            ((errno == EAGAIN) &&
             (wait_writable(timeout)) &&
             (send_v1(pkt, pktlen))));
  }

  inline bool ring_buffer::send_v2(const void* pkt, size_t pktlen, int timeout)
  {
    return ((send_v2(pkt, pktlen)) ||
            ((errno == EAGAIN) &&
             (wait_writable(timeout)) &&
             (send_v2(pkt, pktlen))));
  }

  inline bool ring_buffer::send_v3(const void* pkt, size_t pktlen, int timeout)
  {
    return ((send_v3(pkt, pktlen)) ||
            ((errno == EAGAIN) &&
             (wait_writable(timeout)) &&
             (send_v3(pkt, pktlen))));
  }

  inline bool ring_buffer::sendv_v1(const struct iovec* iov,
                                    size_t iovcnt,
                                    int timeout)
  {
    return ((sendv_v1(iov, iovcnt)) ||
            ((errno == EAGAIN) &&
             (wait_writable(timeout)) &&
             (sendv_v1(iov, iovcnt))));
  }

  inline bool ring_buffer::sendv_v2(const struct iovec* iov,
                                    size_t iovcnt,
                                    int timeout)
  {
    return ((sendv_v2(iov, iovcnt)) ||
            ((errno == EAGAIN) &&
             (wait_writable(timeout)) &&
             (sendv_v2(iov, iovcnt))));
  }

  inline bool ring_buffer::sendv_v3(const struct iovec* iov,
                                    size_t iovcnt,
                                    int timeout)
  {
    return ((sendv_v3(iov, iovcnt)) ||
            ((errno == EAGAIN) &&
             (wait_writable(timeout)) &&
             (sendv_v3(iov, iovcnt))));
  }
}

#endif // NET_RING_BUFFER_H
