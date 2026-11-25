#include "util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

namespace sb {

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string wall_clock_iso() {
  auto t = std::chrono::system_clock::now();
  std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  return buf;
}

void timer_init() {
#ifdef _WIN32
  timeBeginPeriod(1);
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

void precise_sleep_until(int64_t t) {
  for (;;) {
    int64_t d = t - now_ns();
    if (d <= 0) return;
    if (d > 2'000'000) std::this_thread::sleep_for(std::chrono::nanoseconds(d - 1'500'000));
    else if (d > 200'000) std::this_thread::yield();
  }
}

Args::Args(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--", 0) != 0) continue;
    a = a.substr(2);
    auto eq = a.find('=');
    if (eq != std::string::npos) {
      kv_[a.substr(0, eq)] = a.substr(eq + 1);
    } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
      kv_[a] = argv[++i];
    } else {
      kv_[a] = "1";
    }
  }
}
std::string Args::str(const std::string& k, const std::string& d) const {
  auto it = kv_.find(k);
  return it == kv_.end() ? d : it->second;
}
int64_t Args::num(const std::string& k, int64_t d) const {
  auto it = kv_.find(k);
  return it == kv_.end() ? d : std::stoll(it->second);
}
double Args::real(const std::string& k, double d) const {
  auto it = kv_.find(k);
  return it == kv_.end() ? d : std::stod(it->second);
}
bool Args::flag(const std::string& k) const {
  auto it = kv_.find(k);
  return it != kv_.end() && it->second != "0" && it->second != "false";
}
std::string Args::dump() const {
  std::string s;
  for (auto& [k, v] : kv_) s += k + "=" + v + ";";
  return s;
}

// ---------------------------------------------------------------- CPU
#ifdef _WIN32
static double ft_s(const FILETIME& f) {
  ULARGE_INTEGER u;
  u.LowPart = f.dwLowDateTime;
  u.HighPart = f.dwHighDateTime;
  return u.QuadPart / 1e7;
}
#endif

CpuMeter::CpuMeter() {
  cores_ = std::max(1u, std::thread::hardware_concurrency());
  double p, s;
  sample(&p, &s);
}

void CpuMeter::sample(double* proc_pct, double* sys_pct) {
  int64_t wall = now_ns();
  double proc_s = 0;
  uint64_t idle = 0, total = 0;
#ifdef _WIN32
  FILETIME c, e, k, u;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  proc_s = ft_s(k) + ft_s(u);
  FILETIME si, sk, su;
  GetSystemTimes(&si, &sk, &su);
  idle = static_cast<uint64_t>(ft_s(si) * 1e7);
  total = static_cast<uint64_t>((ft_s(sk) + ft_s(su)) * 1e7);  // kernel includes idle
#else
  {
    std::ifstream f("/proc/self/stat");
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto rp = s.rfind(')');
    std::istringstream is(s.substr(rp + 2));
    std::string tok;
    unsigned long long ut = 0, st = 0;
    for (int i = 3; i <= 15 && is >> tok; ++i) {
      if (i == 14) ut = std::stoull(tok);
      if (i == 15) st = std::stoull(tok);
    }
    proc_s = static_cast<double>(ut + st) / sysconf(_SC_CLK_TCK);
  }
  {
    std::ifstream f("/proc/stat");
    std::string cpu;
    unsigned long long v[10] = {};
    f >> cpu;
    for (auto& x : v) f >> x;
    idle = v[3] + v[4];
    for (auto x : v) total += x;
  }
#endif
  if (last_wall_ && wall > last_wall_) {
    double dt = (wall - last_wall_) / 1e9;
    *proc_pct = 100.0 * (proc_s - last_proc_s_) / dt;
    uint64_t dtot = total - last_total_, didle = idle - last_idle_;
    *sys_pct = dtot ? 100.0 * (1.0 - static_cast<double>(didle) / dtot) : 0;
  } else {
    *proc_pct = *sys_pct = 0;
  }
  last_wall_ = wall;
  last_proc_s_ = proc_s;
  last_idle_ = idle;
  last_total_ = total;
}

// ---------------------------------------------------------------- UDP
UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
  if (fd_ >= 0) {
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(fd_));
#else
    ::close(static_cast<int>(fd_));
#endif
  }
  fd_ = -1;
}

bool UdpSocket::open(const std::string& bind_ip, int port) {
  close();
  auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
  if (s == INVALID_SOCKET) return false;
#else
  if (s < 0) return false;
#endif
  fd_ = static_cast<long long>(s);
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (bind_ip.empty() || bind_ip == "0.0.0.0") a.sin_addr.s_addr = INADDR_ANY;
  else inet_pton(AF_INET, bind_ip.c_str(), &a.sin_addr);
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    close();
    return false;
  }
  return true;
}

bool UdpSocket::set_peer(const std::string& ip, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
  std::memcpy(peer_, &a, sizeof a);
  peer_len_ = sizeof a;
  return true;
}

int UdpSocket::send(const void* d, int len) { return send_to(d, len, peer_, peer_len_); }

int UdpSocket::send_to(const void* d, int len, const void* addr, int addrlen) {
  if (fd_ < 0) return -1;
  return static_cast<int>(::sendto(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                                   static_cast<const char*>(d), len, 0,
                                   static_cast<const sockaddr*>(addr), static_cast<socklen_t>(addrlen)));
}

int UdpSocket::recv(void* buf, int len, int timeout_ms, void* from, int* fromlen) {
  if (fd_ < 0) return -1;
  auto s = static_cast<decltype(::socket(0, 0, 0))>(fd_);
#ifdef _WIN32
  WSAPOLLFD p{};
  p.fd = s;
  p.events = POLLRDNORM;
  if (WSAPoll(&p, 1, timeout_ms) <= 0) return 0;
#else
  pollfd p{};
  p.fd = s;
  p.events = POLLIN;
  if (::poll(&p, 1, timeout_ms) <= 0) return 0;
#endif
  sockaddr_storage ss{};
  socklen_t sl = sizeof ss;
  int n = static_cast<int>(::recvfrom(s, static_cast<char*>(buf), len, 0, reinterpret_cast<sockaddr*>(&ss), &sl));
  if (n > 0 && from && fromlen) {
    std::memcpy(from, &ss, std::min<size_t>(sl, 32));
    *fromlen = static_cast<int>(sl);
  }
  return n;
}

// ---------------------------------------------------------------- stats / csv
Summary summarize(std::vector<double> v) {
  Summary s;
  s.n = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  auto q = [&](double p) {
    double idx = p * (v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx)), hi = static_cast<size_t>(std::ceil(idx));
    return v[lo] + (v[hi] - v[lo]) * (idx - lo);
  };
  s.min = v.front();
  s.max = v.back();
  s.median = q(0.5);
  s.p95 = q(0.95);
  s.p99 = q(0.99);
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / v.size();
  return s;
}

std::string fmt(double v, int prec) {
  char b[64];
  std::snprintf(b, sizeof b, "%.*f", prec, v);
  return b;
}

void csv_append(const std::string& path, const std::vector<std::pair<std::string, std::string>>& cols) {
  bool fresh = true;
  {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (in && in.tellg() > 0) fresh = false;
  }
  std::ofstream out(path, std::ios::app | std::ios::binary);
  auto esc = [](const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string r = "\"";
    for (char c : s) r += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return r + "\"";
  };
  if (fresh) {
    for (size_t i = 0; i < cols.size(); ++i) out << (i ? "," : "") << esc(cols[i].first);
    out << "\n";
  }
  for (size_t i = 0; i < cols.size(); ++i) out << (i ? "," : "") << esc(cols[i].second);
  out << "\n";
}

}  // namespace sb
