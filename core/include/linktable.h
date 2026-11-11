// MeatNet link/unlink tables (messages 0x73-0x7B), port of linktable.py.
// Stored as JSON (same format as the Python units' links.json); where it is
// kept is up to the platform, through `saver`.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "protocol.h"

namespace core {

class LinkTable {
public:
    std::vector<protocol::LinkEntry> linked, unlinked;
    bool proximity = false;
    // Called with the JSON text after every change; unset = not persisted.
    std::function<void(const std::string &)> saver;

    void load(const std::string &json);     // bad or empty input leaves the table empty
    std::string to_json() const;
    void save() const;

    bool link(const protocol::LinkEntry &e);     // true if anything changed
    bool unlink(const protocol::LinkEntry &e);
    // Merge entries pulled from a peer. A node never stores its own serial:
    // an entry naming us refers to the sending peer instead, and is dropped
    // if the sender is unknown (empty).
    bool merge(const std::vector<protocol::LinkEntry> &entries, bool linked_list,
               const std::string &own_serial10, const protocol::Bytes &sender_serial10);

private:
    bool put(std::vector<protocol::LinkEntry> &lst, std::vector<protocol::LinkEntry> &other,
             const protocol::LinkEntry &e);
};

}  // namespace core
