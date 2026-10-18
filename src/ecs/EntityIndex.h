#pragma once

#include "Entity.h"
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine {

// Checked stable-ID -> dense render-array index lookup. IDs are monotonic for
// a Scene lifetime; a rebuild is transactional and rejects null/duplicate IDs.
class EntityIndex {
public:
    bool rebuild(const std::vector<EntityID>& ids) {
        std::unordered_map<EntityID, std::size_t> next;
        next.reserve(ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const EntityID id = ids[index];
            if (id == NULL_ENTITY || !next.emplace(id, index).second) return false;
        }
        m_indexById.swap(next);
        return true;
    }

    std::optional<std::size_t> find(EntityID id) const {
        const auto it = m_indexById.find(id);
        if (it == m_indexById.end()) return std::nullopt;
        return it->second;
    }

    void clear() { m_indexById.clear(); }
    std::size_t size() const { return m_indexById.size(); }

private:
    std::unordered_map<EntityID, std::size_t> m_indexById;
};

} // namespace Engine
