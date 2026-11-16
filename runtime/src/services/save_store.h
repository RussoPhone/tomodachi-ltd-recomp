// SPDX-License-Identifier: GPL-3.0-or-later
// Save data with the console's commit semantics.
//
// On the Switch a save file system is journaled: the game overwrites its files in place and then calls Commit, and only
// Commit makes the new contents the save. If the software is closed (or crashes, or loses power) before Commit, the
// previous save is still there, complete. Tomodachi Life relies on this: it rewrites Mii.sav, Player.sav and Map.sav one
// after the other and commits once at the end.
//
// A SaveStore gives the game a working copy ("live") to read and write, and publishes it as the committed save only on
// Commit, by copying it to a scratch folder, flushing it to disk and swapping it in with two renames. Every step leaves a
// state that Open() can finish or undo, so a save is always either the previous commit or the new one, never a mix.
//
//   committed: <user>/save/<space>/<program>/<user id>/<type>      (what backups and the launcher see)
//   scratch:   <user>/.nxrt-save/<space>/<program>/<user id>/<type>/{live,tmp,new,old}
#pragma once

#include <filesystem>
#include <memory>

namespace nxrt::hle {

class SaveStore {
public:
    SaveStore(std::filesystem::path committed, std::filesystem::path scratch);

    // Finishes or undoes a commit that was interrupted, drops writes that were never committed and starts a fresh
    // working copy of the committed save. False if the working copy could not be made: the caller must then refuse to
    // open the save rather than hand the game an empty folder (it would take that as "no save" and start over).
    bool Open();

    // Publishes the working copy as the committed save. False if nothing was published (the previous commit stands).
    bool Commit();

    const std::filesystem::path& Live() const { return live_; }
    const std::filesystem::path& Committed() const { return committed_; }

private:
    bool Recover();
    bool Promote();

    std::filesystem::path committed_, scratch_, live_, tmp_, new_, old_;
};

// The one store of a save folder in this process, opened on first use. Null if it could not be opened.
std::shared_ptr<SaveStore> AcquireSaveStore(const std::filesystem::path& committed,
                                            const std::filesystem::path& scratch);

// Called right before the process ends on purpose (window closed, HOME, the game exiting): waits for a commit in
// progress to finish (up to a few seconds) and keeps new ones from starting, so the exit never cuts a commit in half.
void HoldSaveCommitsForExit();

} // namespace nxrt::hle
