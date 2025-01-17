//==---- memory.hpp -------------------------------*- C++ -*----------------==//
//
// Copyright (C) Intel Corporation
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// See https://llvm.org/LICENSE.txt for license information.
//
//===----------------------------------------------------------------------===//

#ifndef __DPCT_MEMORY_HPP__
#define __DPCT_MEMORY_HPP__

#include "device.hpp"
#include <CL/sycl.hpp>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <map>
#include <utility>
#include <thread>
#include <type_traits>

#if defined(__linux__)
#include <sys/mman.h>
#elif defined(_WIN64)
#define NOMINMAX
#include <windows.h>
#else
#error "Only support Windows and Linux."
#endif

namespace dpct {

enum memcpy_direction {
  host_to_host,
  host_to_device,
  device_to_host,
  device_to_device,
  automatic
};
enum memory_region {
  global = 0,  // device global memory
  constant,    // device constant memory
  local,       // device local memory
  shared,      // memory which can be accessed by host and device
};

typedef uint8_t byte_t;

/// Buffer type to be used in Memory Management runtime.
typedef cl::sycl::buffer<byte_t> buffer_t;

/// Pitched 2D/3D memory data.
class pitched_data {
public:
  pitched_data() : pitched_data(nullptr, 0, 0, 0) {}
  pitched_data(void *data, size_t pitch, size_t x, size_t y)
      : _data(data), _pitch(pitch), _x(x), _y(y) {}

  void *get_data_ptr() { return _data; }
  void set_data_ptr(void *data) { _data = data; }

  size_t get_pitch() { return _pitch; }
  void set_pitch(size_t pitch) { _pitch = pitch; }

  size_t get_x() { return _x; }
  void set_x(size_t x) { _x = x; };

  size_t get_y() { return _y; }
  void set_y(size_t y) { _y = y; }

private:
  void *_data;
  size_t _pitch, _x, _y;
};

namespace detail {
class mem_mgr {
  mem_mgr() {
    // Reserved address space, no real memory allocation happens here.
#if defined(__linux__)
    mapped_address_space =
        (byte_t *)mmap(nullptr, mapped_region_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(_WIN64)
    mapped_address_space = (byte_t *)VirtualAlloc(
        NULL,               // NULL specified as the base address parameter
        mapped_region_size, // Size of allocation
        MEM_RESERVE,        // Allocate reserved pages
        PAGE_NOACCESS);     // Protection = no access
#else
#error "Only support Windows and Linux."
#endif
    next_free = mapped_address_space;
  };

public:
  using buffer_id_t = int;

  struct allocation {
    buffer_t buffer;
    byte_t *alloc_ptr;
    size_t size;
  };

  ~mem_mgr() {
#if defined(__linux__)
    munmap(mapped_address_space, mapped_region_size);
#elif defined(_WIN64)
    VirtualFree(mapped_address_space, 0, MEM_RELEASE);
#else
#error "Only support Windows and Linux."
#endif
  };

  mem_mgr(const mem_mgr &) = delete;
  mem_mgr &operator=(const mem_mgr &) = delete;
  mem_mgr(mem_mgr &&) = delete;
  mem_mgr &operator=(mem_mgr &&) = delete;

  /// Allocate
  void *mem_alloc(size_t size) {
    if (!size)
      return nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (next_free + size > mapped_address_space + mapped_region_size) {
      throw std::runtime_error("dpct_malloc: out of memory for virtual memory pool");
    }
    // Allocation
    cl::sycl::range<1> r(size);
    buffer_t buf(r);
    allocation A{buf, next_free, size};
    // Map allocation to device pointer
    void *result = next_free;
    m_map.emplace(next_free + size, A);
    // Update pointer to the next free space.
    next_free += (size + extra_padding + alignment - 1) & ~(alignment - 1);

    return result;
  }

  /// Deallocate
  void mem_free(const void *ptr) {
    if (!ptr)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = get_map_iterator(ptr);
    m_map.erase(it);
  }

  /// map: device pointer -> allocation(buffer, alloc_ptr, size)
  allocation translate_ptr(const void *ptr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = get_map_iterator(ptr);
    return it->second;
  }

  /// Check if the pointer represents device pointer or not.
  bool is_device_ptr(const void *ptr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (mapped_address_space <= ptr) &&
           (ptr < mapped_address_space + mapped_region_size);
  }

  /// Returns the instance of memory manager singleton.
  static mem_mgr &instance() {
    static mem_mgr m;
    return m;
  }

private:
  std::map<byte_t *, allocation> m_map;
  mutable std::mutex m_mutex;
  byte_t *mapped_address_space;
  byte_t *next_free;
  const size_t mapped_region_size = 128ull * 1024 * 1024 * 1024;
  const size_t alignment = 256;
  /// This padding may be defined to some positive value to debug
  /// out of bound accesses.
  const size_t extra_padding = 0;

  std::map<byte_t *, allocation>::iterator get_map_iterator(const void *ptr) {
    auto it = m_map.upper_bound((byte_t *)ptr);
    if (it == m_map.end()) {
      // Not a virtual pointer.
      throw std::runtime_error("can not get buffer from non-virtual pointer");
    }
    const allocation &alloc = it->second;
    if (ptr < alloc.alloc_ptr) {
      // Out of bound.
      // This may happen if there's a gap between allocations due to alignment
      // or extra padding and pointer points to this gap.
      throw std::runtime_error("invalid virtual pointer");
    }
    return it;
  }
};

template <class T, memory_region Memory, size_t Dimension> class accessor;
template <memory_region Memory, class T = byte_t> class memory_traits {
public:
  static constexpr cl::sycl::access::address_space asp =
      (Memory == local)
          ? cl::sycl::access::address_space::local_space
          : ((Memory == constant)
                 ? cl::sycl::access::address_space::constant_space
                 : cl::sycl::access::address_space::global_space);
  static constexpr cl::sycl::access::target target =
      (Memory == local)
          ? cl::sycl::access::target::local
          : ((Memory == constant) ? cl::sycl::access::target::constant_buffer
                                  : cl::sycl::access::target::device);
  static constexpr cl::sycl::access_mode mode =
      (Memory == constant) ? cl::sycl::access_mode::read
                           : cl::sycl::access_mode::read_write;
  static constexpr size_t type_size = sizeof(T);
  using element_t =
      typename std::conditional<Memory == constant, const T, T>::type;
  using value_t = typename std::remove_cv<T>::type;
  template <size_t Dimension = 1>
  using accessor_t = cl::sycl::accessor<T, Dimension, mode, target>;
  using pointer_t = T *;
};

static inline void *dpct_malloc(size_t size, cl::sycl::queue &q) {
#ifdef DPCT_USM_LEVEL_NONE
  return mem_mgr::instance().mem_alloc(size * sizeof(byte_t));
#else
  return cl::sycl::malloc_device(size, q.get_device(), q.get_context());
#endif // DPCT_USM_LEVEL_NONE
}

#define PITCH_DEFAULT_ALIGN(x) (((x) + 31) & ~(0x1F))
static inline void *dpct_malloc(size_t &pitch, size_t x, size_t y, size_t z,
                                cl::sycl::queue &q) {
  pitch = PITCH_DEFAULT_ALIGN(x);
  return dpct_malloc(pitch * y * z, q);
}

/// Set \p value to the first \p size bytes starting from \p dev_ptr in \p q.
///
/// \param q The queue in which the operation is done.
/// \param dev_ptr Pointer to the device memory address.
/// \param value Value to be set.
/// \param size Number of bytes to be set to the value.
/// \returns An event representing the memset operation.
static inline cl::sycl::event dpct_memset(cl::sycl::queue &q, void *dev_ptr,
                                          int value, size_t size) {
#ifdef DPCT_USM_LEVEL_NONE
  auto &mm = mem_mgr::instance();
  assert(mm.is_device_ptr(dev_ptr));
  auto alloc = mm.translate_ptr(dev_ptr);
  size_t offset = (byte_t *)dev_ptr - alloc.alloc_ptr;

  return q.submit([&](cl::sycl::handler &cgh) {
    auto r = cl::sycl::range<1>(size);
    auto o = cl::sycl::id<1>(offset);
    cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::write,
                       cl::sycl::access::target::device>
        acc(alloc.buffer, cgh, r, o);
    cgh.fill(acc, (byte_t)value);
  });
#else
  return q.memset(dev_ptr, value, size);
#endif // DPCT_USM_LEVEL_NONE
}

/// Set \p value to the 3D memory region pointed by \p data in \p q. \p size
/// specifies the 3D memory size to set.
///
/// \param q The queue in which the operation is done.
/// \param data Pointer to the device memory region.
/// \param value Value to be set.
/// \param size Memory region size.
/// \returns An event list representing the memset operations.
static inline std::vector<cl::sycl::event>
dpct_memset(cl::sycl::queue &q, pitched_data data, int value,
            cl::sycl::range<3> size) {
  std::vector<cl::sycl::event> event_list;
  size_t slice = data.get_pitch() * data.get_y();
  unsigned char *data_surface = (unsigned char *)data.get_data_ptr();
  for (size_t z = 0; z < size.get(2); ++z) {
    unsigned char *data_ptr = data_surface;
    for (size_t y = 0; y < size.get(1); ++y) {
      event_list.push_back(dpct_memset(q, data_ptr, value, size.get(0)));
      data_ptr += data.get_pitch();
    }
    data_surface += slice;
  }
  return event_list;
}

/// memset 2D matrix with pitch.
static inline std::vector<cl::sycl::event>
dpct_memset(cl::sycl::queue &q, void *ptr, size_t pitch, int val, size_t x,
            size_t y) {
  return dpct_memset(q, pitched_data(ptr, pitch, x, 1), val,
                     cl::sycl::range<3>(x, y, 1));
}

enum class pointer_access_attribute {
  host_only = 0,
  device_only,
  host_device,
  end
};

static pointer_access_attribute get_pointer_attribute(cl::sycl::queue &q,
                                                      const void *ptr) {
#ifdef DPCT_USM_LEVEL_NONE
  return mem_mgr::instance().is_device_ptr(ptr)
             ? pointer_access_attribute::device_only
             : pointer_access_attribute::host_only;
#else
  switch (cl::sycl::get_pointer_type(ptr, q.get_context())) {
  case cl::sycl::usm::alloc::unknown:
    return pointer_access_attribute::host_only;
  case cl::sycl::usm::alloc::device:
    return pointer_access_attribute::device_only;
  case cl::sycl::usm::alloc::shared:
  case cl::sycl::usm::alloc::host:
    return pointer_access_attribute::host_device;
  }
#endif
}

static memcpy_direction deduce_memcpy_direction(cl::sycl::queue &q, void *to_ptr,
                                             const void *from_ptr,
                                             memcpy_direction dir) {
  switch (dir) {
  case memcpy_direction::host_to_host:
  case memcpy_direction::host_to_device:
  case memcpy_direction::device_to_host:
  case memcpy_direction::device_to_device:
    return dir;
  case memcpy_direction::automatic: {
    // table[to_attribute][from_attribute]
    static const memcpy_direction
        direction_table[static_cast<unsigned>(pointer_access_attribute::end)]
                       [static_cast<unsigned>(pointer_access_attribute::end)] =
                           {{memcpy_direction::host_to_host,
                             memcpy_direction::device_to_host,
                             memcpy_direction::host_to_host},
                            {memcpy_direction::host_to_device,
                             memcpy_direction::device_to_device,
                             memcpy_direction::device_to_device},
                            {memcpy_direction::host_to_host,
                             memcpy_direction::device_to_device,
                             memcpy_direction::device_to_device}};
    return direction_table[static_cast<unsigned>(get_pointer_attribute(
        q, to_ptr))][static_cast<unsigned>(get_pointer_attribute(q, from_ptr))];
  }
  default:
    throw std::runtime_error("dpct_memcpy: invalid direction value");
  }
}

static cl::sycl::event
dpct_memcpy(cl::sycl::queue &q, void *to_ptr, const void *from_ptr, size_t size,
            memcpy_direction direction,
            const std::vector<cl::sycl::event> &dep_events = {}) {
  if (!size)
    return cl::sycl::event{};
#ifdef DPCT_USM_LEVEL_NONE
  auto &mm = mem_mgr::instance();
  auto real_direction = deduce_memcpy_direction(q, to_ptr, from_ptr, direction);

  switch (real_direction) {
  case host_to_host:
    return q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(dep_events);
      cgh.host_task([=] { std::memcpy(to_ptr, from_ptr, size); });
    });
  case host_to_device: {
    auto alloc = mm.translate_ptr(to_ptr);
    size_t offset = (byte_t *)to_ptr - alloc.alloc_ptr;
    return q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(dep_events);
      auto r = cl::sycl::range<1>(size);
      auto o = cl::sycl::id<1>(offset);
      cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::write,
                          cl::sycl::access::target::device>
          acc(alloc.buffer, cgh, r, o);
      cgh.copy(from_ptr, acc);
    });
  }
  case device_to_host: {
    auto alloc = mm.translate_ptr(from_ptr);
    size_t offset = (byte_t *)from_ptr - alloc.alloc_ptr;
    return q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(dep_events);
      auto r = cl::sycl::range<1>(size);
      auto o = cl::sycl::id<1>(offset);
      cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::read,
                          cl::sycl::access::target::device>
          acc(alloc.buffer, cgh, r, o);
      cgh.copy(acc, to_ptr);
    });
  }
  case device_to_device: {
    auto to_alloc = mm.translate_ptr(to_ptr);
    auto from_alloc = mm.translate_ptr(from_ptr);
    size_t to_offset = (byte_t *)to_ptr - to_alloc.alloc_ptr;
    size_t from_offset = (byte_t *)from_ptr - from_alloc.alloc_ptr;
    return q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(dep_events);
      auto r = cl::sycl::range<1>(size);
      auto to_o = cl::sycl::id<1>(to_offset);
      auto from_o = cl::sycl::id<1>(from_offset);
      cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::write,
                          cl::sycl::access::target::device>
          to_acc(to_alloc.buffer, cgh, r, to_o);
      cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::read,
                          cl::sycl::access::target::device>
          from_acc(from_alloc.buffer, cgh, r, from_o);
      cgh.copy(from_acc, to_acc);
    });
  }
  default:
    throw std::runtime_error("dpct_memcpy: invalid direction value");
  }
#else
  return q.memcpy(to_ptr, from_ptr, size, dep_events);
#endif // DPCT_USM_LEVEL_NONE
}

/// copy 3D matrix specified by \p size from 3D matrix specified by \p from_ptr
/// and \p from_range to another specified by \p to_ptr and \p to_range.
static inline std::vector<cl::sycl::event>
dpct_memcpy(cl::sycl::queue &q, void *to_ptr, const void *from_ptr,
            cl::sycl::range<3> to_range, cl::sycl::range<3> from_range,
            cl::sycl::id<3> to_id, cl::sycl::id<3> from_id,
            cl::sycl::range<3> size, memcpy_direction direction,
            const std::vector<cl::sycl::event> &dep_events = {}) {
  std::vector<cl::sycl::event> event_list;

  size_t to_slice = to_range.get(1) * to_range.get(0),
         from_slice = from_range.get(1) * from_range.get(0);
  unsigned char *to_surface = (unsigned char *)to_ptr +
                              to_id.get(2) * to_slice +
                              to_id.get(1) * to_range.get(0) + to_id.get(0);
  const unsigned char *from_surface =
      (const unsigned char *)from_ptr + from_id.get(2) * from_slice +
      from_id.get(1) * from_range.get(0) + from_id.get(0);

  if (to_slice == from_slice && to_slice == size.get(1) * size.get(0)) {
    return {dpct_memcpy(q, to_surface, from_surface, to_slice * size.get(2),
                        direction, dep_events)};
  }
  direction = deduce_memcpy_direction(q, to_ptr, from_ptr, direction);
  void *tmp_host_buf = nullptr;
  size_t size_slice = size.get(1) * size.get(0);
  switch (direction) {
  case host_to_host:
    for (size_t z = 0; z < size.get(2); ++z) {
      unsigned char *to_ptr = to_surface;
      const unsigned char *from_ptr = from_surface;
      if (to_range.get(0) == from_range.get(0) &&
          to_range.get(0) == size.get(0)) {
        event_list.push_back(dpct_memcpy(q, to_ptr, from_ptr, size_slice,
                                         direction, dep_events));
      } else {
        for (size_t y = 0; y < size.get(1); ++y) {
          event_list.push_back(dpct_memcpy(q, to_ptr, from_ptr, size.get(0),
                                           direction, dep_events));
          to_ptr += to_range.get(0);
          from_ptr += from_range.get(0);
        }
      }
      to_surface += to_slice;
      from_surface += from_slice;
    }
    break;
  case host_to_device: {
    tmp_host_buf = std::malloc(to_slice * size.get(2));
    std::vector<cl::sycl::event> host_events;
    if (to_slice == size_slice) {
      // Copy host data to a temp host buffer with the shape of target.
      host_events =
          dpct_memcpy(q, tmp_host_buf, from_surface, to_range, from_range,
                      sycl::id<3>(0, 0, 0), sycl::id<3>(0, 0, 0), size,
                      host_to_host, dep_events);
    } else {
      // Copy host data to a temp host buffer with the shape of target.
      host_events = dpct_memcpy(
          q, tmp_host_buf, from_surface, to_range, from_range,
          sycl::id<3>(0, 0, 0), sycl::id<3>(0, 0, 0), size, host_to_host,
          // If has padding data, not sure whether it is useless. So fill temp
          // buffer with it.
          std::vector<cl::sycl::event>{
              dpct_memcpy(q, tmp_host_buf, to_surface, to_slice * size.get(2),
                          device_to_host, dep_events)});
    }
    // Copy from temp host buffer to device with only one submit.
    event_list.push_back(dpct_memcpy(q, to_surface, tmp_host_buf,
                                     to_slice * size.get(2), host_to_device,
                                     host_events));
    break;
  }
  case device_to_host: {
    tmp_host_buf = std::malloc(from_slice * size.get(2));
    // Copy from host temp buffer to host target with reshaping.
    event_list = dpct_memcpy(
        q, to_surface, tmp_host_buf, to_range, from_range, sycl::id<3>(0, 0, 0),
        sycl::id<3>(0, 0, 0), size, host_to_host,
        // Copy from device to temp host buffer with only one submit.
        std::vector<cl::sycl::event>{dpct_memcpy(q, tmp_host_buf, from_surface,
                                                 from_slice * size.get(2),
                                                 device_to_host, dep_events)});
    break;
  }
  case device_to_device:
#ifdef DPCT_USM_LEVEL_NONE
  {
    auto &mm = mem_mgr::instance();
    auto to_alloc = mm.translate_ptr(to_surface);
    auto from_alloc = mm.translate_ptr(from_surface);
    size_t to_offset = (byte_t *)to_surface - to_alloc.alloc_ptr;
    size_t from_offset = (byte_t *)from_surface - from_alloc.alloc_ptr;
    event_list.push_back(q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(dep_events);
      auto to_o = cl::sycl::id<1>(to_offset);
      auto from_o = cl::sycl::id<1>(from_offset);
      cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::write,
                         cl::sycl::access::target::device>
          to_acc(to_alloc.buffer, cgh, to_slice * size.get(2), to_o);
      cl::sycl::accessor<byte_t, 1, cl::sycl::access_mode::read,
                         cl::sycl::access::target::device>
          from_acc(from_alloc.buffer, cgh, from_slice * size.get(2), from_o);
      cgh.parallel_for<class dpct_memcpy_3d_detail_usmnone>(
          sycl::range<3>(size.get(2), size.get(1), size.get(0)),
          [=](cl::sycl::id<3> id) {
            to_acc[to_slice * id.get(0) + to_range.get(0) * id.get(1) +
                   id.get(2)] =
                from_acc[from_slice * id.get(0) +
                         from_range.get(0) * id.get(1) + id.get(2)];
          });
    }));
  }
#else
    event_list.push_back(q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(dep_events);
      cgh.parallel_for<class dpct_memcpy_3d_detail>(
          sycl::range<3>(size.get(2), size.get(1), size.get(0)),
          [=](cl::sycl::id<3> id) {
            to_surface[to_slice * id.get(0) + to_range.get(0) * id.get(1) +
                       id.get(2)] =
                from_surface[from_slice * id.get(0) +
                             from_range.get(0) * id.get(1) + id.get(2)];
          });
    }));
#endif
  break;
  default:
    throw std::runtime_error("dpct_memcpy: invalid direction value");
  }
  if (tmp_host_buf)
    q.submit([&](cl::sycl::handler &cgh) {
      cgh.depends_on(event_list);
      cgh.host_task([=] { std::free(tmp_host_buf); });
    });
  return event_list;
}

/// memcpy 2D/3D matrix specified by pitched_data.
static inline std::vector<cl::sycl::event>
dpct_memcpy(cl::sycl::queue &q, pitched_data to, cl::sycl::id<3> to_id,
            pitched_data from, cl::sycl::id<3> from_id, cl::sycl::range<3> size,
            memcpy_direction direction = automatic) {
  return dpct_memcpy(q, to.get_data_ptr(), from.get_data_ptr(),
                     cl::sycl::range<3>(to.get_pitch(), to.get_y(), 1),
                     cl::sycl::range<3>(from.get_pitch(), from.get_y(), 1), to_id, from_id,
                     size, direction);
}

/// memcpy 2D matrix with pitch.
static inline std::vector<cl::sycl::event>
dpct_memcpy(cl::sycl::queue &q, void *to_ptr, const void *from_ptr,
            size_t to_pitch, size_t from_pitch, size_t x, size_t y,
            memcpy_direction direction = automatic) {
  return dpct_memcpy(q, to_ptr, from_ptr, cl::sycl::range<3>(to_pitch, y, 1),
                     cl::sycl::range<3>(from_pitch, y, 1),
                     cl::sycl::id<3>(0, 0, 0), cl::sycl::id<3>(0, 0, 0),
                     cl::sycl::range<3>(x, y, 1), direction);
}
} // namespace detail

#ifdef DPCT_USM_LEVEL_NONE
/// Check if the pointer \p ptr represents device pointer or not.
///
/// \param ptr The pointer to be checked.
/// \returns true if \p ptr is a device pointer.
template<class T>
static inline bool is_device_ptr(T ptr) {
  if constexpr (std::is_pointer<T>::value) {
    return detail::mem_mgr::instance().is_device_ptr(ptr);
  }
  return false;
}
#endif

/// Get the buffer and the offset of a piece of memory pointed to by \p ptr.
///
/// \param ptr Pointer to a piece of memory.
/// If NULL is passed as an argument, an exception will be thrown.
/// \returns a pair containing both the buffer and the offset.
static std::pair<buffer_t, size_t> get_buffer_and_offset(const void *ptr) {
  if (ptr) {
    auto alloc = detail::mem_mgr::instance().translate_ptr(ptr);
    size_t offset = (byte_t *)ptr - alloc.alloc_ptr;
    return std::make_pair(alloc.buffer, offset);
  } else {
    throw std::runtime_error(
        "NULL pointer argument in get_buffer_and_offset function is invalid");
  }
}

/// Get the data pointed from \p ptr as a 1D buffer reinterpreted as type T.
template <typename T> static cl::sycl::buffer<T> get_buffer(const void *ptr) {
  auto alloc = detail::mem_mgr::instance().translate_ptr(ptr);
  return alloc.buffer.reinterpret<T>(
      cl::sycl::range<1>(alloc.size / sizeof(T)));
}

/// Get the buffer of a piece of memory pointed to by \p ptr.
///
/// \param ptr Pointer to a piece of memory.
/// \returns the buffer.
static buffer_t get_buffer(const void *ptr) {
  return detail::mem_mgr::instance().translate_ptr(ptr).buffer;
}

/// A wrapper class contains an accessor and an offset.
template <typename dataT,
          cl::sycl::access_mode accessMode = cl::sycl::access_mode::read_write>
class access_wrapper {
  cl::sycl::accessor<byte_t, 1, accessMode> accessor;
  size_t offset;

public:
  /// Construct the accessor wrapper for memory pointed by \p ptr.
  ///
  /// \param ptr Pointer to memory.
  /// \param cgh The command group handler.
  access_wrapper(const void *ptr, cl::sycl::handler &cgh)
      : accessor(get_buffer(ptr).get_access<accessMode>(cgh)), offset(0) {
    auto alloc = detail::mem_mgr::instance().translate_ptr(ptr);
    offset = (byte_t *)ptr - alloc.alloc_ptr;
  }

  /// Get the device pointer.
  ///
  /// \returns a device pointer with offset.
  dataT get_raw_pointer() const { return (dataT)(&accessor[0] + offset); }
};

/// Get the accessor for memory pointed by \p ptr.
///
/// \param ptr Pointer to memory.
/// If NULL is passed as an argument, an exception will be thrown.
/// \param cgh The command group handler.
/// \returns an accessor.
template <cl::sycl::access_mode accessMode = cl::sycl::access_mode::read_write>
static cl::sycl::accessor<byte_t, 1, accessMode>
get_access(const void *ptr, cl::sycl::handler &cgh) {
  if (ptr) {
    auto alloc = detail::mem_mgr::instance().translate_ptr(ptr);
    return alloc.buffer.get_access<accessMode>(cgh);
  } else {
    throw std::runtime_error(
        "NULL pointer argument in get_access function is invalid");
  }
}

/// Allocate memory block on the device.
/// \param num_bytes Number of bytes to allocate.
/// \param q Queue to execute the allocate task.
/// \returns A pointer to the newly allocated memory.
template <typename T>
static inline void *dpct_malloc(T num_bytes,
                                cl::sycl::queue &q = get_default_queue()) {
  return detail::dpct_malloc(static_cast<size_t>(num_bytes), q);
}

/// Get the host pointer from a buffer that is mapped to virtual pointer ptr.
/// \param ptr Virtual Pointer mapped to device buffer
/// \returns A host pointer
template <typename T> static inline T *get_host_ptr(const void *ptr) {
  auto BufferOffset = get_buffer_and_offset(ptr);
  auto host_ptr =
      BufferOffset.first.get_access<cl::sycl::access_mode::read_write>()
          .get_pointer();
  return (T *)(host_ptr + BufferOffset.second);
}

/// Allocate memory block for 3D array on the device.
/// \param size Size of the memory block, in bytes.
/// \param q Queue to execute the allocate task.
/// \returns A pitched_data object which stores the memory info.
static inline pitched_data
dpct_malloc(cl::sycl::range<3> size, cl::sycl::queue &q = get_default_queue()) {
  pitched_data pitch(nullptr, 0, size.get(0), size.get(1));
  size_t pitch_size;
  pitch.set_data_ptr(detail::dpct_malloc(pitch_size, size.get(0), size.get(1),
                                         size.get(2), q));
  pitch.set_pitch(pitch_size);
  return pitch;
}

/// Allocate memory block for 2D array on the device.
/// \param [out] pitch Aligned size of x in bytes.
/// \param x Range in dim x.
/// \param y Range in dim y.
/// \param q Queue to execute the allocate task.
/// \returns A pointer to the newly allocated memory.
static inline void *dpct_malloc(size_t &pitch, size_t x, size_t y,
                                cl::sycl::queue &q = get_default_queue()) {
  return detail::dpct_malloc(pitch, x, y, 1, q);
}

/// free
/// \param ptr Point to free.
/// \param q Queue to execute the free task.
/// \returns no return value.
static inline void dpct_free(void *ptr,
                             cl::sycl::queue &q = get_default_queue()) {
  if (ptr) {
#ifdef DPCT_USM_LEVEL_NONE
    detail::mem_mgr::instance().mem_free(ptr);
#else
    cl::sycl::free(ptr, q.get_context());
#endif // DPCT_USM_LEVEL_NONE
  }
}

#ifndef DPCT_USM_LEVEL_NONE
/// Free the device memory pointed by a batch of pointers in \p pointers which
/// are related to \p q after \p events completed.
///
/// \param pointers The pointers point to the device memory requested to be freed.
/// \param events The events to be waited.
/// \param q The sycl::queue the memory relates to.
inline void async_dpct_free(std::vector<void *> pointers,
                            std::vector<cl::sycl::event> events,
                            cl::sycl::queue &q = get_default_queue()) {
  std::thread t(
      [](std::vector<void *> pointers, std::vector<cl::sycl::event> events,
         cl::sycl::context ctxt) {
        cl::sycl::event::wait(events);
        for (auto p : pointers)
          cl::sycl::free(p, ctxt);
      },
      std::move(pointers), std::move(events), q.get_context());
  get_current_device().add_task(std::move(t));
}
#endif

/// Synchronously copies \p size bytes from the address specified by \p from_ptr
/// to the address specified by \p to_ptr. The value of \p direction is used to
/// set the copy direction, it can be \a host_to_host, \a host_to_device,
/// \a device_to_host, \a device_to_device or \a automatic. The function will
/// return after the copy is completed.
///
/// \param to_ptr Pointer to destination memory address.
/// \param from_ptr Pointer to source memory address.
/// \param size Number of bytes to be copied.
/// \param direction Direction of the copy.
/// \param q Queue to execute the copy task.
/// \returns no return value.
static void dpct_memcpy(void *to_ptr, const void *from_ptr, size_t size,
                        memcpy_direction direction = automatic,
                        cl::sycl::queue &q = get_default_queue()) {
  detail::dpct_memcpy(q, to_ptr, from_ptr, size, direction).wait();
}

/// Asynchronously copies \p size bytes from the address specified by \p
/// from_ptr to the address specified by \p to_ptr. The value of \p direction is
/// used to set the copy direction, it can be \a host_to_host, \a
/// host_to_device, \a device_to_host, \a device_to_device or \a automatic. The
/// return of the function does NOT guarantee the copy is completed.
///
/// \param to_ptr Pointer to destination memory address.
/// \param from_ptr Pointer to source memory address.
/// \param size Number of bytes to be copied.
/// \param direction Direction of the copy.
/// \param q Queue to execute the copy task.
/// \returns no return value.
static void async_dpct_memcpy(void *to_ptr, const void *from_ptr, size_t size,
                              memcpy_direction direction = automatic,
                              cl::sycl::queue &q = dpct::get_default_queue()) {
  detail::dpct_memcpy(q, to_ptr, from_ptr, size, direction);
}

/// Synchronously copies 2D matrix specified by \p x and \p y from the address
/// specified by \p from_ptr to the address specified by \p to_ptr, while \p
/// from_pitch and \p to_pitch are the range of dim x in bytes of the matrix
/// specified by \p from_ptr and \p to_ptr. The value of \p direction is used to
/// set the copy direction, it can be \a host_to_host, \a host_to_device, \a
/// device_to_host, \a device_to_device or \a automatic. The function will
/// return after the copy is completed.
///
/// \param to_ptr Pointer to destination memory address.
/// \param to_pitch Range of dim x in bytes of destination matrix.
/// \param from_ptr Pointer to source memory address.
/// \param from_pitch Range of dim x in bytes of source matrix.
/// \param x Range of dim x of matrix to be copied.
/// \param y Range of dim y of matrix to be copied.
/// \param direction Direction of the copy.
/// \param q Queue to execute the copy task.
/// \returns no return value.
static inline void dpct_memcpy(void *to_ptr, size_t to_pitch,
                               const void *from_ptr, size_t from_pitch,
                               size_t x, size_t y,
                               memcpy_direction direction = automatic,
                               cl::sycl::queue &q = dpct::get_default_queue()) {
  cl::sycl::event::wait(detail::dpct_memcpy(q, to_ptr, from_ptr, to_pitch,
                                            from_pitch, x, y, direction));
}

/// Asynchronously copies 2D matrix specified by \p x and \p y from the address
/// specified by \p from_ptr to the address specified by \p to_ptr, while \p
/// \p from_pitch and \p to_pitch are the range of dim x in bytes of the matrix
/// specified by \p from_ptr and \p to_ptr. The value of \p direction is used to
/// set the copy direction, it can be \a host_to_host, \a host_to_device, \a
/// device_to_host, \a device_to_device or \a automatic. The return of the
/// function does NOT guarantee the copy is completed.
///
/// \param to_ptr Pointer to destination memory address.
/// \param to_pitch Range of dim x in bytes of destination matrix.
/// \param from_ptr Pointer to source memory address.
/// \param from_pitch Range of dim x in bytes of source matrix.
/// \param x Range of dim x of matrix to be copied.
/// \param y Range of dim y of matrix to be copied.
/// \param direction Direction of the copy.
/// \param q Queue to execute the copy task.
/// \returns no return value.
static inline void
async_dpct_memcpy(void *to_ptr, size_t to_pitch, const void *from_ptr,
                  size_t from_pitch, size_t x, size_t y,
                  memcpy_direction direction = automatic,
                  cl::sycl::queue &q = get_default_queue()) {
  detail::dpct_memcpy(q, to_ptr, from_ptr, to_pitch, from_pitch, x, y,
                      direction);
}

/// Synchronously copies a subset of a 3D matrix specified by \p to to another
/// 3D matrix specified by \p from. The from and to position info are specified
/// by \p from_pos and \p to_pos The copied matrix size is specified by \p size.
/// The value of \p direction is used to set the copy direction, it can be \a
/// host_to_host, \a host_to_device, \a device_to_host, \a device_to_device or
/// \a automatic. The function will return after the copy is completed.
///
/// \param to Destination matrix info.
/// \param to_pos Position of destination.
/// \param from Source matrix info.
/// \param from_pos Position of destination.
/// \param size Range of the submatrix to be copied.
/// \param direction Direction of the copy.
/// \param q Queue to execute the copy task.
/// \returns no return value.
static inline void dpct_memcpy(pitched_data to, cl::sycl::id<3> to_pos,
                               pitched_data from, cl::sycl::id<3> from_pos,
                               cl::sycl::range<3> size,
                               memcpy_direction direction = automatic,
                               cl::sycl::queue &q = dpct::get_default_queue()) {
  cl::sycl::event::wait(
      detail::dpct_memcpy(q, to, to_pos, from, from_pos, size, direction));
}

/// Asynchronously copies a subset of a 3D matrix specified by \p to to another
/// 3D matrix specified by \p from. The from and to position info are specified
/// by \p from_pos and \p to_pos The copied matrix size is specified by \p size.
/// The value of \p direction is used to set the copy direction, it can be \a
/// host_to_host, \a host_to_device, \a device_to_host, \a device_to_device or
/// \a automatic. The return of the function does NOT guarantee the copy is
/// completed.
///
/// \param to Destination matrix info.
/// \param to_pos Position of destination.
/// \param from Source matrix info.
/// \param from_pos Position of destination.
/// \param size Range of the submatrix to be copied.
/// \param direction Direction of the copy.
/// \param q Queue to execute the copy task.
/// \returns no return value.
static inline void
async_dpct_memcpy(pitched_data to, cl::sycl::id<3> to_pos, pitched_data from,
                  cl::sycl::id<3> from_pos, cl::sycl::range<3> size,
                  memcpy_direction direction = automatic,
                  cl::sycl::queue &q = get_default_queue()) {
  detail::dpct_memcpy(q, to, to_pos, from, from_pos, size, direction);
}

/// Synchronously sets \p value to the first \p size bytes starting from \p
/// dev_ptr. The function will return after the memset operation is completed.
///
/// \param dev_ptr Pointer to the device memory address.
/// \param value Value to be set.
/// \param size Number of bytes to be set to the value.
/// \param q The queue in which the operation is done.
/// \returns no return value.
static void dpct_memset(void *dev_ptr, int value, size_t size,
                        cl::sycl::queue &q = get_default_queue()) {
  detail::dpct_memset(q, dev_ptr, value, size).wait();
}

/// Asynchronously sets \p value to the first \p size bytes starting from \p
/// dev_ptr. The return of the function does NOT guarantee the memset operation
/// is completed.
///
/// \param dev_ptr Pointer to the device memory address.
/// \param value Value to be set.
/// \param size Number of bytes to be set to the value.
/// \returns no return value.
static void async_dpct_memset(void *dev_ptr, int value, size_t size,
                              cl::sycl::queue &q = dpct::get_default_queue()) {
  detail::dpct_memset(q, dev_ptr, value, size);
}

/// Sets \p value to the 2D memory region pointed by \p ptr in \p q. \p x and
/// \p y specify the setted 2D memory size. \p pitch is the bytes in linear
/// dimension, including padding bytes. The function will return after the
/// memset operation is completed.
///
/// \param ptr Pointer to the device memory region.
/// \param pitch Bytes in linear dimension, including padding bytes.
/// \param value Value to be set.
/// \param x The setted memory size in linear dimension.
/// \param y The setted memory size in second dimension.
/// \param q The queue in which the operation is done.
/// \returns no return value.
static inline void dpct_memset(void *ptr, size_t pitch, int val, size_t x,
                               size_t y,
                               cl::sycl::queue &q = get_default_queue()) {
  cl::sycl::event::wait(detail::dpct_memset(q, ptr, pitch, val, x, y));
}

/// Sets \p value to the 2D memory region pointed by \p ptr in \p q. \p x and
/// \p y specify the setted 2D memory size. \p pitch is the bytes in linear
/// dimension, including padding bytes. The return of the function does NOT
/// guarantee the memset operation is completed.
///
/// \param ptr Pointer to the device memory region.
/// \param pitch Bytes in linear dimension, including padding bytes.
/// \param value Value to be set.
/// \param x The setted memory size in linear dimension.
/// \param y The setted memory size in second dimension.
/// \param q The queue in which the operation is done.
/// \returns no return value.
static inline void async_dpct_memset(void *ptr, size_t pitch, int val, size_t x,
                                     size_t y,
                                     cl::sycl::queue &q = get_default_queue()) {
  detail::dpct_memset(q, ptr, pitch, val, x, y);
}

/// Sets \p value to the 3D memory region specified by \p pitch in \p q. \p size
/// specify the setted 3D memory size. The function will return after the
/// memset operation is completed.
///
/// \param pitch Specify the 3D memory region.
/// \param value Value to be set.
/// \param size The setted 3D memory size.
/// \param q The queue in which the operation is done.
/// \returns no return value.
static inline void dpct_memset(pitched_data pitch, int val,
                               cl::sycl::range<3> size,
                               cl::sycl::queue &q = get_default_queue()) {
  cl::sycl::event::wait(detail::dpct_memset(q, pitch, val, size));
}

/// Sets \p value to the 3D memory region specified by \p pitch in \p q. \p size
/// specify the setted 3D memory size. The return of the function does NOT
/// guarantee the memset operation is completed.
///
/// \param pitch Specify the 3D memory region.
/// \param value Value to be set.
/// \param size The setted 3D memory size.
/// \param q The queue in which the operation is done.
/// \returns no return value.
static inline void async_dpct_memset(pitched_data pitch, int val,
                                     cl::sycl::range<3> size,
                                     cl::sycl::queue &q = get_default_queue()) {
  detail::dpct_memset(q, pitch, val, size);
}

/// dpct accessor used as device function parameter.
template <class T, memory_region Memory, size_t Dimension> class accessor;
template <class T, memory_region Memory> class accessor<T, Memory, 3> {
public:
  using memory_t = detail::memory_traits<Memory, T>;
  using element_t = typename memory_t::element_t;
  using pointer_t = typename memory_t::pointer_t;
  using accessor_t = typename memory_t::template accessor_t<3>;
  accessor(pointer_t data, const cl::sycl::range<3> &in_range)
      : _data(data), _range(in_range) {}
  template <memory_region M = Memory>
  accessor(typename std::enable_if<M != local, const accessor_t>::type &acc)
      : accessor(acc, acc.get_range()) {}
  accessor(const accessor_t &acc, const cl::sycl::range<3> &in_range)
      : accessor(acc.get_pointer(), in_range) {}
  accessor<T, Memory, 2> operator[](size_t index) const {
    cl::sycl::range<2> sub(_range.get(1), _range.get(2));
    return accessor<T, Memory, 2>(_data + index * sub.size(), sub);
  }

  pointer_t get_ptr() const { return _data; }

private:
  pointer_t _data;
  cl::sycl::range<3> _range;
};
template <class T, memory_region Memory> class accessor<T, Memory, 2> {
public:
  using memory_t = detail::memory_traits<Memory, T>;
  using element_t = typename memory_t::element_t;
  using pointer_t = typename memory_t::pointer_t;
  using accessor_t = typename memory_t::template accessor_t<2>;
  accessor(pointer_t data, const cl::sycl::range<2> &in_range)
      : _data(data), _range(in_range) {}
  template <memory_region M = Memory>
  accessor(typename std::enable_if<M != local, const accessor_t>::type &acc)
      : accessor(acc, acc.get_range()) {}
  accessor(const accessor_t &acc, const cl::sycl::range<2> &in_range)
      : accessor(acc.get_pointer(), in_range) {}

  pointer_t operator[](size_t index) const {
    return _data + _range.get(1) * index;
  }

  pointer_t get_ptr() const { return _data; }

private:
  pointer_t _data;
  cl::sycl::range<2> _range;
};

namespace detail {
/// Device variable with address space of shared, global or constant.
template <class T, memory_region Memory, size_t Dimension>
class device_memory {
public:
  using accessor_t =
      typename detail::memory_traits<Memory, T>::template accessor_t<Dimension>;
  using value_t = typename detail::memory_traits<Memory, T>::value_t;
  using dpct_accessor_t = dpct::accessor<T, Memory, Dimension>;

  device_memory() : device_memory(cl::sycl::range<Dimension>(1)) {}

  /// Constructor of 1-D array with initializer list
  device_memory(
      const cl::sycl::range<Dimension> &in_range,
      std::initializer_list<value_t> &&init_list)
      : device_memory(in_range) {
    assert(init_list.size() <= in_range.size());
    _host_ptr = (value_t *)std::malloc(_size);
    std::memset(_host_ptr, 0, _size);
    std::memcpy(_host_ptr, init_list.begin(), init_list.size() * sizeof(T));
  }

  /// Constructor of 2-D array with initializer list
  template <size_t D = Dimension>
  device_memory(
      const typename std::enable_if<D == 2, cl::sycl::range<2>>::type &in_range,
      std::initializer_list<std::initializer_list<value_t>> &&init_list)
      : device_memory(in_range) {
    assert(init_list.size() <= in_range[0]);
    _host_ptr = (value_t *)std::malloc(_size);
    std::memset(_host_ptr, 0, _size);
    auto tmp_data = _host_ptr;
    for (auto sub_list : init_list) {
      assert(sub_list.size() <= in_range[1]);
      std::memcpy(tmp_data, sub_list.begin(), sub_list.size() * sizeof(T));
      tmp_data += in_range[1];
    }
  }

  /// Constructor with range
  device_memory(const cl::sycl::range<Dimension> &range_in)
      : _size(range_in.size() * sizeof(T)), _range(range_in), _reference(false),
        _host_ptr(nullptr), _device_ptr(nullptr) {
    static_assert(
        (Memory == global) || (Memory == constant) || (Memory == shared),
        "device memory region should be global, constant or shared");
    // Make sure that singleton class mem_mgr and dev_mgr will destruct later
    // than this.
    detail::mem_mgr::instance();
    dev_mgr::instance();
  }

  /// Constructor with range
  template <class... Args>
  device_memory(Args... Arguments)
      : device_memory(cl::sycl::range<Dimension>(Arguments...)) {}

  ~device_memory() {
    if (_device_ptr && !_reference)
      dpct_free(_device_ptr);
    if (_host_ptr)
      std::free(_host_ptr);
  }

  /// Allocate memory with default queue, and init memory if has initial value.
  void init() {
    init(dpct::get_default_queue());
  }
  /// Allocate memory with specified queue, and init memory if has initial value.
  void init(cl::sycl::queue &q) {
    if (_device_ptr)
      return;
    if (!_size)
      return;
    allocate_device(q);
    if (_host_ptr)
      detail::dpct_memcpy(q, _device_ptr, _host_ptr, _size, host_to_device);
  }

  /// The variable is assigned to a device pointer.
  void assign(value_t *src, size_t size) {
    this->~device_memory();
    new (this) device_memory(src, size);
  }

  /// Get memory pointer of the memory object, which is virtual pointer when
  /// usm is not used, and device pointer when usm is used.
  value_t *get_ptr() {
    return get_ptr(get_default_queue());
  }
  /// Get memory pointer of the memory object, which is virtual pointer when
  /// usm is not used, and device pointer when usm is used.
  value_t *get_ptr(cl::sycl::queue &q) {
    init(q);
    return _device_ptr;
  }

  /// Get the device memory object size in bytes.
  size_t get_size() { return _size; }

  template <size_t D = Dimension>
  typename std::enable_if<D == 1, T>::type &operator[](size_t index) {
    init();
#ifdef DPCT_USM_LEVEL_NONE
    return dpct::get_buffer<typename std::enable_if<D == 1, T>::type>(
               _device_ptr)
        .template get_access<sycl::access_mode::read_write>()[index];
#else
    return _device_ptr[index];
#endif // DPCT_USM_LEVEL_NONE
  }

#ifdef DPCT_USM_LEVEL_NONE
  /// Get cl::sycl::accessor for the device memory object when usm is not used.
  accessor_t get_access(cl::sycl::handler &cgh) {
    return get_buffer(_device_ptr)
        .template reinterpret<T, Dimension>(_range)
        .template get_access<detail::memory_traits<Memory, T>::mode,
                             detail::memory_traits<Memory, T>::target>(cgh);
  }
#else
  /// Get dpct::accessor with dimension info for the device memory object
  /// when usm is used and dimension is greater than 1.
  template <size_t D = Dimension>
  typename std::enable_if<D != 1, dpct_accessor_t>::type
  get_access(cl::sycl::handler &cgh) {
    return dpct_accessor_t((T *)_device_ptr, _range);
  }
#endif // DPCT_USM_LEVEL_NONE

private:
  device_memory(value_t *memory_ptr, size_t size)
      : _size(size), _range(size / sizeof(T)), _reference(true),
        _device_ptr(memory_ptr) {}

  void allocate_device(cl::sycl::queue &q) {
#ifndef DPCT_USM_LEVEL_NONE
    if (Memory == shared) {
      _device_ptr = (value_t *)cl::sycl::malloc_shared(
          _size, q.get_device(), q.get_context());
      return;
    }
#endif
    _device_ptr = (value_t *)detail::dpct_malloc(_size, q);
  }

  size_t _size;
  cl::sycl::range<Dimension> _range;
  bool _reference;
  value_t *_host_ptr;
  value_t *_device_ptr;
};
template <class T, memory_region Memory>
class device_memory<T, Memory, 0> : public device_memory<T, Memory, 1> {
public:
  using base = device_memory<T, Memory, 1>;
  using value_t = typename base::value_t;
  using accessor_t =
      typename detail::memory_traits<Memory, T>::template accessor_t<0>;

  /// Constructor with initial value.
  device_memory(const value_t &val) : base(cl::sycl::range<1>(1), {val}) {}

  /// Default constructor
  device_memory() : base(1) {}

#ifdef DPCT_USM_LEVEL_NONE
  /// Get cl::sycl::accessor for the device memory object when usm is not used.
  accessor_t get_access(cl::sycl::handler &cgh) {
    auto buf = get_buffer(base::get_ptr())
                   .template reinterpret<T, 1>(cl::sycl::range<1>(1));
    return accessor_t(buf, cgh);
  }
#endif // DPCT_USM_LEVEL_NONE
};
}

template <class T, size_t Dimension>
using global_memory = detail::device_memory<T, global, Dimension>;
template <class T, size_t Dimension>
using constant_memory = detail::device_memory<T, constant, Dimension>;
template <class T, size_t Dimension>
using shared_memory = detail::device_memory<T, shared, Dimension>;
} // namespace dpct

#endif // __DPCT_MEMORY_HPP__
