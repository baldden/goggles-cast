// The goggles on this PC's USB port: DJI Virtual Flight's PC route, as G2 Bridge found it
// (github.com/Ammaar-Alam/g2-bridge, GPL-3.0, docs/PROTOCOL.md; checked on Goggles 2 firmware 01.11/01.14 and
// Integra 01.09). The goggles enumerate as 2CA3:0020 on a USB-A to USB-C cable (over C to C they take the host role
// and the PC does not see them); interface 4 (ff/43/01, bulk OUT 0x04, IN 0x85) carries raw DUML V1 both ways.
// The PC is device type 10, id 1. After heartbeat, identity and simulator mode (with More > DJI Virtual Flight open
// on the goggles) every frame goes as V1 0B/53 packets of at most 978 encoded bytes each, to 28.1, no ack.
// Frames over about 56 KB froze the goggles or left them black until a power cycle: they are never sent.
//
// USB access is DJI's own libusb-win32 driver (DJI Assistant 2 installs it, 5.0.0.7, System32\libusb0_device.dll);
// its usb_device structure has a 512-byte location field the upstream one has not (G2 Bridge usb_transport.py).
// Included by gcast_capture.cpp after crc8/crc16, qpc, g_freq and g_want_key.
#pragma once

namespace usbg {

#pragma pack(push, 1)
struct DeviceDescriptor {
  uint8_t bLength, bDescriptorType;
  uint16_t bcdUSB;
  uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
  uint16_t idVendor, idProduct, bcdDevice;
  uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};
struct Bus;
struct Device {   // DJI's libusb0 layout: upstream plus location[512]
  Device *next, *prev;
  char filename[512];
  char location[512];
  Bus *bus;
  DeviceDescriptor descriptor;
  void *config, *dev;
  uint8_t devnum, num_children;
  Device **children;
};
struct Bus {
  Bus *next, *prev;
  char dirname[512];
  Device *devices;
  uint32_t location;
  Device *root_dev;
};
#pragma pack(pop)
static_assert(offsetof(Device, descriptor) == 1048 && sizeof(Device) == 1092, "DJI libusb0 usb_device layout");

static constexpr uint16_t VID = 0x2CA3, PID = 0x0020;
static constexpr int INTERFACE = 4, EP_OUT = 0x04, EP_IN = 0x85;
static constexpr uint8_t PC_TYPE = 10, PC_ID = 1, GOGGLES = 0x1C;
static constexpr size_t V1_OVERHEAD = 13, V1_MAX = 1023, VIDEO_HEADER = 32;
static constexpr size_t VIDEO_CHUNK = V1_MAX - V1_OVERHEAD - VIDEO_HEADER;   // 978
static constexpr size_t MAX_FRAME = 56000;
static constexpr size_t WRITE_SIZE = 16384;

static struct {
  void (*init)();
  int (*find_busses)();
  int (*find_devices)();
  Bus *(*get_busses)();
  void *(*open)(Device *);
  int (*close)(void *);
  int (*claim_interface)(void *, int);
  int (*bulk_write)(void *, int, char *, int, int);
  int (*bulk_read)(void *, int, char *, int, int);
  char *(*strerror)();
} lib;

static bool load() {
  static int state = 0;   // 0 not tried, 1 loaded, -1 missing
  if (state) return state > 0;
  wchar_t path[MAX_PATH];
  GetSystemDirectoryW(path, MAX_PATH);
  wcscat_s(path, L"\\libusb0_device.dll");
  HMODULE m = LoadLibraryW(path);
  if (!m) { state = -1; return false; }
  lib.init = (decltype(lib.init)) GetProcAddress(m, "usb_init");
  lib.find_busses = (decltype(lib.find_busses)) GetProcAddress(m, "usb_find_busses");
  lib.find_devices = (decltype(lib.find_devices)) GetProcAddress(m, "usb_find_devices");
  lib.get_busses = (decltype(lib.get_busses)) GetProcAddress(m, "usb_get_busses");
  lib.open = (decltype(lib.open)) GetProcAddress(m, "usb_open");
  lib.close = (decltype(lib.close)) GetProcAddress(m, "usb_close");
  lib.claim_interface = (decltype(lib.claim_interface)) GetProcAddress(m, "usb_claim_interface");
  lib.bulk_write = (decltype(lib.bulk_write)) GetProcAddress(m, "usb_bulk_write");
  lib.bulk_read = (decltype(lib.bulk_read)) GetProcAddress(m, "usb_bulk_read");
  lib.strerror = (decltype(lib.strerror)) GetProcAddress(m, "usb_strerror");
  state = lib.init && lib.find_busses && lib.find_devices && lib.get_busses && lib.open && lib.close &&
          lib.claim_interface && lib.bulk_write && lib.bulk_read ? 1 : -1;
  if (state > 0) lib.init();
  return state > 0;
}

// G2 Bridge control.py V1Packet.serialize
static void v1(std::vector<uint8_t> &out, uint8_t set, uint8_t id, uint8_t dst_id, uint8_t ack, uint16_t seq,
               const uint8_t *payload, size_t n) {
  const size_t len = n + V1_OVERHEAD, at = out.size();
  out.resize(at + len);
  uint8_t *p = &out[at];
  p[0] = 0x55;
  p[1] = uint8_t(len);
  p[2] = uint8_t((len >> 8) | 0x04);
  p[3] = crc8(p, 3);
  p[4] = PC_TYPE | (PC_ID << 5);
  p[5] = GOGGLES | (dst_id << 5);
  p[6] = uint8_t(seq); p[7] = uint8_t(seq >> 8);
  p[8] = uint8_t(ack << 5);
  p[9] = set; p[10] = id;
  if (n) memcpy(p + 11, payload, n);
  const uint16_t c = crc16(p, len - 2);
  p[len - 2] = uint8_t(c); p[len - 1] = uint8_t(c >> 8);
}

class Link {
 public:
  std::atomic<bool> ready{false};
  std::atomic<int64_t> frames_sent{0}, frames_oversize{0};

  void start() { std::thread([this] { run(); }).detach(); }

  // One encoded access unit (Annex-B, SPS/PPS before a key frame), from the encoder's side. False when not sent.
  bool send_frame(const uint8_t *au, size_t n, bool idr) {
    if (!ready) { resync_ = true; return false; }
    if (n > MAX_FRAME) {
      ++frames_oversize;
      g_want_key = true;   // the frames after it refer to it: start again at a key frame that fits
      resync_ = true;
      return false;
    }
    if (resync_ && !idr) { g_want_key = true; return false; }
    resync_ = false;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t unix_ms = int64_t(((uint64_t) ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
    const int64_t ts = (int32_t) (uint32_t) unix_ms;   // as the app: low 32 bits, signed, sign-extended
    std::vector<uint8_t> wire;
    wire.reserve(n + (n / VIDEO_CHUNK + 1) * (V1_OVERHEAD + VIDEO_HEADER));
    uint8_t pl[VIDEO_HEADER + VIDEO_CHUNK];
    for (size_t off = 0; off < n; off += VIDEO_CHUNK) {
      const size_t len = std::min(VIDEO_CHUNK, n - off);
      memset(pl, 0, VIDEO_HEADER);   // pc_video.py / duml.py video_fragments
      pl[1] = n > VIDEO_CHUNK;
      pl[2] = off + len == n;
      put_le<uint32_t>(pl + 4, chunk_counter_++);
      put_le<uint64_t>(pl + 8, (uint64_t) ts);
      put_le<uint32_t>(pl + 16, (uint32_t) VIDEO_HEADER);
      put_le<uint32_t>(pl + 20, (uint32_t) len);
      pl[24] = off == 0;
      memcpy(pl + VIDEO_HEADER, au + off, len);
      uint16_t s;
      {
        std::lock_guard<std::mutex> l(m_);
        s = next_seq();
      }
      v1(wire, 0x0B, 0x53, 1, 0, s, pl, VIDEO_HEADER + len);
    }
    if (!write(wire)) return false;
    ++frames_sent;
    return true;
  }

 private:
  struct Pending { uint8_t set, id, dst; std::vector<uint8_t> bytes; int64_t sent; int tries; };
  std::mutex m_;                          // seq_ and pending_ (the video thread takes sequence numbers too)
  std::mutex w_;                          // one writer at a time on the OUT endpoint
  void *h_ = nullptr;
  uint16_t seq_ = 0;
  std::map<uint16_t, Pending> pending_;
  std::atomic<bool> resync_{true}, write_failed_{false};
  uint32_t chunk_counter_ = 0;            // the video thread only
  // the reader thread only
  int state_ = 0;                         // 0 heartbeat, 1 identity, 2 simulator mode, 3 ready
  int64_t next_hb_ = 0, last_ack_ = 0;

  uint16_t next_seq() {   // video and control share it, never reusing one still waiting for a reply
    for (int i = 0; i < 65536; ++i) {
      const uint16_t s = seq_++;
      if (!pending_.count(s)) return s;
    }
    return seq_++;
  }

  bool write(const std::vector<uint8_t> &data) {
    std::lock_guard<std::mutex> l(w_);
    if (!h_) return false;
    for (size_t off = 0; off < data.size(); off += WRITE_SIZE) {
      const int len = (int) std::min(WRITE_SIZE, data.size() - off);
      if (lib.bulk_write(h_, EP_OUT, (char *) data.data() + off, len, 500) != len) {
        write_failed_ = true;   // the reader thread starts again
        return false;
      }
    }
    return true;
  }

  void request(uint8_t set, uint8_t id, uint8_t dst, const uint8_t *payload, size_t n, int64_t now) {
    std::vector<uint8_t> pkt;
    {
      std::lock_guard<std::mutex> l(m_);
      const uint16_t s = next_seq();
      v1(pkt, set, id, dst, 2, s, payload, n);
      pending_[s] = {set, id, dst, pkt, now, 1};
    }
    write(pkt);
  }

  void restart(const char *why) {
    if (ready.exchange(false) || state_) {
      fprintf(stderr, "usb: handshake again (%s)\n", why);
      fflush(stderr);
    }
    std::lock_guard<std::mutex> l(m_);
    state_ = 0; next_hb_ = 0; last_ack_ = 0;
    pending_.clear();
    resync_ = true;
  }

  // session.py poll(): a heartbeat every 0.5 s; an unanswered identity or simulator mode request is sent again after
  // 0.5 s, three times at most; three heartbeat intervals without an answer start the handshake again
  void tick(int64_t now) {
    const int64_t half = g_freq / 2;
    if (last_ack_ && now - last_ack_ >= 3 * half) { restart("no heartbeat answer for 1.5 s"); return; }
    std::vector<std::vector<uint8_t>> again;
    bool sim_ignored = false;
    {
      std::lock_guard<std::mutex> l(m_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        Pending &p = it->second;
        if (now - p.sent < half) { ++it; continue; }
        if (p.set == 0x00 && p.id == 0x00) { it = pending_.erase(it); continue; }   // the next heartbeat replaces it
        if (p.tries >= 3) {
          sim_ignored |= p.set == 0x0B && p.id == 0x1A;
          it = pending_.erase(it);
          continue;
        }
        ++p.tries;
        p.sent = now;
        again.push_back(p.bytes);
        ++it;
      }
    }
    for (auto &b : again) write(b);
    if (sim_ignored) {
      // heartbeat and identity are answered outside Virtual Flight too, simulator mode is not (session.py)
      fprintf(stderr, "usb: the goggles ignore simulator mode: open More > DJI Virtual Flight on them\n");
      fflush(stderr);
      restart("simulator mode not answered");
      return;
    }
    if (now >= next_hb_) {
      static const uint8_t hb[] = {0, 0, 0x20, 0, 0, 0};
      next_hb_ = now + half;
      request(0x00, 0x00, 0, hb, sizeof hb, now);
    }
  }

  // session.py receive(): a reply counts when it matches a waiting request's sequence, command and route
  void on_packet(const uint8_t *p, size_t len, int64_t now) {
    if (!(p[8] & 0x80)) return;   // not a response
    const uint16_t seq = uint16_t(p[6] | p[7] << 8);
    const uint8_t set = p[9], id = p[10];
    {
      std::lock_guard<std::mutex> l(m_);
      auto it = pending_.find(seq);
      if (it == pending_.end() || it->second.set != set || it->second.id != id) return;
      if ((p[4] & 31) != GOGGLES || (p[4] >> 5) != it->second.dst || (p[5] & 31) != PC_TYPE || (p[5] >> 5) != PC_ID)
        return;
      pending_.erase(it);
    }
    const uint8_t *pl = p + 11;
    const size_t n = len - V1_OVERHEAD;
    if (!n || pl[0] != 0) {
      fprintf(stderr, "usb: the goggles rejected %02x/%02x (status %02x)\n", set, id, n ? pl[0] : 0xFF);
      fflush(stderr);
      return;
    }
    if (set == 0x00 && id == 0x00) {
      last_ack_ = now;
      if (state_ == 0) {
        state_ = 1;
        static const uint8_t z[] = {0};
        request(0x00, 0xFF, 1, z, 1, now);
      }
    } else if (set == 0x00 && id == 0xFF && state_ == 1) {
      std::string model((const char *) pl + 1, strnlen((const char *) pl + 1, n - 1));
      fprintf(stderr, "usb: goggles '%s'\n", model.c_str());
      fflush(stderr);
      state_ = 2;
      static const uint8_t sim[] = {1, 1, 0, 0};
      request(0x0B, 0x1A, 0, sim, sizeof sim, now);
    } else if (set == 0x0B && id == 0x1A && state_ == 2) {
      state_ = 3;
      resync_ = true;
      g_want_key = true;   // the goggles' decoder starts at a key frame
      ready = true;
      fprintf(stderr, "usb: ready, video to the goggles\n");
      fflush(stderr);
    }
  }

  // control.py V1Stream: V1 packets out of the byte stream, a bad header or checksum skipped a byte at a time
  void feed(std::vector<uint8_t> &buf, int64_t now) {
    size_t i = 0;
    while (buf.size() - i >= 4) {
      const uint8_t *p = &buf[i];
      const uint16_t sv = uint16_t(p[1] | p[2] << 8);
      const size_t size = sv & 0x3FF;
      if (p[0] != 0x55 || (sv >> 10) != 1 || size < V1_OVERHEAD || crc8(p, 3) != p[3]) { ++i; continue; }
      if (buf.size() - i < size) break;
      if (crc16(p, size - 2) != uint16_t(p[size - 2] | p[size - 1] << 8)) { ++i; continue; }
      on_packet(p, size, now);
      i += size;
    }
    buf.erase(buf.begin(), buf.begin() + ptrdiff_t(i));
    if (buf.size() > (1u << 16)) buf.clear();
  }

  bool open() {
    lib.find_busses();
    lib.find_devices();
    for (Bus *b = lib.get_busses(); b; b = b->next)
      for (Device *d = b->devices; d; d = d->next)
        if (d->descriptor.idVendor == VID && d->descriptor.idProduct == PID) {
          void *h = lib.open(d);
          if (!h) continue;
          if (lib.claim_interface(h, INTERFACE) < 0) { lib.close(h); continue; }
          std::lock_guard<std::mutex> l(w_);
          h_ = h;
          return true;
        }
    return false;
  }

  void close() {
    std::lock_guard<std::mutex> l(w_);
    if (h_) lib.close(h_);
    h_ = nullptr;
  }

  void run() {
    if (!load()) {
      fprintf(stderr, "usb: DJI USB driver not found (install DJI Assistant 2: System32\\libusb0_device.dll)\n");
      fflush(stderr);
      return;
    }
    {
      LARGE_INTEGER r;
      QueryPerformanceCounter(&r);
      std::lock_guard<std::mutex> l(m_);
      seq_ = uint16_t((uint64_t) r.QuadPart * 2654435761u >> 7);   // a restart from 0 is taken for repeats
    }
    bool said_waiting = false;
    std::vector<uint8_t> rx;
    char buf[4096];
    for (;;) {
      if (!h_) {
        if (!open()) {
          if (!said_waiting) {
            fprintf(stderr, "usb: waiting for the goggles (USB-A to USB-C cable, More > DJI Virtual Flight)\n");
            fflush(stderr);
            said_waiting = true;
          }
          Sleep(1000);
          continue;
        }
        said_waiting = false;
        fprintf(stderr, "usb: goggles on USB\n");
        fflush(stderr);
        restart("connected");
        rx.clear();
      }
      if (write_failed_.exchange(false)) {
        restart("USB write failed");
        close();
        continue;
      }
      tick(qpc());
      const int n = lib.bulk_read(h_, EP_IN, buf, sizeof buf, 50);
      if (n > 0) {
        rx.insert(rx.end(), buf, buf + n);
        feed(rx, qpc());
      } else if (n < 0 && n != -116) {   // -116: no reply within 50 ms (session.py is_timeout)
        restart("USB read failed");
        close();
        Sleep(500);
      }
    }
  }
};

}  // namespace usbg
