// SPDX-License-Identifier: GPL-3.0-or-later
#include "services/save_store.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "common.h"

namespace nxrt::hle {

namespace fs = std::filesystem;

namespace {

// One lock for every commit in the process: commits are rare (a save in the game) and short (a few MB).
std::timed_mutex g_commit_mutex;

// Antivirus scanners, sync clients (OneDrive) and the launcher's save watcher can hold a save file or folder for a
// moment, which makes a rename fail on Windows. Retry for about a second before giving up.
constexpr int kAttempts = 20;
constexpr auto kAttemptPause = std::chrono::milliseconds(50);

// Paths stay out of the log (they contain the user's home folder); the step and the system's message are enough.
void LogFailure(const char* step, const std::error_code& ec) {
    LOG_ERROR("save: %s failed: %s", step, ec ? ec.message().c_str() : "unknown error");
}

bool Remove(const fs::path& path, const char* step) {
    std::error_code ec;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        ec.clear();
        fs::remove_all(path, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(kAttemptPause);
    }
    LogFailure(step, ec);
    return false;
}

bool Rename(const fs::path& from, const fs::path& to, const char* step) {
    std::error_code ec;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        ec.clear();
        fs::rename(from, to, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(kAttemptPause);
    }
    LogFailure(step, ec);
    return false;
}

bool Exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

// Renames a folder into the committed place, whose parent may not exist yet (first run, or the save folder deleted).
bool RenameInto(const fs::path& from, const fs::path& to, const char* step) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    return Rename(from, to, step);
}

// Pushes a file's contents to the disk. Without it a power loss right after the renames could leave the new names
// pointing at files whose data never reached the disk.
bool SyncFile(const fs::path& path) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

// Makes renames and new entries in a folder durable. Windows has no equivalent; NTFS journals its metadata.
void SyncDir(const fs::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
#else
    (void)path;
#endif
}

// Copies a save folder and flushes every file it wrote. `to` must not exist.
bool CopyTree(const fs::path& from, const fs::path& to, bool durable) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        LogFailure("creating a folder", ec);
        return false;
    }
    std::vector<fs::path> dirs{to};
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path target = to / fs::relative(it->path(), from, ec);
        if (ec) break;
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
            dirs.push_back(target);
        } else if (it->is_regular_file(ec)) {
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
            if (!ec && durable && !SyncFile(target)) ec = std::make_error_code(std::errc::io_error);
        }
        if (ec) break;
    }
    if (ec) {
        LogFailure("copying the save", ec);
        return false;
    }
    if (durable)
        for (const fs::path& dir : dirs) SyncDir(dir);
    return true;
}

} // namespace

SaveStore::SaveStore(fs::path committed, fs::path scratch)
    : committed_(std::move(committed)), scratch_(std::move(scratch)), live_(scratch_ / "live"),
      tmp_(scratch_ / "tmp"), new_(scratch_ / "new"), old_(scratch_ / "old") {}

// `new` is only ever a complete, flushed copy (it gets that name by a rename from `tmp`), so a `new` that is still
// here is a commit that was decided but not swapped in: finish it. A lone `old` without a committed folder means the
// swap stopped halfway with no `new` to put in place: put the previous save back.
bool SaveStore::Recover() {
    if (!Remove(tmp_, "dropping an unfinished copy")) return false;
    if (Exists(new_)) return Promote();
    if (!Exists(committed_) && Exists(old_)) {
        if (!RenameInto(old_, committed_, "restoring the previous save")) return false;
        SyncDir(committed_.parent_path());
    }
    return Remove(old_, "removing the previous save");
}

// Swaps `new` in as the committed save: committed -> old, new -> committed, drop old.
bool SaveStore::Promote() {
    if (Exists(committed_)) {
        if (!Remove(old_, "removing the previous save")) return false;
        if (!Rename(committed_, old_, "setting the previous save aside")) return false;
    }
    if (!RenameInto(new_, committed_, "publishing the save")) return false;
    SyncDir(committed_.parent_path());
    SyncDir(scratch_);
    return Remove(old_, "removing the previous save");
}

bool SaveStore::Open() {
    std::lock_guard<std::timed_mutex> lock(g_commit_mutex);
    std::error_code ec;
    fs::create_directories(scratch_, ec);
    if (ec) {
        LogFailure("creating the save work folder", ec);
        return false;
    }
    if (!Recover()) return false;
    fs::create_directories(committed_, ec);
    if (ec) {
        LogFailure("creating the save folder", ec);
        return false;
    }
    // Whatever is in `live` was written after the last commit by a session that ended without committing it: on the
    // console it would be gone too.
    if (!Remove(live_, "dropping uncommitted writes")) return false;
    if (!CopyTree(committed_, live_, false)) return false;
    LOG_INFO("save: opened (writes are kept on commit)");
    return true;
}

bool SaveStore::Commit() {
    std::lock_guard<std::timed_mutex> lock(g_commit_mutex);
    if (!Remove(tmp_, "dropping an unfinished copy")) return false;
    if (!CopyTree(live_, tmp_, true)) {
        Remove(tmp_, "dropping an unfinished copy");
        return false;
    }
    SyncDir(scratch_);
    // A `new` left by an earlier commit whose swap failed is older than what was just copied: replace it.
    if (!Remove(new_, "replacing a pending save")) return false;
    if (!Rename(tmp_, new_, "sealing the save")) return false;
    SyncDir(scratch_);
    // From here the commit is durable: if the swap fails (a folder held by another program), `new` stays and the next
    // commit or the next start puts it in place.
    if (!Promote()) LOG_WARN("save: committed, but the folder swap is postponed to the next commit or start");
    return true;
}

std::shared_ptr<SaveStore> AcquireSaveStore(const fs::path& committed, const fs::path& scratch) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<SaveStore>> stores;
    std::lock_guard<std::mutex> lock(mutex);
    const std::string key = committed.lexically_normal().generic_string();
    auto found = stores.find(key);
    if (found != stores.end()) return found->second;
    auto store = std::make_shared<SaveStore>(committed, scratch);
    if (!store->Open()) return nullptr;
    stores.emplace(key, store);
    return store;
}

void HoldSaveCommitsForExit() {
    // Deliberately never released: the process is about to end. If a commit is stuck for longer than this, exiting
    // anyway is still safe, because an interrupted commit is finished or undone on the next start.
    if (!g_commit_mutex.try_lock_for(std::chrono::seconds(5)))
        LOG_WARN("save: a commit was still running when the game closed; it will be completed on the next start");
}

} // namespace nxrt::hle
