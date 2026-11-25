// Small cross-platform helpers: monotonic time, arguments, CPU load, UDP sockets, stats, CSV.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace sb {

// Monotonic nanoseconds (QueryPerformanceCounter on Windows, CLOCK_MONOTONIC on Linux).
int64_t now_ns();
std::string wall_clock_iso();  // local wall time, for logs only (never subtracted across hosts)
void precise_sleep_until(int64_t t_ns);  // coarse sleep then spin; call timer_init() once
void timer_init();

class Args {
 public:
  Args(int argc, char** argv);
  std::string str(const std::string& key, const std::string& def = "") const;
  int64_t num(const std::string& key, int64_t def) const;
  double real(const std::string& key, double def) const;
  bool flag(const std::string& key) const;
  bool has(const std::string& key) const { return kv_.count(key) != 0; }
  std::string dump() const;  // "key=value;..." for the CSV config column

 private:
  std::map<std::string, std::string> kv_;
};

// Process and whole-system CPU utilisation since the previous sample, in percent of ONE core
// (process) and percent of ALL cores (system).
class CpuMeter {
 public:
  CpuMeter();
  void sample(double* proc_pct_one_core, double* sys_pct_all);
  int cores() const { return cores_; }

 private:
  int cores_ = 1;
  int64_t last_wall_ = 0;
  double last_proc_s_ = 0;
  uint64_t last_idle_ = 0, last_total_ = 0;
};

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();
  bool open(const std::string& bind_ip, int port);  // port 0 = ephemeral
  bool set_peer(const std::string& ip, int port);
  int send(const void* data, int len);                       // to peer
  int send_to(const void* data, int len, const void* addr, int addrlen);
  int recv(void* buf, int len, int timeout_ms, void* from = nullptr, int* fromlen = nullptr);
  void close();
  bool ok() const { return fd_ >= 0; }

 private:
  long long fd_ = -1;
  unsigned char peer_[32] = {};
  int peer_len_ = 0;
};

struct Summary {
  size_t n = 0;
  double min = 0, median = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
};
Summary summarize(std::vector<double> v);

// Appends one header+row CSV file (writes header if the file is new/empty).
void csv_append(const std::string& path, const std::vector<std::pair<std::string, std::string>>& cols);
std::string fmt(double v, int prec = 3);

}  // namespace sb
