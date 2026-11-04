// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <mutex>
namespace srt::sync {
template <class T> using atomic = std::atomic<T>;
using Mutex = std::mutex;
}
