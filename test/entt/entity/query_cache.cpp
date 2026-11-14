#include <algorithm>
#include <vector>
#include <gtest/gtest.h>
#include <entt/entity/entity.hpp>
#include <entt/entity/query_cache.hpp>
#include <entt/entity/registry.hpp>
#include <entt/entity/view.hpp>

namespace {

struct position {
    int x{};
    int y{};
};

struct velocity {
    int x{};
    int y{};
};

struct dead {};

struct stable {
    static constexpr auto in_place_delete = true;
    int value{};
};

template<typename... Get, typename... Exclude>
std::vector<entt::entity> collect(entt::registry &registry, entt::exclude_t<Exclude...> excludes = entt::exclude_t<Exclude...>{}) {
    std::vector<entt::entity> entities{};

    for(const auto entt: registry.view<Get...>(excludes)) {
        entities.push_back(entt);
    }

    return entities;
}

} // namespace

TEST(QueryCache, EmptyRegistry) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    ASSERT_TRUE(cache.empty());
    ASSERT_TRUE(cache.query<position>().empty());
    ASSERT_TRUE((cache.query<position, velocity>(entt::exclude<dead>)).empty());

    ASSERT_EQ(cache.size(), 2u);
    ASSERT_EQ(cache.hits(), 0u);
    ASSERT_EQ(cache.misses(), 2u);

    ASSERT_TRUE(cache.query<position>().empty());
    ASSERT_EQ(cache.hits(), 1u);
}

TEST(QueryCache, ConsistencyWithView) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    for(std::size_t next{}; next < 16u; ++next) {
        const auto entity = registry.create();

        if(next % 2u == 0u) {
            registry.emplace<position>(entity, 1, 2);
        }

        if(next % 3u == 0u) {
            registry.emplace<velocity>(entity, 3, 4);
        }
    }

    ASSERT_EQ(cache.query<position>(), collect<position>(registry));
    ASSERT_EQ(cache.query<velocity>(), collect<velocity>(registry));
    ASSERT_EQ((cache.query<position, velocity>()), (collect<position, velocity>(registry)));
    ASSERT_EQ(cache.query<position>(entt::exclude<velocity>), collect<position>(registry, entt::exclude<velocity>));

    // repeated queries are served from the cache and stay consistent
    ASSERT_EQ((cache.query<position, velocity>()), (collect<position, velocity>(registry)));
    ASSERT_EQ(cache.misses(), 4u);
    ASSERT_EQ(cache.hits(), 1u);
}

TEST(QueryCache, ConstQualifiedComponents) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<const position>(), cache.query<position>());
    ASSERT_EQ(cache.query<const position>(entt::exclude<const velocity>), cache.query<position>(entt::exclude<velocity>));
    ASSERT_EQ(cache.size(), 2u);
}

TEST(QueryCache, EntityCreated) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    ASSERT_TRUE((cache.query<position, velocity>()).empty());

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    // partial match: the entity is not part of the result yet
    ASSERT_TRUE((cache.query<position, velocity>()).empty());

    registry.emplace<velocity>(entity, 3, 4);

    ASSERT_EQ((cache.query<position, velocity>()), std::vector<entt::entity>{entity});
    ASSERT_EQ((cache.query<position, velocity>()), (collect<position, velocity>(registry)));
}

TEST(QueryCache, EntityDestroyed) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    const auto other = registry.create();
    registry.emplace<position>(entity, 1, 2);
    registry.emplace<velocity>(entity, 3, 4);
    registry.emplace<position>(other, 5, 6);
    registry.emplace<velocity>(other, 7, 8);

    ASSERT_EQ((cache.query<position, velocity>()).size(), 2u);

    registry.destroy(entity);

    ASSERT_EQ((cache.query<position, velocity>()), std::vector<entt::entity>{other});
    ASSERT_EQ((cache.query<position, velocity>()), (collect<position, velocity>(registry)));

    // destroying an entity that owns no relevant component doesn't invalidate the cache
    const auto hits = cache.hits();
    const auto lonely = registry.create();
    registry.destroy(lonely);

    ASSERT_EQ((cache.query<position, velocity>()), std::vector<entt::entity>{other});
    ASSERT_EQ(cache.hits(), hits + 1u);
}

TEST(QueryCache, ComponentAddRemove) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_TRUE((cache.query<position, velocity>()).empty());

    registry.emplace<velocity>(entity, 3, 4);

    ASSERT_EQ((cache.query<position, velocity>()), std::vector<entt::entity>{entity});

    registry.remove<velocity>(entity);

    ASSERT_TRUE((cache.query<position, velocity>()).empty());
    ASSERT_EQ(cache.query<position>(), std::vector<entt::entity>{entity});

    registry.erase<position>(entity);

    ASSERT_TRUE(cache.query<position>().empty());
}

TEST(QueryCache, ExcludeAddRemove) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<position>(entt::exclude<dead>), std::vector<entt::entity>{entity});

    registry.emplace<dead>(entity);

    ASSERT_TRUE(cache.query<position>(entt::exclude<dead>).empty());

    registry.remove<dead>(entity);

    ASSERT_EQ(cache.query<position>(entt::exclude<dead>), std::vector<entt::entity>{entity});
}

TEST(QueryCache, ComponentUpdate) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<position>(), std::vector<entt::entity>{entity});

    const auto misses = cache.misses();

    // updating a component value doesn't affect the matching set
    registry.replace<position>(entity, 3, 4);
    registry.patch<position>(entity, [](auto &pos) { pos.x = 5; });

    ASSERT_EQ(cache.query<position>(), std::vector<entt::entity>{entity});
    ASSERT_EQ(cache.misses(), misses);
    ASSERT_EQ(registry.get<position>(entity).x, 5);
}

TEST(QueryCache, RegistryClear) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);
    registry.emplace<velocity>(entity, 3, 4);

    ASSERT_EQ((cache.query<position, velocity>()).size(), 1u);

    registry.clear<velocity>();

    ASSERT_TRUE((cache.query<position, velocity>()).empty());
    ASSERT_EQ(cache.query<position>().size(), 1u);

    registry.clear();

    ASSERT_TRUE((cache.query<position, velocity>()).empty());
    ASSERT_TRUE(cache.query<position>().empty());
}

TEST(QueryCache, RecycledEntity) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<position>(), std::vector<entt::entity>{entity});

    registry.destroy(entity);
    entity = registry.create();

    // the recycled identifier doesn't leak into the cached result
    ASSERT_TRUE(cache.query<position>().empty());

    registry.emplace<position>(entity, 3, 4);

    ASSERT_EQ(cache.query<position>(), std::vector<entt::entity>{entity});
    ASSERT_EQ(cache.query<position>(), collect<position>(registry));
}

TEST(QueryCache, MultipleCompositions) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<position>().size(), 1u);
    ASSERT_TRUE((cache.query<position, velocity>()).empty());

    const auto hits = cache.hits();
    const auto misses = cache.misses();

    // touches velocity only: invalidates <position, velocity> but not <position>
    registry.emplace<velocity>(entity, 3, 4);

    ASSERT_EQ(cache.query<position>(), std::vector<entt::entity>{entity});
    ASSERT_EQ((cache.query<position, velocity>()), std::vector<entt::entity>{entity});

    ASSERT_EQ(cache.hits(), hits + 1u);
    ASSERT_EQ(cache.misses(), misses + 1u);
}

TEST(QueryCache, Clear) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<position>().size(), 1u);
    ASSERT_EQ(cache.size(), 1u);

    cache.clear();

    ASSERT_TRUE(cache.empty());
    ASSERT_EQ(cache.size(), 0u);

    const auto misses = cache.misses();

    // the cache rebuilds from scratch and stays consistent
    ASSERT_EQ(cache.query<position>(), collect<position>(registry));
    ASSERT_EQ(cache.misses(), misses + 1u);

    // listeners have been disconnected: mutations no longer invalidate
    registry.emplace<velocity>(entity, 3, 4);

    ASSERT_EQ(cache.query<position>(), collect<position>(registry));
    ASSERT_EQ(cache.misses(), misses + 1u);
}

TEST(QueryCache, Refresh) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    registry.emplace<position>(entity, 1, 2);

    ASSERT_EQ(cache.query<position>().size(), 1u);

    const auto misses = cache.misses();

    ASSERT_EQ(cache.refresh<position>(), collect<position>(registry));
    ASSERT_EQ(cache.misses(), misses + 1u);

    ASSERT_EQ((cache.refresh<position, velocity>()), (collect<position, velocity>(registry)));
}

TEST(QueryCache, TombstoneComponent) {
    entt::registry registry{};
    entt::query_cache cache{registry};

    const auto entity = registry.create();
    const auto other = registry.create();
    registry.emplace<stable>(entity, 1);
    registry.emplace<stable>(other, 2);

    ASSERT_EQ(cache.query<stable>().size(), 2u);

    // in-place deletion leaves a tombstone behind: the cache stays consistent
    registry.erase<stable>(entity);

    ASSERT_EQ(cache.query<stable>(), collect<stable>(registry));
    ASSERT_EQ(cache.query<stable>(), std::vector<entt::entity>{other});

    registry.destroy(other);

    ASSERT_TRUE(cache.query<stable>().empty());
}

TEST(QueryCache, RandomizedConsistency) {
    entt::registry registry{};
    entt::query_cache cache{registry};
    std::vector<entt::entity> entities{};
    std::size_t counter{};

    // deterministic pseudo-random sequence of mutations
    const auto next = [&counter] { return (counter = counter * 1103515245u + 12345u) / 65536u % 32768u; };

    for(std::size_t step{}; step < 256u; ++step) {
        switch(next() % 4u) {
        case 0u: {
            const auto entity = registry.create();
            entities.push_back(entity);
            registry.emplace<position>(entity, 1, 2);

            if(next() % 2u == 0u) {
                registry.emplace<velocity>(entity, 3, 4);
            }

            if(next() % 3u == 0u) {
                registry.emplace<dead>(entity);
            }
        } break;
        case 1u:
            if(!entities.empty()) {
                const auto entity = entities[next() % entities.size()];

                if(registry.valid(entity)) {
                    registry.destroy(entity);
                }
            }
            break;
        case 2u:
            if(!entities.empty()) {
                const auto entity = entities[next() % entities.size()];

                if(registry.valid(entity)) {
                    if(registry.all_of<velocity>(entity)) {
                        registry.remove<velocity>(entity);
                    } else {
                        registry.emplace<velocity>(entity, 3, 4);
                    }
                }
            }
            break;
        default:
            if(!entities.empty()) {
                const auto entity = entities[next() % entities.size()];

                if(registry.valid(entity)) {
                    if(registry.all_of<dead>(entity)) {
                        registry.remove<dead>(entity);
                    } else {
                        registry.emplace<dead>(entity);
                    }
                }
            }
            break;
        }

        ASSERT_EQ(cache.query<position>(), collect<position>(registry)) << "step " << step;
        ASSERT_EQ((cache.query<position, velocity>()), (collect<position, velocity>(registry))) << "step " << step;
        ASSERT_EQ((cache.query<position, velocity>(entt::exclude<dead>)), (collect<position, velocity>(registry, entt::exclude<dead>))) << "step " << step;
    }
}
