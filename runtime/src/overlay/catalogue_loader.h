// Get mods: the catalogue list, shown at once from the local cache and checked for new mods in the
// background (mods_model.h CataloguePolicy). Only the public index is fetched; packages are downloaded
// on Install / Update only. No ImGui: tested by runtime/tools/mods_model_test.cpp.
#pragma once
#include "../mods/catalogue_client.h"
#include "mods_model.h"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace overlay {
class CatalogueLoader {
public:
    struct Snapshot {
        bool have_list = false;           // a list (cached or fresh) is shown
        bool fresh = false;               // checked this session
        bool checking = false;
        bool failed = false;              // the last check failed (the list, if any, is the cached one)
        std::string error;                // technical reason (for the Details of More)
        mods::catalogue::Loaded catalogue;
        uint64_t generation = 0;          // changes whenever the list changes
    };

    ~CatalogueLoader() { join(); }

    // Every frame Get mods (or the Mods tab) is shown. The first call shows the cache; a check starts
    // when the policy allows it. source: catalogue URL (or a local index for fixtures).
    void open(const std::string& source, const std::filesystem::path& work, const mods::catalogue::Fetch& fetch,
              double now, bool automatic) {
        std::unique_lock lock(mutex_);
        policy_.automatic = automatic;
        if (source != source_) {  // another catalogue: its own cache, its own check
            source_ = source;
            snapshot_ = {};
            snapshot_.generation = ++generation_;
            policy_.last_check = -1;
            cache_read_ = false;
        }
        if (!cache_read_) {
            cache_read_ = true;
            try {
                snapshot_.catalogue = mods::catalogue::cached(source, work);
                snapshot_.have_list = true;
                snapshot_.generation = ++generation_;
            } catch (const std::exception&) {}  // no cache yet
        }
        if (policy_.should_fetch(now)) start(lock, source, work, fetch, now);
    }
    // Refresh (and Retry): check now, whatever the policy says.
    void refresh(const std::string& source, const std::filesystem::path& work, const mods::catalogue::Fetch& fetch,
                 double now) {
        {
            std::lock_guard lock(mutex_);
            if (policy_.checking) return;
        }
        if (source != source_) open(source, work, fetch, now, false);  // shows that source's cache first
        std::unique_lock lock(mutex_);
        if (!policy_.checking) start(lock, source, work, fetch, now);
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }
    bool checking() const { std::lock_guard lock(mutex_); return policy_.checking; }
    int fetches() const { std::lock_guard lock(mutex_); return fetches_; }
    void join() {
        std::thread t;
        { std::lock_guard lock(mutex_); t = std::move(thread_); }
        if (t.joinable()) t.join();
    }

private:
    void start(std::unique_lock<std::mutex>& lock, const std::string& source, const std::filesystem::path& work,
               const mods::catalogue::Fetch& fetch, double now) {
        if (thread_.joinable()) {  // the previous check has finished (checking is false); reap it
            std::thread old = std::move(thread_);
            lock.unlock();
            old.join();
            lock.lock();
        }
        policy_.checking = true;
        policy_.last_check = now;  // the hour counts from the start of a check
        snapshot_.checking = true;
        ++fetches_;
        thread_ = std::thread([this, source, work, fetch] {
            mods::catalogue::Loaded result;
            std::string error;
            try {
                result = mods::catalogue::refresh_cached(source, fetch, work);
            } catch (const std::exception& e) {
                error = e.what();
                if (error.empty()) error = "Catalogue check failed";
            }
            std::lock_guard lock(mutex_);
            policy_.checking = false;
            snapshot_.checking = false;
            if (source != source_) return;  // the source changed meanwhile
            if (error.empty()) {
                snapshot_.catalogue = std::move(result);
                snapshot_.have_list = snapshot_.fresh = true;
                snapshot_.failed = false;
                snapshot_.error.clear();
                snapshot_.generation = ++generation_;
                fprintf(stderr, "[catalogue] refreshed %zu entries\n", snapshot_.catalogue.index.entries.size());
            } else {
                snapshot_.failed = true;
                snapshot_.error = error;
            }
        });
    }

    mutable std::mutex mutex_;
    std::thread thread_;
    std::string source_;
    bool cache_read_ = false;
    modsmodel::CataloguePolicy policy_;
    Snapshot snapshot_;
    uint64_t generation_ = 0;
    int fetches_ = 0;
};
}  // namespace overlay
