// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pg {

// The diary's save slots on disk (specs/formats/savegame.md; the engine's own format).
// Slot 1 is "The Beginning..." (a new game) and is never written. Files are
// <dir>/pagan_NNN.sav. On the web the folder is an IDBFS mount; on_write then syncs it.
class SaveStore {
public:
    static constexpr int kSlots = 96;  // the diary's 16 x 6 entries (1 = new game)
    // Engine slots after the diary's (app/debug_tools.h): three rotating autosaves and the
    // debug menu's snapshot. They are never offered for writing in the diary. kQuickSlot is the
    // quick-save (F7 / F8, v2.12 hotkeys): shown on the diary's autosave page, never a diary entry.
    static constexpr int kAutoFirst = 97, kAutoSlots = 3, kSnapshotSlot = 100, kQuickSlot = 101, kMaxSlot = 101;
    explicit SaveStore(std::string dir = "") : dir_(std::move(dir)) {}
    void set_dir(const std::string& d) { dir_ = d; refresh(); }
    const std::string& dir() const { return dir_; }
    std::string path(int slot) const;

    // Re-reads the descriptions of all slots.
    void refresh();
    bool exists(int slot) const { return slot > 1 && slot <= kMaxSlot && !desc_[size_t(slot)].empty(); }
    std::string description(int slot) const { return slot >= 0 && slot <= kMaxSlot ? desc_[size_t(slot)] : ""; }

    bool write(int slot, const std::vector<uint8_t>& data);
    bool read(int slot, std::vector<uint8_t>& data) const;

    // The newest autosave slot (0 if none): the "autosave_last" setting, else the newest file.
    int latest_autosave() const;
    // The most recently written save of any kind (diary or autosave; not the debug snapshot),
    // 0 if none: the "save_last" setting, else the newest file. The start menu's Continue.
    int latest() const;
    // The most recently written diary entry (2..kSlots; 0 if none): "save_last" when it is one,
    // else the newest diary file. Write Diary opens on the first free entry after it.
    int latest_diary() const;
    int last_slot = 0;              // most recently saved or loaded (the diary opens there)
    std::function<void()> on_write;  // called after a successful write (web: sync to IndexedDB)

private:
    std::string dir_;
    std::vector<std::string> desc_ = std::vector<std::string>(kMaxSlot + 1);
};

// The header every save starts with; description() reads only this.
struct SaveHeader {
    static constexpr const char* kMagic = "PAGANSAV";
    static constexpr uint32_t kVersion = 6;  // 2: sound and music state (milestone 5); 3: visited maps; 4: companions; 5: world mode (WRLD); 6: levels (LVL1)
    std::string description;
    unsigned map = 0;
    uint32_t game_frames = 0;
    std::string avatar_name;
    // Parses the header; false if this is not a save of a known version.
    static bool read(const std::vector<uint8_t>& data, SaveHeader& out);
};

}  // namespace pg
