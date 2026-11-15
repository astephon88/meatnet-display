// The Linux unit's files: its settings (core::Settings as JSON) and helpers
// to read and (atomically) write a file.
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

#include "cJSON.h"
#include "unit_settings.h"

namespace linux_unit {

inline std::string read_file(const std::string &path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool write_file(const std::string &path, const std::string &text)
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);     // e.g. host/ in a fresh clone
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!(f << text)) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

// The settings file, loaded at start and rewritten on every change from the
// web page.
class SettingsFile {
public:
    explicit SettingsFile(std::string path) : path_(std::move(path)) { core::merge_settings(s_, read_file(path_)); }
    core::Settings get()
    {
        std::lock_guard<std::mutex> g(mu_);
        return s_;
    }
    // Command-line overrides: in effect, but not written to the file.
    void override(const std::string &role, const std::optional<std::string> &peer,
                  const std::optional<std::string> &key)
    {
        std::lock_guard<std::mutex> g(mu_);
        role_ = role;
        peer_ = peer;
        key_ = key;
        apply_overrides(s_);
    }
    bool update(const cJSON *req, std::string &err, bool &reboot)
    {
        std::lock_guard<std::mutex> g(mu_);
        core::Settings saved;
        core::merge_settings(saved, read_file(path_));
        if (!core::apply_config(saved, req, err, reboot)) return false;
        cJSON *o = core::settings_to_json(saved, true);
        char *text = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        bool written = text && write_file(path_, text);
        cJSON_free(text);
        if (!written) {
            err = "could not write " + path_;
            return false;
        }
        s_ = saved;
        apply_overrides(s_);
        return true;
    }

private:
    void apply_overrides(core::Settings &s)
    {
        s.role = role_;
        if (peer_) s.split_peer = *peer_;
        if (key_) s.split_key = *key_;
    }
    std::string path_, role_;
    std::optional<std::string> peer_, key_;
    std::mutex mu_;
    core::Settings s_;
};

}  // namespace linux_unit
