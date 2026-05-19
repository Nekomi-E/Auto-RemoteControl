#pragma once

// winsock2.h MUST be included before windows.h to prevent
// windows.h from pulling in the ancient winsock.h
// WIN32_LEAN_AND_MEAN and NOMINMAX are defined via CMake compile definitions
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <chrono>
#include <stdexcept>
#include <optional>
