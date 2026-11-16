#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <entt/entity/command_buffer.hpp>
#include <entt/entity/registry.hpp>

namespace {

struct allocation_state {
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t live_bytes{};
};

template<typename Type>
class counting_allocator {
    template<typename>
    friend class counting_allocator;

public:
    using value_type = Type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    counting_allocator()
        : state{std::make_shared<allocation_state>()} {}

    explicit counting_allocator(std::shared_ptr<allocation_state> value)
        : state{std::move(value)} {}

    template<typename Other>
    counting_allocator(const counting_allocator<Other> &other) noexcept
        : state{other.state} {}

    [[nodiscard]] Type *allocate(const std::size_t length) {
        auto *value = std::allocator<Type>{}.allocate(length);
        const auto bytes = length * sizeof(Type);
        ++state->allocations;
        state->live_bytes += bytes;
        return value;
    }

    void deallocate(Type *value, const std::size_t length) noexcept {
        const auto bytes = length * sizeof(Type);
        ++state->deallocations;
        state->live_bytes -= bytes;
        std::allocator<Type>{}.deallocate(value, length);
    }

    template<typename Other>
    [[nodiscard]] bool operator==(const counting_allocator<Other> &other) const noexcept {
        return state == other.state;
    }

private:
    std::shared_ptr<allocation_state> state;
};

struct error final {};

} // namespace

struct CommandBuffer: testing::Test {
    struct position {
        int x{};
        int y{};

        [[nodiscard]] friend bool operator==(const position &lhs, const position &rhs) noexcept {
            return lhs.x == rhs.x && lhs.y == rhs.y;
        }
    };

    struct velocity {
        int x{};
        int y{};

        [[nodiscard]] friend bool operator==(const velocity &lhs, const velocity &rhs) noexcept {
            return lhs.x == rhs.x && lhs.y == rhs.y;
        }
    };

    struct tag final {};

    using operation_type = std::function<void(entt::registry &)>;

    struct pending_operation {
        entt::entity group{entt::null};
        operation_type operation;
    };

    struct random_number_generator {
        std::uint64_t state{0x9E3779B97F4A7C15ull};

        [[nodiscard]] std::uint64_t next() noexcept {
            state ^= state << 7u;
            state ^= state >> 9u;
            return state;
        }

        [[nodiscard]] std::size_t operator()(const std::size_t bound) noexcept {
            return static_cast<std::size_t>(next() % bound);
        }
    };

    template<typename Type>
    static bool same_component(entt::registry &lhs, entt::registry &rhs) {
        auto &lpool = lhs.storage<Type>();
        auto &rpool = rhs.storage<Type>();

        if(lpool.size() != rpool.size()) {
            return false;
        }

        auto lit = lpool.each().begin();
        auto rit = rpool.each().begin();
        const auto lend = lpool.each().end();
        const auto rend = rpool.each().end();

        for(; lit != lend && rit != rend; ++lit, ++rit) {
            const auto entity = std::get<0>(*lit);

            if(entity != std::get<0>(*rit)) {
                return false;
            }

            if constexpr(!std::is_empty_v<Type>) {
                if(!(std::get<1>(*lit) == std::get<1>(*rit))) {
                    return false;
                }
            }
        }

        return lit == lend && rit == rend;
    }

    static bool same_registry(entt::registry &lhs, entt::registry &rhs) {
        auto &lentities = lhs.storage<entt::entity>();
        auto &rentities = rhs.storage<entt::entity>();

        if(lentities.size() != rentities.size() || lentities.free_list() != rentities.free_list()) {
            return false;
        }

        for(std::size_t pos{}; pos < lentities.size(); ++pos) {
            if(lentities.data()[pos] != rentities.data()[pos]) {
                return false;
            }
        }

        return same_component<position>(lhs, rhs)
               && same_component<velocity>(lhs, rhs)
               && same_component<tag>(lhs, rhs);
    }

    static std::vector<pending_operation> generate_operations(entt::registry &registry, random_number_generator &rng, const std::size_t length) {
        std::vector<entt::entity> alive{};
        alive.reserve(registry.storage<entt::entity>().free_list());

        for(auto [entity]: registry.storage<entt::entity>().each()) {
            alive.push_back(entity);
        }

        std::vector<pending_operation> operations{};
        operations.reserve(length);

        for(std::size_t pos{}; pos < length; ++pos) {
            const auto target = alive[rng(alive.size())];
            auto source = alive[rng(alive.size())];

            if(source == target) {
                source = alive[(rng(alive.size()) + 1u) % alive.size()];
            }

            switch(rng(9u)) {
            case 0u: {
                const auto value = static_cast<int>(rng(1000u));
                operations.push_back({target, [target, value](auto &reg) { reg.template emplace_or_replace<position>(target, position{value, value / 2}); }});
                break;
            }
            case 1u:
                operations.push_back({target, [target](auto &reg) { reg.template remove<position>(target); }});
                break;
            case 2u: {
                const auto value = static_cast<int>(rng(1000u));
                operations.push_back({target, [target, value](auto &reg) { reg.template emplace_or_replace<velocity>(target, velocity{value, value + 1}); }});
                break;
            }
            case 3u:
                operations.push_back({target, [target](auto &reg) { reg.template remove<velocity>(target); }});
                break;
            case 4u:
                operations.push_back({target, [target, source](auto &reg) {
                                          if(const auto *value = reg.template try_get<position>(source); value != nullptr) {
                                              reg.template emplace_or_replace<position>(target, *value);
                                          } else {
                                              reg.template remove<position>(target);
                                          }
                                      }});
                break;
            case 5u:
                operations.push_back({target, [target, source](auto &reg) {
                                          auto *lhs = reg.template try_get<position>(target);
                                          auto *rhs = reg.template try_get<position>(source);

                                          if(lhs != nullptr && rhs != nullptr) {
                                              using std::swap;
                                              swap(*lhs, *rhs);
                                          }
                                      }});
                break;
            case 6u: {
                const auto delta = static_cast<int>(rng(21u)) - 10;
                operations.push_back({target, [target, delta](auto &reg) {
                                          if(auto *value = reg.template try_get<position>(target); value != nullptr) {
                                              value->x += delta;
                                          }
                                      }});
                break;
            }
            case 7u:
                operations.push_back({target, [target](auto &reg) {
                                          if(reg.template all_of<tag>(target)) {
                                              reg.template remove<tag>(target);
                                          } else {
                                              reg.template emplace<tag>(target);
                                          }
                                      }});
                break;
            default:
                operations.push_back({target, [target](auto &reg) {
                                          if(reg.template all_of<position, velocity>(target)) {
                                              reg.template erase<position>(target);
                                          }
                                      }});
                break;
            }
        }

        return operations;
    }
};

TEST_F(CommandBuffer, EmptyBuffer) {
    entt::registry registry;
    entt::command_buffer buffer{registry};

    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.size(), 0u);
    ASSERT_EQ(buffer.group_count(), 0u);
    ASSERT_EQ(buffer.commit(), 0u);
    ASSERT_EQ(buffer.rollback(), 0u);
    ASSERT_TRUE(buffer.empty());
}

TEST_F(CommandBuffer, GroupsAndSubmissionOrder) {
    entt::registry registry;
    entt::command_buffer buffer{registry};
    const auto lhs = registry.create();
    const auto rhs = registry.create();
    std::vector<int> order{};

    ASSERT_EQ(buffer.submit(lhs, [&order](auto &) { order.push_back(1); }), 0u);
    ASSERT_EQ(buffer.submit(rhs, [&order](auto &) { order.push_back(2); }), 1u);
    ASSERT_EQ(buffer.submit(lhs, [&order](auto &) { order.push_back(3); }), 2u);
    ASSERT_EQ(buffer.submit(rhs, [&order](auto &) { order.push_back(4); }), 3u);

    ASSERT_EQ(buffer.size(), 4u);
    ASSERT_EQ(buffer.group_count(), 2u);
    ASSERT_EQ(buffer.size(lhs), 2u);
    ASSERT_EQ(buffer.size(rhs), 2u);
    ASSERT_TRUE(order.empty());

    ASSERT_EQ(buffer.commit(), 4u);
    ASSERT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.group_count(), 0u);
}

TEST_F(CommandBuffer, CommitMatchesInterleavedImmediateExecution) {
    entt::registry registry;
    entt::command_buffer buffer{registry};
    const auto lhs = registry.create();
    const auto rhs = registry.create();

    registry.emplace<position>(lhs, position{1, 1});
    registry.emplace<position>(rhs, position{2, 2});

    buffer.submit(rhs, [rhs](auto &reg) { reg.template replace<position>(rhs, position{10, 10}); });
    buffer.submit(lhs, [lhs, rhs](auto &reg) { reg.template replace<position>(lhs, reg.template get<position>(rhs)); });
    buffer.submit(rhs, [rhs](auto &reg) { reg.template replace<position>(rhs, position{20, 20}); });

    ASSERT_EQ(registry.get<position>(lhs), (position{1, 1}));
    ASSERT_EQ(buffer.commit(), 3u);
    ASSERT_EQ(registry.get<position>(lhs), (position{10, 10}));
    ASSERT_EQ(registry.get<position>(rhs), (position{20, 20}));
}

TEST_F(CommandBuffer, RollbackDiscardsPendingCommands) {
    entt::registry registry;
    entt::command_buffer buffer{registry};
    const auto entity = registry.create();

    ASSERT_EQ(buffer.submit(entity, [entity](auto &reg) { reg.template emplace<position>(entity, position{1, 2}); }), 0u);
    ASSERT_EQ(buffer.submit(entity, [entity](auto &reg) { reg.template patch<position>(entity, [](auto &value) { value.x = 3; }); }), 1u);
    ASSERT_FALSE(registry.all_of<position>(entity));

    ASSERT_EQ(buffer.rollback(), 2u);
    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.group_count(), 0u);
    ASSERT_FALSE(registry.all_of<position>(entity));
    ASSERT_EQ(buffer.commit(), 0u);
    ASSERT_EQ(buffer.sequence(), 2u);

    ASSERT_EQ(buffer.submit(entity, [entity](auto &reg) { reg.template emplace<position>(entity, position{4, 5}); }), 2u);
    ASSERT_EQ(buffer.commit(), 1u);
    ASSERT_EQ(registry.get<position>(entity), (position{4, 5}));
}

TEST_F(CommandBuffer, GlobalCommands) {
    entt::registry registry;
    entt::command_buffer buffer{registry};
    std::vector<int> order{};

    buffer.submit([&order](auto &) { order.push_back(1); });
    buffer.submit(entt::null, [&order](auto &) { order.push_back(2); });

    ASSERT_EQ(buffer.group_count(), 1u);
    ASSERT_EQ(buffer.size(buffer.global_entity()), 2u);
    ASSERT_EQ(buffer.commit(), 2u);
    ASSERT_EQ(order, (std::vector<int>{1, 2}));
}

TEST_F(CommandBuffer, ReentrantSubmissionRunsOnNextCommit) {
    entt::registry registry;
    entt::command_buffer buffer{registry};
    const auto entity = registry.create();

    buffer.submit(entity, [&buffer, entity](auto &reg) {
        reg.template emplace<position>(entity, position{1, 0});
        buffer.submit(entity, [entity](auto &inner) { inner.template patch<position>(entity, [](auto &value) { value.x = 2; }); });
    });

    ASSERT_EQ(buffer.commit(), 1u);
    ASSERT_EQ(registry.get<position>(entity), (position{1, 0}));
    ASSERT_EQ(buffer.size(), 1u);
    ASSERT_EQ(buffer.size(entity), 1u);

    ASSERT_EQ(buffer.commit(), 1u);
    ASSERT_EQ(registry.get<position>(entity), (position{2, 0}));
    ASSERT_TRUE(buffer.empty());
}

TEST_F(CommandBuffer, ThrowingCommandPropagatesAndDropsPendingBatch) {
    entt::registry registry;
    entt::command_buffer buffer{registry};

    buffer.submit([](auto &) { throw error{}; });
    buffer.submit([](auto &) {});

    ASSERT_THROW(buffer.commit(), error);
    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.group_count(), 0u);
}

TEST_F(CommandBuffer, EntityLifecycleMatchesImmediateExecution) {
    entt::registry immediate;
    entt::registry deferred;
    entt::command_buffer buffer{deferred};

    const auto lhs = immediate.create();
    const auto deferred_lhs = deferred.create();
    const auto rhs = immediate.create();
    const auto deferred_rhs = deferred.create();

    ASSERT_EQ(lhs, deferred_lhs);
    ASSERT_EQ(rhs, deferred_rhs);

    immediate.emplace<position>(lhs, position{1, 2});
    deferred.emplace<position>(deferred_lhs, position{1, 2});

    buffer.submit(deferred_lhs, [deferred_lhs](auto &reg) { reg.destroy(deferred_lhs); });
    immediate.destroy(lhs);

    buffer.submit([value = position{3, 4}](auto &reg) {
        const auto entity = reg.create();
        reg.template emplace<position>(entity, value);
    });

    const auto created = immediate.create();
    immediate.emplace<position>(created, position{3, 4});

    ASSERT_EQ(buffer.commit(), 2u);
    ASSERT_TRUE(same_registry(immediate, deferred));
}

TEST_F(CommandBuffer, DifferentialConsistencyForTenThousandFrames) {
    constexpr std::size_t frames = 10000u;
    constexpr std::size_t commands_per_frame = 16u;
    constexpr std::size_t entities = 32u;

    entt::registry immediate;
    entt::registry deferred;
    entt::command_buffer buffer{deferred};
    random_number_generator rng{};

    for(std::size_t pos{}; pos < entities; ++pos) {
        const auto lhs = immediate.create();
        const auto rhs = deferred.create();
        ASSERT_EQ(lhs, rhs);

        const auto value = static_cast<int>(pos);
        immediate.emplace<position>(lhs, position{value, value + 1});
        immediate.emplace<velocity>(lhs, velocity{value + 2, value + 3});
        deferred.emplace<position>(rhs, position{value, value + 1});
        deferred.emplace<velocity>(rhs, velocity{value + 2, value + 3});
    }

    std::size_t mismatches{};
    std::size_t commands{};

    for(std::size_t frame{}; frame < frames; ++frame) {
        auto operations = generate_operations(immediate, rng, commands_per_frame);

        for(auto &&curr: operations) {
            curr.operation(immediate);
        }

        for(auto &&curr: operations) {
            buffer.submit(curr.group, curr.operation);
        }

        const auto committed = buffer.commit();

        if(committed != operations.size() || !same_registry(immediate, deferred)) {
            ++mismatches;
            break;
        }

        commands += committed;
    }

    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(commands, frames * commands_per_frame);
    RecordProperty("frames", frames);
    RecordProperty("commands", commands);
    RecordProperty("mismatches", mismatches);
}

TEST_F(CommandBuffer, TenThousandCommitsDoNotLeak) {
    constexpr std::size_t frames = 10000u;
    constexpr std::size_t warmup = 100u;
    constexpr std::size_t commands_per_frame = 8u;
    constexpr std::size_t entities = 16u;

    auto state = std::make_shared<allocation_state>();
    entt::registry registry;
    std::vector<entt::entity> pool{};

    for(std::size_t pos{}; pos < entities; ++pos) {
        const auto entity = registry.create();
        pool.push_back(entity);
        registry.emplace<position>(entity, position{static_cast<int>(pos), 0});
    }

    std::size_t baseline_bytes{};
    std::size_t baseline_capacity{};

    {
        entt::basic_command_buffer<entt::registry, counting_allocator<entt::entity>> buffer{registry, counting_allocator<entt::entity>{state}};

        for(std::size_t frame{}; frame < frames; ++frame) {
            for(std::size_t pos{}; pos < commands_per_frame; ++pos) {
                const auto entity = pool[(frame + pos) % pool.size()];
                const auto value = static_cast<int>((frame + pos) % 1024u);
                buffer.submit(entity, [entity, value](auto &reg) { reg.template patch<position>(entity, [value](auto &elem) { elem.x = value; }); });
            }

            ASSERT_EQ(buffer.commit(), commands_per_frame);
            ASSERT_TRUE(buffer.empty());
            ASSERT_EQ(buffer.group_count(), 0u);

            if(frame == warmup) {
                baseline_bytes = state->live_bytes;
                baseline_capacity = buffer.capacity();
            }
        }

        EXPECT_EQ(state->live_bytes, baseline_bytes);
        EXPECT_EQ(buffer.capacity(), baseline_capacity);
        EXPECT_EQ(buffer.size(), 0u);
        EXPECT_EQ(buffer.group_count(), 0u);
    }

    EXPECT_EQ(state->live_bytes, 0u);
    EXPECT_EQ(state->allocations, state->deallocations);
    RecordProperty("frames", frames);
    RecordProperty("commands", frames * commands_per_frame);
    RecordProperty("live_bytes_delta", 0u);
}
