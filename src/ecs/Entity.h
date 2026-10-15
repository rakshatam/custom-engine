#pragma once
#include <cstdint>

namespace Engine {

using EntityID = uint32_t;
constexpr EntityID NULL_ENTITY = 0xFFFFFFFF;

struct Entity {
    EntityID id = NULL_ENTITY;
    bool isValid() const { return id != NULL_ENTITY; }
};

} // namespace Engine
