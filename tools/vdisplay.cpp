// sb_vdisplay (Windows only): temporary virtual monitor via the installed Parsec Virtual Display
// Driver, so Sunshine can capture a genuine 3840x2160@60 desktop on a laptop whose panel is not 4K.
//
//   sb_vdisplay [--width 3840 --height 2160 --hz 60] [--seconds N]
//
// The display exists only while this program runs (the driver removes displays that stop
// receiving keep-alive pings). Ctrl+C or --seconds removes it again; nothing is persisted.
// IOCTL interface as documented by the open-source parsec-vdd project (nomi-san/parsec-vdd).
#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")

namespace {

const GUID kVddInterface = {0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
constexpr DWORD kIoctlAdd = 0x0022e004, kIoctlRemove = 0x0022a008, kIoctlUpdate = 0x0022a00c,
                kIoctlVersion = 0x0022e010;
std::atomic<bool> g_stop{false};

BOOL WINAPI on_ctrl(DWORD) {
  g_stop = true;
  return TRUE;
}

HANDLE open_vdd() {
  HDEVINFO dev = SetupDiGetClassDevsA(&kVddInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  HANDLE h = INVALID_HANDLE_VALUE;
  SP_DEVICE_INTERFACE_DATA di{};
  di.cbSize = sizeof di;
  for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(dev, nullptr, &kVddInterface, i, &di); ++i) {
    DWORD sz = 0;
    SetupDiGetDeviceInterfaceDetailA(dev, &di, nullptr, 0, &sz, nullptr);
    std::vector<char> buf(sz);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
    if (SetupDiGetDeviceInterfaceDetailA(dev, &di, det, sz, &sz, nullptr))
      h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED |
                      FILE_FLAG_WRITE_THROUGH, nullptr);
  }
  SetupDiDestroyDeviceInfoList(dev);
  return h;
}

DWORD ioctl(HANDLE h, DWORD code, const void* data, size_t len) {
  BYTE in[32] = {};
  if (data && len) std::memcpy(in, data, len < sizeof in ? len : sizeof in);
  DWORD out = 0, got = 0;
  OVERLAPPED ov{};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  DeviceIoControl(h, code, in, sizeof in, &out, sizeof out, nullptr, &ov);
  GetOverlappedResultEx(h, &ov, &got, 5000, FALSE);
  CloseHandle(ov.hEvent);
  return out;
}

void update(HANDLE h) { ioctl(h, kIoctlUpdate, nullptr, 0); }

std::vector<std::string> parsec_displays() {
  std::vector<std::string> out;
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof dd;
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    if (std::strstr(dd.DeviceString, "Parsec") && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
      out.push_back(dd.DeviceName);
    dd.cb = sizeof dd;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  int w = 3840, h = 2160, hz = 60;
  double seconds = 0;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!std::strcmp(argv[i], "--width")) w = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--height")) h = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--hz")) hz = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(argv[++i]);
  }
  SetConsoleCtrlHandler(on_ctrl, TRUE);
  HANDLE vdd = open_vdd();
  if (vdd == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "Parsec Virtual Display Driver not found\n");
    return 1;
  }
  DWORD ver = ioctl(vdd, kIoctlVersion, nullptr, 0);
  auto before = parsec_displays();
  int index = static_cast<int>(ioctl(vdd, kIoctlAdd, nullptr, 0));
  update(vdd);
  std::fprintf(stderr, "[vdisplay] driver version %lu, added display index %d\n", ver, index);

  // Keep-alive thread: the driver drops displays that are not pinged regularly.
  std::thread keep([&] {
    while (!g_stop) {
      update(vdd);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  std::string dev;
  for (int t = 0; t < 50 && dev.empty(); ++t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto& d : parsec_displays())
      if (std::find(before.begin(), before.end(), d) == before.end()) dev = d;
  }
  if (dev.empty()) {
    auto all = parsec_displays();
    if (!all.empty()) dev = all.back();
  }
  if (!dev.empty()) {
    DEVMODEA dm{};
    dm.dmSize = sizeof dm;
    dm.dmPelsWidth = w;
    dm.dmPelsHeight = h;
    dm.dmDisplayFrequency = hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG rc = ChangeDisplaySettingsExA(dev.c_str(), &dm, nullptr, 0, nullptr);
    DEVMODEA cur{};
    cur.dmSize = sizeof cur;
    EnumDisplaySettingsA(dev.c_str(), ENUM_CURRENT_SETTINGS, &cur);
    std::printf("DISPLAY %s %lux%lu@%lu (requested %dx%d@%d, ChangeDisplaySettings rc=%ld) at %ld,%ld\n",
                dev.c_str(), cur.dmPelsWidth, cur.dmPelsHeight, cur.dmDisplayFrequency, w, h, hz, rc,
                cur.dmPosition.x, cur.dmPosition.y);
    std::fflush(stdout);
  } else {
    std::printf("DISPLAY none (driver accepted the add but Windows shows no new monitor)\n");
    std::fflush(stdout);
  }

  auto t0 = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) g_stop = true;
  }
  keep.join();
  uint16_t be = static_cast<uint16_t>(((index & 0xFF) << 8) | ((index >> 8) & 0xFF));
  ioctl(vdd, kIoctlRemove, &be, sizeof be);
  update(vdd);
  CloseHandle(vdd);
  std::fprintf(stderr, "[vdisplay] removed display %d\n", index);
  return 0;
}
