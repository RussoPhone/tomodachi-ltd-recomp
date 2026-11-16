// SPDX-License-Identifier: GPL-3.0-or-later
// SaveStore: writes become the save only on Commit, and every interrupted commit is finished or undone on the next
// start (issue #21: a save cut between Mii.sav/Player.sav and Map.sav lost the island layout).
#include "services/save_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using nxrt::hle::SaveStore;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

static void Put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

static std::string Get(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "<missing>";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Dirs {
    fs::path base, committed, scratch;
    explicit Dirs(const char* name) {
        base = fs::temp_directory_path() / ("nxrt-save-store-test-" + std::string(name));
        fs::remove_all(base);
        committed = base / "save" / "1" / "prog" / "user" / "1";
        scratch = base / ".nxrt-save" / "1" / "prog" / "user" / "1";
    }
    ~Dirs() { fs::remove_all(base); }
    SaveStore Start() const {
        SaveStore store(committed, scratch);
        CHECK(store.Open());
        return store;
    }
};

// The game's save pattern: three files rewritten in place, one Commit at the end.
static void SaveAll(const SaveStore& store, const std::string& tag) {
    Put(store.Live() / "Mii.sav", "mii-" + tag);
    Put(store.Live() / "Player.sav", "player-" + tag);
    Put(store.Live() / "Map.sav", "map-" + tag);
}

static void FreshSaveCommits() {
    Dirs d("fresh");
    SaveStore store = d.Start();
    CHECK(fs::is_directory(d.committed));
    CHECK(fs::is_empty(store.Live()));
    SaveAll(store, "1");
    CHECK(Get(d.committed / "Map.sav") == "<missing>");  // not a save before Commit
    CHECK(store.Commit());
    CHECK(Get(d.committed / "Mii.sav") == "mii-1");
    CHECK(Get(d.committed / "Map.sav") == "map-1");
    CHECK(!fs::exists(d.scratch / "tmp") && !fs::exists(d.scratch / "new") && !fs::exists(d.scratch / "old"));
    // The working copy keeps going after a commit (the game may keep its files open).
    CHECK(Get(store.Live() / "Map.sav") == "map-1");
}

// Killed after rewriting Mii.sav and Player.sav but before Map.sav and Commit: the whole save is the previous one.
static void KillBeforeCommitKeepsPreviousSave() {
    Dirs d("kill");
    {
        SaveStore store = d.Start();
        SaveAll(store, "1");
        CHECK(store.Commit());
        Put(store.Live() / "Mii.sav", "mii-2");
        Put(store.Live() / "Player.sav", "player-2");
    }  // process gone, no Commit
    SaveStore next = d.Start();
    CHECK(Get(d.committed / "Mii.sav") == "mii-1");
    CHECK(Get(d.committed / "Player.sav") == "player-1");
    CHECK(Get(d.committed / "Map.sav") == "map-1");
    CHECK(Get(next.Live() / "Mii.sav") == "mii-1");  // the uncommitted write is gone, as on the console
}

static void ExistingSaveIsAdopted() {
    Dirs d("legacy");
    Put(d.committed / "Mii.sav", "mii-old");
    Put(d.committed / "Map.sav", "map-old");
    SaveStore store = d.Start();
    CHECK(Get(store.Live() / "Mii.sav") == "mii-old");
    CHECK(Get(store.Live() / "Map.sav") == "map-old");
    CHECK(Get(d.committed / "Map.sav") == "map-old");
}

static void SecondCommitReplaces() {
    Dirs d("twice");
    SaveStore store = d.Start();
    SaveAll(store, "1");
    CHECK(store.Commit());
    SaveAll(store, "2");
    fs::remove(store.Live() / "Player.sav");
    CHECK(store.Commit());
    CHECK(Get(d.committed / "Map.sav") == "map-2");
    CHECK(!fs::exists(d.committed / "Player.sav"));  // deletions are committed too
}

// Killed while copying the working copy: the half copy is dropped, the previous save stands.
static void KillDuringCopy() {
    Dirs d("copy");
    Put(d.committed / "Map.sav", "map-1");
    Put(d.scratch / "tmp" / "Map.sav", "map-2-half");
    d.Start();
    CHECK(Get(d.committed / "Map.sav") == "map-1");
    CHECK(!fs::exists(d.scratch / "tmp"));
}

// Killed after the new copy was sealed but before the swap: the commit is finished on the next start.
static void KillBeforeSwap() {
    Dirs d("seal");
    Put(d.committed / "Map.sav", "map-1");
    Put(d.scratch / "new" / "Map.sav", "map-2");
    SaveStore store = d.Start();
    CHECK(Get(d.committed / "Map.sav") == "map-2");
    CHECK(Get(store.Live() / "Map.sav") == "map-2");
    CHECK(!fs::exists(d.scratch / "new") && !fs::exists(d.scratch / "old"));
}

// Killed between the two renames: no committed folder, the old one aside, the new one sealed.
static void KillMidSwap() {
    Dirs d("swap");
    Put(d.scratch / "old" / "Map.sav", "map-1");
    Put(d.scratch / "new" / "Map.sav", "map-2");
    d.Start();
    CHECK(Get(d.committed / "Map.sav") == "map-2");
    CHECK(!fs::exists(d.scratch / "old"));
}

// Killed after the swap, before the old copy was removed.
static void KillAfterSwap() {
    Dirs d("after");
    Put(d.committed / "Map.sav", "map-2");
    Put(d.scratch / "old" / "Map.sav", "map-1");
    d.Start();
    CHECK(Get(d.committed / "Map.sav") == "map-2");
    CHECK(!fs::exists(d.scratch / "old"));
}

// A lone `old` with no committed folder and nothing sealed: put the previous save back rather than start empty.
static void LoneOldIsRestored() {
    Dirs d("lone");
    Put(d.scratch / "old" / "Map.sav", "map-1");
    d.Start();
    CHECK(Get(d.committed / "Map.sav") == "map-1");
}

static void OneStorePerFolder() {
    Dirs d("acquire");
    auto a = nxrt::hle::AcquireSaveStore(d.committed, d.scratch);
    auto b = nxrt::hle::AcquireSaveStore(d.committed, d.scratch);
    CHECK(a && a == b);  // a second OpenSaveDataFileSystem must not wipe the first one's working copy
    Put(a->Live() / "Map.sav", "map-1");
    auto c = nxrt::hle::AcquireSaveStore(d.committed, d.scratch);
    CHECK(Get(c->Live() / "Map.sav") == "map-1");
}

int main() {
    FreshSaveCommits();
    KillBeforeCommitKeepsPreviousSave();
    ExistingSaveIsAdopted();
    SecondCommitReplaces();
    KillDuringCopy();
    KillBeforeSwap();
    KillMidSwap();
    KillAfterSwap();
    LoneOldIsRestored();
    OneStorePerFolder();
    nxrt::hle::HoldSaveCommitsForExit();  // idle: returns at once
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("save_store: all checks passed");
    return 0;
}
