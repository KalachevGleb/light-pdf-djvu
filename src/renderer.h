// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "document.h"

namespace lv {

// A tile is identified by the page, the view rotation and the full page width in pixels (which
// fixes the scale), plus its column/row in the tile grid.
struct TileKey {
  int page = 0, rot = 0, full_w = 0, tx = 0, ty = 0;
  bool operator==(const TileKey&) const = default;
};

struct TileKeyHash {
  size_t operator()(const TileKey& k) const {
    uint64_t h = (uint64_t)k.page * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)k.full_w << 20) ^ ((uint64_t)k.rot << 18) ^ ((uint64_t)k.tx << 9) ^ (uint64_t)k.ty;
    return (size_t)(h ^ (h >> 29));
  }
};

struct Tile {
  TileKey key;
  int full_h = 0;
  int x = 0, y = 0, w = 0, h = 0;  // position inside the full page bitmap
  std::vector<uint8_t> px;         // BGRx, top-down, stride w*4
  uint32_t gen = 0;
  uint64_t last_use = 0;
};

struct TileRequest {
  TileKey key;
  int full_h = 0;
  int x = 0, y = 0, w = 0, h = 0;
};

class Renderer {
 public:
  static constexpr int kTileSize = 1024;

  Renderer(HWND notify, UINT msg);
  ~Renderer();

  // keep_stale: old tiles stay drawable as placeholders until re-rendered (used on reload).
  void set_document(std::shared_ptr<Document> doc, bool keep_stale);
  void set_budget(size_t bytes);

  // Replaces the pending queue (ordered by priority). Aborts the tile being rendered if it is no
  // longer wanted.
  void request(std::vector<TileRequest> reqs);

  std::shared_ptr<const Tile> find(const TileKey& key, bool* fresh);
  void page_tiles(int page, int rot, std::vector<std::shared_ptr<const Tile>>& out);

 private:
  void run();
  void evict_locked();

  HWND notify_;
  UINT msg_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::shared_ptr<Document> doc_;
  uint32_t gen_ = 1;
  std::vector<TileRequest> queue_;
  std::unordered_map<TileKey, std::shared_ptr<Tile>, TileKeyHash> cache_;
  size_t bytes_ = 0;
  size_t budget_ = 192u << 20;
  uint64_t clock_ = 0;
  bool busy_ = false;
  TileKey busy_key_;
  std::atomic<bool> abort_{false};
  std::thread thread_;
};

}  // namespace lv
