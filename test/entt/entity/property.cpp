#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
#include <entt/entity/view.hpp>

namespace {

using entity_ref = std::uint32_t;

constexpr auto component_count = 3u;
constexpr auto invalid_ref = std::numeric_limits<entity_ref>::max();
constexpr auto null_entity = entt::entity{entt::null};

struct position {
    int value{};
};

struct velocity {
    int value{};
};

struct stable_component {
    static constexpr auto in_place_delete = true;
    int value{};
};

enum class component : std::uint8_t {
    position,
    velocity,
    stable
};

enum class operation_kind : std::uint8_t {
    create,
    destroy,
    add,
    remove,
    query_has,
    query_value,
    query_view
};

struct operation {
    operation_kind kind{operation_kind::create};
    entity_ref entity{invalid_ref};
    component first{component::position};
    component second{component::velocity};
    int value{};
};

struct entity_state {
    bool alive{};
    std::uint8_t mask{};
    std::array<int, component_count> values{};

    [[nodiscard]] static constexpr auto index(const component elem) noexcept {
        return static_cast<std::size_t>(elem);
    }

    [[nodiscard]] bool has(const component elem) const noexcept {
        return (mask & (std::uint8_t{1u} << static_cast<unsigned>(elem))) != 0u;
    }

    [[nodiscard]] std::optional<int> get(const component elem) const noexcept {
        return has(elem) ? std::optional<int>{values[index(elem)]} : std::nullopt;
    }

    void add(const component elem, const int value) noexcept {
        mask = static_cast<std::uint8_t>(mask | (std::uint8_t{1u} << static_cast<unsigned>(elem)));
        values[index(elem)] = value;
    }

    void remove(const component elem) noexcept {
        mask = static_cast<std::uint8_t>(mask & ~(std::uint8_t{1u} << static_cast<unsigned>(elem)));
    }
};

struct world {
    entt::registry registry{};
    std::vector<entity_state> model{};
    std::vector<entt::entity> entity_of_ref{};
    std::vector<entity_ref> ref_of_entity{};

    [[nodiscard]] const entity_state *state(const entity_ref ref) const noexcept {
        return (static_cast<std::size_t>(ref) < model.size()) ? &model[ref] : nullptr;
    }

    [[nodiscard]] entity_state &state(const entity_ref ref) {
        if(static_cast<std::size_t>(ref) >= model.size()) {
            model.resize(static_cast<std::size_t>(ref) + 1u);
        }

        return model[ref];
    }

    [[nodiscard]] bool alive(const entity_ref ref) const noexcept {
        const auto *elem = state(ref);
        return elem && elem->alive;
    }

    [[nodiscard]] entt::entity entity(const entity_ref ref) const noexcept {
        return (static_cast<std::size_t>(ref) < entity_of_ref.size()) ? entity_of_ref[ref] : null_entity;
    }

    [[nodiscard]] entity_ref ref(const entt::entity entt) const noexcept {
        const auto idx = static_cast<std::size_t>(entt::to_entity(entt));
        return (idx < ref_of_entity.size()) ? ref_of_entity[idx] : invalid_ref;
    }

    void bind(const entity_ref ref, const entt::entity entt) {
        if(static_cast<std::size_t>(ref) >= entity_of_ref.size()) {
            entity_of_ref.resize(static_cast<std::size_t>(ref) + 1u, null_entity);
        }

        entity_of_ref[ref] = entt;

        const auto idx = static_cast<std::size_t>(entt::to_entity(entt));

        if(idx >= ref_of_entity.size()) {
            ref_of_entity.resize(idx + 1u, invalid_ref);
        }

        ref_of_entity[idx] = ref;
    }

    void unbind(const entity_ref ref) {
        if(static_cast<std::size_t>(ref) < entity_of_ref.size()) {
            const auto entt = entity_of_ref[ref];

            if(entt != null_entity) {
                const auto idx = static_cast<std::size_t>(entt::to_entity(entt));

                if(idx < ref_of_entity.size() && ref_of_entity[idx] == ref) {
                    ref_of_entity[idx] = invalid_ref;
                }
            }

            entity_of_ref[ref] = null_entity;
        }
    }
};

struct failure {
    std::size_t operation{};
    std::string detail{};
};

using failure_type = std::optional<failure>;

[[nodiscard]] const char *component_name(const component elem) noexcept {
    switch(elem) {
    case component::position:
        return "position";
    case component::velocity:
        return "velocity";
    case component::stable:
        return "stable";
    }

    return "unknown";
}

[[nodiscard]] std::string describe(const std::optional<int> value) {
    return value ? std::to_string(*value) : "<missing>";
}

[[nodiscard]] std::string describe(const operation &op) {
    std::ostringstream stream;

    switch(op.kind) {
    case operation_kind::create:
        stream << "create e" << op.entity;
        break;
    case operation_kind::destroy:
        stream << "destroy e" << op.entity;
        break;
    case operation_kind::add:
        stream << "add e" << op.entity << " " << component_name(op.first) << "=" << op.value;
        break;
    case operation_kind::remove:
        stream << "remove e" << op.entity << " " << component_name(op.first);
        break;
    case operation_kind::query_has:
        stream << "query e" << op.entity << " has " << component_name(op.first);
        break;
    case operation_kind::query_value:
        stream << "query e" << op.entity << " " << component_name(op.first) << " value";
        break;
    case operation_kind::query_view:
        stream << "query view<" << component_name(op.first) << ", " << component_name(op.second) << ">";
        break;
    }

    return stream.str();
}

[[nodiscard]] std::string describe(const std::vector<operation> &ops) {
    std::ostringstream stream;

    for(std::size_t pos{}; pos < ops.size(); ++pos) {
        stream << "  " << pos << ": " << describe(ops[pos]) << '\n';
    }

    return stream.str();
}

[[nodiscard]] failure_type sync_entity(const world &world, const entity_ref ref, const std::size_t operation) {
    const auto expected = world.alive(ref);
    const auto entt = world.entity(ref);
    const auto actual = entt != null_entity && world.registry.valid(entt);

    if(expected != actual) {
        std::ostringstream stream;
        stream << "e" << ref << " validity mismatch: model=" << expected << ", registry=" << actual;
        return failure{operation, stream.str()};
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<int> read_component(const entt::registry &registry, const entt::entity entt, const component elem) {
    if(entt == null_entity) {
        return std::nullopt;
    }

    switch(elem) {
    case component::position:
        if(const auto *value = registry.try_get<position>(entt); value) {
            return value->value;
        }
        break;
    case component::velocity:
        if(const auto *value = registry.try_get<velocity>(entt); value) {
            return value->value;
        }
        break;
    case component::stable:
        if(const auto *value = registry.try_get<stable_component>(entt); value) {
            return value->value;
        }
        break;
    }

    return std::nullopt;
}

void add_component(entt::registry &registry, const entt::entity entt, const component elem, const int value) {
    switch(elem) {
    case component::position:
        registry.emplace_or_replace<position>(entt, value);
        break;
    case component::velocity:
        registry.emplace_or_replace<velocity>(entt, value);
        break;
    case component::stable:
        registry.emplace_or_replace<stable_component>(entt, value);
        break;
    }
}

[[nodiscard]] std::size_t remove_component(entt::registry &registry, const entt::entity entt, const component elem) {
    switch(elem) {
    case component::position:
        return registry.remove<position>(entt);
    case component::velocity:
        return registry.remove<velocity>(entt);
    case component::stable:
        return registry.remove<stable_component>(entt);
    }

    return 0u;
}

struct view_entry {
    entity_ref entity{invalid_ref};
    int first{};
    int second{};

    [[nodiscard]] bool operator==(const view_entry &) const noexcept = default;

    [[nodiscard]] bool operator<(const view_entry &other) const noexcept {
        return std::tie(entity, first, second) < std::tie(other.entity, other.first, other.second);
    }
};

template<typename First, typename Second>
void collect_view(const world &world, std::vector<view_entry> &entries) {
    world.registry.view<First, Second>().each([&](const entt::entity entt, const First &first, const Second &second) {
        entries.push_back(view_entry{world.ref(entt), first.value, second.value});
    });
}

[[nodiscard]] std::vector<view_entry> actual_view(const world &world, const component first, const component second) {
    std::vector<view_entry> entries;

    switch(first) {
    case component::position:
        switch(second) {
        case component::velocity:
            collect_view<position, velocity>(world, entries);
            break;
        case component::stable:
            collect_view<position, stable_component>(world, entries);
            break;
        case component::position:
            break;
        }
        break;
    case component::velocity:
        switch(second) {
        case component::position:
            collect_view<velocity, position>(world, entries);
            break;
        case component::stable:
            collect_view<velocity, stable_component>(world, entries);
            break;
        case component::velocity:
            break;
        }
        break;
    case component::stable:
        switch(second) {
        case component::position:
            collect_view<stable_component, position>(world, entries);
            break;
        case component::velocity:
            collect_view<stable_component, velocity>(world, entries);
            break;
        case component::stable:
            break;
        }
        break;
    }

    std::sort(entries.begin(), entries.end());
    return entries;
}

[[nodiscard]] std::vector<view_entry> expected_view(const world &world, const component first, const component second) {
    std::vector<view_entry> entries;

    for(std::size_t pos{}; pos < world.model.size(); ++pos) {
        const auto &state = world.model[pos];

        if(state.alive && state.has(first) && state.has(second)) {
            entries.push_back(view_entry{static_cast<entity_ref>(pos), *state.get(first), *state.get(second)});
        }
    }

    return entries;
}

[[nodiscard]] failure_type execute(world &world, const operation &op, const std::size_t pos) {
    if(op.kind == operation_kind::create) {
        const auto entt = world.registry.create();
        world.bind(op.entity, entt);
        world.state(op.entity) = entity_state{true, {}, {}};
        return std::nullopt;
    }

    if(op.kind == operation_kind::query_view) {
        const auto expected = expected_view(world, op.first, op.second);
        const auto actual = actual_view(world, op.first, op.second);

        if(expected != actual) {
            std::ostringstream stream;
            stream << "view<" << component_name(op.first) << ", " << component_name(op.second)
                   << "> mismatch: expected " << expected.size() << " entities, actual " << actual.size();

            if(!expected.empty()) {
                stream << ", first expected e" << expected.front().entity;
            }

            if(!actual.empty()) {
                stream << ", first actual e" << actual.front().entity;
            }

            return failure{pos, stream.str()};
        }

        return std::nullopt;
    }

    if(const auto error = sync_entity(world, op.entity, pos); error) {
        return error;
    }

    const auto entt = world.entity(op.entity);
    const auto alive = world.alive(op.entity);
    auto &state = world.state(op.entity);

    switch(op.kind) {
    case operation_kind::destroy:
        if(alive) {
            world.registry.destroy(entt);
            world.unbind(op.entity);
            state = entity_state{};
        }
        break;
    case operation_kind::add:
        if(alive) {
            add_component(world.registry, entt, op.first, op.value);
            state.add(op.first, op.value);
        }
        break;
    case operation_kind::remove:
        if(alive) {
            const auto removed = remove_component(world.registry, entt, op.first);
            const auto expected = state.has(op.first);

            if((removed != 0u) != expected) {
                std::ostringstream stream;
                stream << "e" << op.entity << " remove " << component_name(op.first)
                       << " mismatch: model had=" << expected << ", registry removed=" << (removed != 0u);
                return failure{pos, stream.str()};
            }

            state.remove(op.first);
        }
        break;
    case operation_kind::query_has: {
        const auto expected = alive && state.has(op.first);
        const auto actual = read_component(world.registry, entt, op.first).has_value();

        if(expected != actual) {
            std::ostringstream stream;
            stream << "e" << op.entity << " has " << component_name(op.first)
                   << " mismatch: model=" << expected << ", registry=" << actual;
            return failure{pos, stream.str()};
        }
        break;
    }
    case operation_kind::query_value: {
        const auto expected = alive ? state.get(op.first) : std::optional<int>{};
        const auto actual = read_component(world.registry, entt, op.first);

        if(expected != actual) {
            std::ostringstream stream;
            stream << "e" << op.entity << " " << component_name(op.first)
                   << " mismatch: model=" << describe(expected) << ", registry=" << describe(actual);
            return failure{pos, stream.str()};
        }
        break;
    }
    case operation_kind::create:
    case operation_kind::query_view:
        break;
    }

    return std::nullopt;
}

template<typename Type>
[[nodiscard]] failure_type verify_component(const world &world, const component elem, const std::size_t pos) {
    failure_type error{};

    world.registry.view<Type>().each([&](const entt::entity entt, const Type &value) {
        if(error) {
            return;
        }

        const auto ref = world.ref(entt);
        const auto *state = world.state(ref);

        if(!world.registry.valid(entt) || !state || !state->alive) {
            std::ostringstream stream;
            stream << "component " << component_name(elem) << " belongs to invalid or dead entity e" << ref;
            error = failure{pos, stream.str()};
        } else if(!state->has(elem)) {
            std::ostringstream stream;
            stream << "e" << ref << " unexpectedly has " << component_name(elem);
            error = failure{pos, stream.str()};
        } else if(const auto expected = state->get(elem); expected != value.value) {
            std::ostringstream stream;
            stream << "e" << ref << " " << component_name(elem)
                   << " mismatch: model=" << describe(expected) << ", registry=" << value.value;
            error = failure{pos, stream.str()};
        }
    });

    return error;
}

[[nodiscard]] failure_type verify_world(const world &world, const std::size_t pos) {
    std::size_t expected_live{};

    for(const auto &state: world.model) {
        expected_live += static_cast<std::size_t>(state.alive);
    }

    auto entities = world.registry.view<entt::entity>();
    const auto actual_live = entities.size();

    if(expected_live != actual_live) {
        std::ostringstream stream;
        stream << "live entity count mismatch: model=" << expected_live << ", registry=" << actual_live;
        return failure{pos, stream.str()};
    }

    for(const auto entt: entities) {
        const auto ref = world.ref(entt);
        const auto *state = world.state(ref);

        if(!world.registry.valid(entt) || !state || !state->alive) {
            std::ostringstream stream;
            stream << "registry contains unexpected live entity e" << ref;
            return failure{pos, stream.str()};
        }

        if(world.entity(ref) != entt) {
            std::ostringstream stream;
            stream << "e" << ref << " reverse mapping mismatch";
            return failure{pos, stream.str()};
        }

        for(auto elem: {component::position, component::velocity, component::stable}) {
            const auto expected = state->get(elem);
            const auto actual = read_component(world.registry, entt, elem);

            if(expected != actual) {
                std::ostringstream stream;
                stream << "final e" << ref << " " << component_name(elem)
                       << " mismatch: model=" << describe(expected) << ", registry=" << describe(actual);
                return failure{pos, stream.str()};
            }
        }
    }

    if(const auto error = verify_component<position>(world, component::position, pos); error) {
        return error;
    }

    if(const auto error = verify_component<velocity>(world, component::velocity, pos); error) {
        return error;
    }

    return verify_component<stable_component>(world, component::stable, pos);
}

[[nodiscard]] failure_type run(world &world, const std::vector<operation> &ops) {
    for(std::size_t pos{}; pos < ops.size(); ++pos) {
        if(const auto error = execute(world, ops[pos], pos); error) {
            return error;
        }
    }

    return verify_world(world, ops.size());
}

[[nodiscard]] failure_type run(const std::vector<operation> &ops) {
    world world;
    return run(world, ops);
}

template<typename Predicate>
[[nodiscard]] std::vector<operation> minimize(std::vector<operation> ops, Predicate &&fails) {
    auto reduced = true;

    while(reduced) {
        reduced = false;

        for(std::size_t pos{}; pos < ops.size(); ++pos) {
            auto candidate = ops;
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(pos));

            if(fails(candidate)) {
                ops = std::move(candidate);
                reduced = true;
                break;
            }
        }
    }

    return ops;
}

[[nodiscard]] operation random_query(std::mt19937_64 &rng, const entity_ref max_entity) {
    operation op{};
    op.kind = operation_kind::query_has;
    op.entity = static_cast<entity_ref>(rng() % (static_cast<std::uint64_t>(max_entity) + 1u));
    op.first = static_cast<component>(rng() % component_count);

    switch(rng() % 3u) {
    case 0u:
        op.kind = operation_kind::query_has;
        break;
    case 1u:
        op.kind = operation_kind::query_value;
        break;
    default: {
        op.kind = operation_kind::query_view;
        op.second = static_cast<component>((static_cast<unsigned>(op.first) + 1u + static_cast<unsigned>(rng() % (component_count - 1u))) % component_count);
        break;
    }
    }

    return op;
}

[[nodiscard]] std::vector<operation> generate_sequence(const std::uint64_t seed) {
    std::mt19937_64 rng{seed};
    std::vector<operation> ops;
    entity_ref next_entity{};
    const auto length = static_cast<std::size_t>(32u + rng() % 33u);
    ops.reserve(length);

    ops.push_back(random_query(rng, 0u));

    while(ops.size() < length) {
        const auto choice = rng() % 100u;

        if(choice < 20u) {
            ops.push_back(operation{operation_kind::create, next_entity++});
        } else if(next_entity == 0u || choice < 35u) {
            ops.push_back(random_query(rng, next_entity));
        } else {
            const auto target = static_cast<entity_ref>(rng() % next_entity);

            if(choice < 50u) {
                ops.push_back(operation{operation_kind::add, target, static_cast<component>(rng() % component_count), {}, static_cast<int>(rng() % 255u) - 127});
            } else if(choice < 65u) {
                ops.push_back(operation{operation_kind::remove, target, static_cast<component>(rng() % component_count)});
            } else if(choice < 75u) {
                ops.push_back(operation{operation_kind::destroy, target});
            } else {
                ops.push_back(random_query(rng, next_entity));
            }
        }
    }

    return ops;
}

void populate_huge(world &world, const std::uint32_t count) {
    std::vector<entt::entity> entities(count);

    world.registry.storage<entt::entity>().reserve(count);
    world.registry.storage<position>().reserve(count / 2u);
    world.registry.storage<velocity>().reserve(count / 3u);
    world.registry.storage<stable_component>().reserve(count / 5u);
    world.registry.create(entities.begin(), entities.end());

    world.model.resize(count);
    world.entity_of_ref.resize(count);
    world.ref_of_entity.resize(static_cast<std::size_t>(entt::to_entity(null_entity)) + 1u, invalid_ref);

    for(std::uint32_t pos{}; pos < count; ++pos) {
        const auto entt = entities[pos];
        auto &state = world.model[pos];

        state.alive = true;
        world.entity_of_ref[pos] = entt;
        world.ref_of_entity[static_cast<std::size_t>(entt::to_entity(entt))] = pos;

        if((pos % 2u) == 0u) {
            const auto value = static_cast<int>(pos / 2u);
            world.registry.emplace<position>(entt, value);
            state.add(component::position, value);
        }

        if((pos % 3u) == 0u) {
            const auto value = -static_cast<int>(pos / 3u);
            world.registry.emplace<velocity>(entt, value);
            state.add(component::velocity, value);
        }

        if((pos % 5u) == 0u) {
            const auto value = static_cast<int>(pos / 5u);
            world.registry.emplace<stable_component>(entt, value);
            state.add(component::stable, value);
        }
    }
}

[[nodiscard]] std::vector<operation> generate_large_tail(const std::uint64_t seed, const std::uint32_t count) {
    std::mt19937_64 rng{seed};
    std::vector<operation> ops;
    ops.reserve(32u);

    for(std::size_t pos{}; pos < 32u; ++pos) {
        const auto target = static_cast<entity_ref>(rng() % count);
        const auto elem = static_cast<component>(rng() % component_count);

        switch(rng() % 5u) {
        case 0u:
            ops.push_back(operation{operation_kind::destroy, target});
            break;
        case 1u:
            ops.push_back(operation{operation_kind::add, target, elem, {}, static_cast<int>(rng() % 255u) - 127});
            break;
        case 2u:
            ops.push_back(operation{operation_kind::remove, target, elem});
            break;
        case 3u:
            ops.push_back(operation{operation_kind::query_has, target, elem});
            break;
        default:
            ops.push_back(operation{operation_kind::query_value, target, elem});
            break;
        }
    }

    return ops;
}

} // namespace

TEST(EntityProperty, RandomOperationSequences) {
    constexpr std::uint32_t sequence_count = 10000u;
    constexpr std::uint64_t seed_base = 0x9E3779B97F4A7C15ull;
    std::uint64_t operation_count{};

    for(std::uint32_t seq{}; seq < sequence_count; ++seq) {
        const auto seed = seed_base + (static_cast<std::uint64_t>(seq) * 0xD1B54A32D192ED03ull);
        const auto ops = generate_sequence(seed);
        operation_count += ops.size();

        if(const auto error = run(ops); error) {
            const auto minimized = minimize(ops, [](const std::vector<operation> &candidate) {
                return run(candidate).has_value();
            });
            const auto minimized_error = run(minimized);

            if(!minimized_error) {
                FAIL() << "minimizer lost the failure after reducing " << ops.size() << " operations";
            }

            std::ostringstream stream;
            stream << "random sequence failed\n"
                   << "seed: " << seed << "\n"
                   << "original operations: " << ops.size() << "\n"
                   << "minimized operations: " << minimized.size() << "\n"
                   << "failure at operation " << minimized_error->operation << ": " << minimized_error->detail << "\n"
                   << "minimized sequence:\n"
                   << describe(minimized);
            FAIL() << stream.str();
        }
    }

    RecordProperty("sequences", std::to_string(sequence_count));
    RecordProperty("operations", std::to_string(operation_count));
}

TEST(EntityProperty, EmptyWorldQueries) {
    constexpr std::uint32_t sequence_count = 10000u;

    for(std::uint32_t seq{}; seq < sequence_count; ++seq) {
        std::mt19937_64 rng{0x94D049BB133111EBull + seq};
        std::vector<operation> ops;
        ops.reserve(8u);

        for(std::size_t pos{}; pos < 8u; ++pos) {
            ops.push_back(random_query(rng, 0u));
        }

        if(const auto error = run(ops); error) {
            FAIL() << "empty world sequence " << seq << " failed at operation " << error->operation << ": " << error->detail << '\n'
                   << describe(ops);
        }
    }

    RecordProperty("sequences", std::to_string(sequence_count));
}

TEST(EntityProperty, HugeEntityCount) {
    constexpr std::uint32_t entity_count = 1000000u;
    constexpr std::uint64_t seed = 0xD6E8FEB86659FD93ull;
    const auto ops = generate_large_tail(seed, entity_count);
    world large;
    populate_huge(large, entity_count);

    if(const auto error = run(large, ops); error) {
        const auto minimized = minimize(ops, [entity_count](const std::vector<operation> &candidate) {
            world replay;
            populate_huge(replay, entity_count);
            return run(replay, candidate).has_value();
        });
        world replay;
        populate_huge(replay, entity_count);
        const auto minimized_error = run(replay, minimized);

        if(!minimized_error) {
            FAIL() << "minimizer lost the failure after reducing " << ops.size() << " operations";
        }

        std::ostringstream stream;
        stream << "huge world sequence failed\n"
               << "initial entities: " << entity_count << "\n"
               << "seed: " << seed << "\n"
               << "original operations: " << ops.size() << "\n"
               << "minimized operations: " << minimized.size() << "\n"
               << "failure at operation " << minimized_error->operation << ": " << minimized_error->detail << "\n"
               << "minimized sequence:\n"
               << describe(minimized);
        FAIL() << stream.str();
    }

    RecordProperty("initial_entities", std::to_string(entity_count));
    RecordProperty("operations", std::to_string(ops.size()));
}
