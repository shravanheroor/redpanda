#include "storage/segment_offset_index.h"

#include "storage/logger.h"
#include "storage/segment_offset_index_utils.h"
#include "vassert.h"

#include <seastar/core/fstream.hh>

#include <bits/stdint-uintn.h>
#include <boost/container/container_fwd.hpp>
#include <fmt/format.h>

#include <algorithm>

namespace storage {

segment_offset_index::segment_offset_index(
  ss::sstring filename, ss::file f, model::offset base, size_t step)
  : _name(std::move(filename))
  , _out(std::move(f))
  , _base(base)
  , _step(step) {}

void segment_offset_index::maybe_track(
  model::offset o, size_t pos, size_t data_size) {
    vassert(
      o >= _base,
      "cannot track offsets that are lower than our base, o:{}, _base:{}",
      o,
      _base);
    if (!_positions.empty()) {
        // check if this is an earlier offset; ignore if so
        const uint32_t i = o() - _base();
        if (_positions.back().first > i) {
            return;
        }
    }
    _last_seen_offset = o;
    _acc += data_size;
    if (_acc >= _step) {
        _acc = 0;
        // We know that a segment cannot be > 4GB
        _positions.emplace_back(
          static_cast<uint32_t>(o() - _base()), // NOLINT
          static_cast<uint32_t>(pos));          // NOLINT
        _needs_persistence = true;
    }
}
struct base_comparator {
    // lower bound bool
    bool
    operator()(const std::pair<uint32_t, uint32_t>& p, uint32_t needle) const {
        return p.first < needle;
    }
    // upper bound bool
    bool
    operator()(uint32_t needle, const std::pair<uint32_t, uint32_t>& p) const {
        return needle < p.first;
    }
};
std::optional<std::pair<model::offset, size_t>>
segment_offset_index::lower_bound_pair(model::offset o) {
    vassert(
      o >= _base,
      "segment_offset::index::lower_bound cannot find offset:{} below:{}",
      o,
      _base);
    if (_positions.empty()) {
        return std::nullopt;
    }
    const uint32_t i = o() - _base();
    auto it = std::lower_bound(
      std::begin(_positions), std::end(_positions), i, base_comparator{});
    if (it == _positions.end()) {
        it = std::prev(it);
    }
    if (it->first <= i) {
        return std::make_pair(_base + model::offset(it->first), it->second);
    }
    if (std::distance(_positions.begin(), it) > 0) {
        it = std::prev(it);
    }
    if (it->first <= i) {
        return std::make_pair(_base + model::offset(it->first), it->second);
    }
    return std::nullopt;
}

std::optional<size_t> segment_offset_index::lower_bound(model::offset o) {
    auto opt = lower_bound_pair(o);
    if (opt) {
        return opt->second;
    }
    return std::nullopt;
}

ss::future<> segment_offset_index::truncate(model::offset o) {
    vassert(
      o >= _base,
      "segment_offset_index::truncate cannot find offset:{} below:{}",
      o,
      _base);
    _last_seen_offset = o;
    const uint32_t i = o() - _base();
    auto it = std::lower_bound(
      std::begin(_positions), std::end(_positions), i, base_comparator{});
    if (it != _positions.end()) {
        _needs_persistence = true;
        _positions.erase(it, _positions.end());
    }
    return flush();
}

ss::future<bool> segment_offset_index::materialize_index() {
    return _out.size()
      .then([this](uint64_t size) mutable {
          return _out.dma_read_bulk<char>(0, size);
      })
      .then([this](ss::temporary_buffer<char> buf) {
          if (buf.empty()) {
              return false;
          }
          _positions = offset_index_from_buf(std::move(buf));
          return true;
      });
}

ss::future<> segment_offset_index::flush() {
    if (!_needs_persistence) {
        return ss::make_ready_future<>();
    }
    _needs_persistence = false;
    return _out.truncate(0).then([this] {
        auto b = offset_index_to_buf(_positions);
        auto out = ss::make_lw_shared<ss::output_stream<char>>(
          ss::make_file_output_stream(ss::file(_out.dup())));
        return out->write(b.get(), b.size())
          .then([out] { return out->flush(); })
          .then([out] { return out->close(); })
          .finally([out] {});
    });
}
ss::future<> segment_offset_index::close() {
    return flush().then([this] { return _out.close(); });
}
std::ostream& operator<<(std::ostream& o, const segment_offset_index& i) {
    return o << "{file:" << i.filename() << ", offsets:" << i.base_offset()
             << "-" << i.last_seen_offset()
             << ", indexed_offsets:" << i.indexed_offsets()
             << ", step:" << i.step()
             << ", needs_persistence:" << i.needs_persistence() << "}";
}
std::ostream& operator<<(std::ostream& o, const segment_offset_index_ptr& i) {
    if (i) {
        return o << "{ptr=" << *i << "}";
    }
    return o << "{ptr=nullptr}";
}

} // namespace storage