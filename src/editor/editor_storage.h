// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
#pragma once
// Where the edit mode keeps painted areas and the world layout (docs/editor_storage.md).
// Every call takes a completion callback, so a network backend can answer
// later; the backends here answer at once.
// Each stored document has a version number: a save names the version it was based on and is
// refused as a conflict when the stored one has moved on (base_version < 0 forces).
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pg {

struct StoredInfo {
    std::string id;
    int version = 0;
    std::string saved_at;  // ISO 8601 UTC
    std::string name;      // the area's name, when known
};

struct StorageResult {
    bool ok = false;
    bool conflict = false;  // save / delete refused: `version` is the stored version
    int version = 0;        // loaded / saved / current version
    std::string text;       // loaded JSON
    std::string err;
    std::vector<StoredInfo> list;  // list / history
};

class EditorStorage {
public:
    using Done = std::function<void(const StorageResult&)>;
    virtual ~EditorStorage() = default;
    virtual std::string name() const = 0;
    // False while the backend is still starting (the browser's IndexedDB being read).
    virtual bool ready() const { return true; }

    virtual void list_areas(Done done) = 0;
    virtual void load_area(const std::string& id, Done done) = 0;
    virtual void save_area(const std::string& id, const std::string& json, int base_version, Done done) = 0;
    virtual void delete_area(const std::string& id, int base_version, Done done) = 0;
    virtual void area_history(const std::string& id, Done done) = 0;
    virtual void load_area_version(const std::string& id, int version, Done done) = 0;

    virtual void load_layout(Done done) = 0;
    virtual void save_layout(const std::string& json, int base_version, Done done) = 0;
    virtual void layout_history(Done done) = 0;
    virtual void load_layout_version(int version, Done done) = 0;

    // Called after every successful write (the web build syncs IndexedDB from it).
    std::function<void()> on_write;

    // Network backends (editor/server_storage.h) answer from a local copy and upload later.
    // EditMode::save calls after_save once its writes are done (explicit_save: false for an
    // autosave; force: overwrite what the server refused). on_notice shows a message in the
    // editor; on_conflict makes the next Save overwrite (like a conflict answered at once).
    virtual void after_save(bool explicit_save, bool force) { (void)explicit_save; (void)force; }
    std::function<void(const std::string& msg, int ticks)> on_notice;
    std::function<void()> on_conflict;
};

// Sets the top-level "revision" field of a JSON document's text (adds it after the opening
// brace if missing) without reformatting the rest; reads it back (0 if none).
std::string stamp_revision(const std::string& json, int revision);
int read_revision(const std::string& json);
bool valid_storage_id(const std::string& id);
std::string utc_now_iso();

// Everything in memory (the tests; a scratch session). Keeps every version.
class MemoryStorage : public EditorStorage {
public:
    std::string name() const override { return "memory"; }
    void list_areas(Done done) override;
    void load_area(const std::string& id, Done done) override;
    void save_area(const std::string& id, const std::string& json, int base_version, Done done) override;
    void delete_area(const std::string& id, int base_version, Done done) override;
    void area_history(const std::string& id, Done done) override;
    void load_area_version(const std::string& id, int version, Done done) override;
    void load_layout(Done done) override;
    void save_layout(const std::string& json, int base_version, Done done) override;
    void layout_history(Done done) override;
    void load_layout_version(int version, Done done) override;
    int writes = 0;

private:
    struct Doc {
        std::vector<std::pair<StoredInfo, std::string>> versions;  // oldest first
        bool deleted = false;
    };
    void save(Doc& d, const std::string& id, const std::string& json, int base, Done& done);
    std::map<std::string, Doc> areas_;
    Doc layout_;
};

// Files: <areas_dir>/<id>.json, <layout_path>, and old versions in <history_dir>/<id>/<v>.json
// (<history_dir>/_layout/ for the layout; the newest kMaxHistory are kept). Native:
// engine/data/areas, engine/data/world_layout.json, ~/.pagan/editor_history. Web: the same
// under /editor, a folder kept in IndexedDB (IDBFS), synced through on_write.
class FileStorage : public EditorStorage {
public:
    static constexpr int kMaxHistory = 20;
    FileStorage(std::string areas_dir, std::string layout_path, std::string history_dir);
    std::string name() const override { return "files " + areas_dir_; }
    bool ready() const override { return !ready_check || ready_check(); }
    std::function<bool()> ready_check;

    void list_areas(Done done) override;
    void load_area(const std::string& id, Done done) override;
    void save_area(const std::string& id, const std::string& json, int base_version, Done done) override;
    void delete_area(const std::string& id, int base_version, Done done) override;
    void area_history(const std::string& id, Done done) override;
    void load_area_version(const std::string& id, int version, Done done) override;
    void load_layout(Done done) override;
    void save_layout(const std::string& json, int base_version, Done done) override;
    void layout_history(Done done) override;
    void load_layout_version(int version, Done done) override;

    const std::string& areas_dir() const { return areas_dir_; }
    const std::string& layout_path() const { return layout_path_; }

private:
    void save_doc(const std::string& path, const std::string& hist_key, const std::string& json, int base, Done& done);
    void history_of(const std::string& hist_key, Done& done);
    std::string areas_dir_, layout_path_, history_dir_;
};

}  // namespace pg
