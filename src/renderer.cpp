// LightView: a PDF and DjVu viewer. Copyright (C) 2026 Kalachev Gleb.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "renderer.h"

#include <algorithm>

namespace lv {

Renderer::Renderer(HWND notify, UINT msg) : notify_(notify), msg_(msg) {
  thread_ = std::thread([this] { run(); });
}

Renderer::~Renderer() {
  {
    std::lock_guard g(mu_);
    stop_ = true;
    abort_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void Renderer::set_document(std::shared_ptr<Document> doc, bool keep_stale) {
  std::lock_guard g(mu_);
  doc_ = std::move(doc);
  ++gen_;
  queue_.clear();
  abort_ = true;
  if (!keep_stale) {
    cache_.clear();
    bytes_ = 0;
  }
}

void Renderer::set_budget(size_t bytes) {
  std::lock_guard g(mu_);
  budget_ = bytes;
  evict_locked();
}

void Renderer::request(std::vector<TileRequest> reqs) {
  {
    std::lock_guard g(mu_);
    // Drop what is already rendered for the current document.
    reqs.erase(std::remove_if(reqs.begin(), reqs.end(),
                              [&](const TileRequest& r) {
                                auto it = cache_.find(r.key);
                                return it != cache_.end() && it->second->gen == gen_;
                              }),
               reqs.end());
    queue_ = std::move(reqs);
    if (busy_ && std::none_of(queue_.begin(), queue_.end(), [&](const TileRequest& r) { return r.key == busy_key_; })) {
      abort_ = true;
    }
  }
  cv_.notify_one();
}

std::shared_ptr<const Tile> Renderer::find(const TileKey& key, bool* fresh) {
  std::lock_guard g(mu_);
  auto it = cache_.find(key);
  if (it == cache_.end()) return nullptr;
  it->second->last_use = ++clock_;
  if (fresh) *fresh = it->second->gen == gen_;
  return it->second;
}

void Renderer::page_tiles(int page, int rot, std::vector<std::shared_ptr<const Tile>>& out) {
  std::lock_guard g(mu_);
  for (auto& [k, t] : cache_) {
    if (k.page == page && k.rot == rot) out.push_back(t);
  }
}

void Renderer::evict_locked() {
  if (bytes_ <= budget_) return;
  std::vector<std::pair<uint64_t, TileKey>> order;
  order.reserve(cache_.size());
  for (auto& [k, t] : cache_) order.push_back({t->last_use + (t->gen == gen_ ? (1ull << 40) : 0), k});
  std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first < b.first; });
  const size_t target = budget_ - budget_ / 8;
  for (auto& [use, k] : order) {
    if (bytes_ <= target) break;
    auto it = cache_.find(k);
    bytes_ -= it->second->px.size();
    cache_.erase(it);
  }
}

void Renderer::run() {
  std::unique_lock lk(mu_);
  for (;;) {
    cv_.wait(lk, [&] { return stop_ || (!queue_.empty() && doc_); });
    if (stop_) return;
    TileRequest r = queue_.front();
    queue_.erase(queue_.begin());
    auto cached = cache_.find(r.key);
    if (cached != cache_.end() && cached->second->gen == gen_) continue;

    std::shared_ptr<Document> doc = doc_;
    const uint32_t gen = gen_;
    busy_ = true;
    busy_key_ = r.key;
    abort_ = false;
    lk.unlock();

    auto tile = std::make_shared<Tile>();
    tile->key = r.key;
    tile->full_h = r.full_h;
    tile->x = r.x;
    tile->y = r.y;
    tile->w = r.w;
    tile->h = r.h;
    tile->px.resize((size_t)r.w * r.h * 4);
    bool ok = doc->render(r.key.page, r.key.full_w, r.full_h, r.key.rot, r.x, r.y, r.w, r.h, tile->px.data(), &abort_);
    doc.reset();  // a replaced document is destroyed here, outside the cache lock

    lk.lock();
    busy_ = false;
    if (!ok || gen != gen_ || stop_) continue;
    tile->gen = gen;
    tile->last_use = ++clock_;
    auto it = cache_.find(r.key);
    if (it != cache_.end()) {
      bytes_ -= it->second->px.size();
      it->second = tile;
    } else {
      cache_.emplace(r.key, tile);
    }
    bytes_ += tile->px.size();
    evict_locked();
    PostMessageW(notify_, msg_, (WPARAM)r.key.page, 0);
  }
}

}  // namespace lv
