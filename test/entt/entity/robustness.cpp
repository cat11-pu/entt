#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_set>
#include <vector>
#include <gtest/gtest.h>
#include <entt/entity/handle.hpp>
#include <entt/entity/registry.hpp>

namespace {

struct movable {
    int value{};
};

struct stable {
    int value{};
    static constexpr bool in_place_delete = true;
};

struct self_erase {
    entt::basic_storage<self_erase, entt::entity> *storage{};
    entt::entity self{entt::null};
    static constexpr bool in_place_delete = true;

    ~self_erase() {
        if(storage != nullptr) {
            storage->remove(self);
        }
    }
};

} // namespace

TEST(Robustness, RepeatedDestroy) {
    entt::registry registry;
    const auto entity = registry.create();

    registry.emplace<int>(entity, 1);
    registry.destroy(entity);
    const auto version = registry.current(entity);

    ASSERT_NO_THROW(registry.destroy(entity));
    ASSERT_NO_THROW(registry.destroy(entity, 3u));
    ASSERT_FALSE(registry.valid(entity));
    ASSERT_EQ(registry.current(entity), version);
}

TEST(Robustness, InvalidHandle) {
    entt::registry registry;
    const auto entity = registry.create();
    entt::handle handle{registry, entity};

    registry.destroy(entity);

    ASSERT_FALSE(handle);
    ASSERT_TRUE(handle.orphan());
    ASSERT_EQ(handle.remove<int>(), 0u);
    ASSERT_NO_THROW(handle.erase<int>());
    ASSERT_EQ(handle.try_get<int>(), nullptr);
    ASSERT_NO_THROW([[maybe_unused]] auto &value = handle.get_or_emplace<int>(3));
    ASSERT_NO_THROW(handle.destroy());
    ASSERT_FALSE(handle);

    entt::handle invalid{};
    ASSERT_FALSE(invalid);
    ASSERT_EQ(invalid.remove<int>(), 0u);
    ASSERT_EQ(invalid.try_get<int>(), nullptr);
    ASSERT_NO_THROW(invalid.destroy());
}

TEST(Robustness, RemoveCurrentDuringIteration) {
    entt::registry registry;
    std::array<entt::entity, 8u> entities{};
    std::array<int, 8u> visits{};

    registry.create(entities.begin(), entities.end());

    for(std::size_t pos{}; pos < entities.size(); ++pos) {
        registry.emplace<movable>(entities[pos], static_cast<int>(pos));
    }

    registry.view<movable>().each([&](const auto entt, auto &elem) {
        ++visits[static_cast<std::size_t>(elem.value)];

        if(elem.value == 2) {
            registry.remove<movable>(entt);
        }
    });

    ASSERT_EQ(registry.storage<movable>().size(), 7u);
    ASSERT_EQ(visits, (std::array<int, 8u>{1, 1, 1, 1, 1, 1, 1, 1}));
}

TEST(Robustness, MutateOtherDuringIteration) {
    entt::registry registry;
    std::array<entt::entity, 8u> entities{};
    std::unordered_set<int> visited;

    registry.create(entities.begin(), entities.end());

    for(std::size_t pos{}; pos < entities.size(); ++pos) {
        registry.emplace<movable>(entities[pos], static_cast<int>(pos));
    }

    registry.view<movable>().each([&](const auto, auto &elem) {
        visited.insert(elem.value);

        if(elem.value == 6) {
            registry.remove<movable>(entities[2u]);
        }
    });

    ASSERT_LE(visited.size(), entities.size());
    ASSERT_EQ(registry.storage<movable>().size(), 7u);
    ASSERT_FALSE(registry.all_of<movable>(entities[2u]));
}

TEST(Robustness, ClearAndShrinkDuringIteration) {
    entt::registry registry;
    std::array<entt::entity, 8u> entities{};
    std::size_t visits{};

    registry.create(entities.begin(), entities.end());

    for(const auto entt: entities) {
        registry.emplace<movable>(entt);
    }

    ASSERT_NO_THROW(registry.view<movable>().each([&](const auto, auto &) {
        ++visits;
        registry.clear<movable>();
        registry.storage<movable>().shrink_to_fit();
    }));
    ASSERT_EQ(visits, 1u);
    ASSERT_TRUE(registry.storage<movable>().empty());
}

TEST(Robustness, AddDuringIteration) {
    entt::registry registry;
    std::array<entt::entity, 4u> entities{};
    std::size_t visits{};

    registry.create(entities.begin(), entities.end());

    for(std::size_t pos{}; pos < entities.size(); ++pos) {
        registry.emplace<movable>(entities[pos], static_cast<int>(pos));
    }

    registry.view<movable>().each([&](const auto, auto &elem) {
        ++visits;

        for(int pos{}; pos < 1024; ++pos) {
            registry.emplace<movable>(registry.create(), 1000 + pos);
        }

        elem.value += 10;
    });

    ASSERT_EQ(visits, entities.size());
    ASSERT_EQ(registry.storage<movable>().size(), entities.size() + 4096u);
}

TEST(Robustness, ReentrantDestructor) {
    using storage_type = entt::basic_storage<self_erase, entt::entity>;
    storage_type storage;
    const auto entity = entt::entity{0};

    storage.emplace(entity);
    storage.get(entity).storage = &storage;
    storage.get(entity).self = entity;

    ASSERT_NO_THROW(storage.erase(entity));
    ASSERT_FALSE(storage.contains(entity));
}

TEST(Robustness, BoundaryScan) {
    for(std::size_t size{}; size <= 64u; ++size) {
        entt::registry registry;
        std::vector<entt::entity> entities(size);

        registry.create(entities.begin(), entities.end());

        for(std::size_t pos{}; pos < size; ++pos) {
            registry.emplace<movable>(entities[pos], static_cast<int>(pos));
        }

        for(std::size_t pos{}; pos < size; ++pos) {
            registry.emplace<stable>(entities[pos], static_cast<int>(pos));
        }

        std::size_t visits{};

        ASSERT_NO_THROW(registry.view<movable>().each([&](const auto entt, auto &elem) {
            ++visits;

            switch((elem.value + static_cast<int>(size)) % 4) {
            case 0:
                registry.remove<movable>(entt);
                break;
            case 1:
                registry.emplace<stable>(entt, elem.value);
                break;
            case 2:
                registry.destroy(entt);
                break;
            default:
                registry.emplace<movable>(entt, elem.value);
                break;
            }
        }));

        ASSERT_LE(visits, size);
        ASSERT_NO_THROW(registry.clear());

        for(const auto entt: entities) {
            ASSERT_FALSE(registry.valid(entt));
        }
    }
}
