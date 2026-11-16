#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <entt/entity/registry.hpp>

// Deterministic model-based test for registry lifetime and iteration rules.
// Failures are delta-reduced by disabling operations and replaying the seed.

namespace {

constexpr std::size_t groups = 200u;
constexpr std::size_t operations_per_group = 256u;
constexpr std::size_t operation_block = 16u;

struct alpha {
    int value{};
};

struct beta {
    beta() = default;

    explicit beta(const int val)
        : text{std::to_string(val)},
          value{val} {}

    std::string text{};
    int value{};
};

struct gamma {
    static constexpr bool in_place_delete = true;
    static constexpr std::uint32_t live_tag = 0xA11CEu;
    static constexpr std::uint32_t moved_tag = 0xBEEFu;
    static constexpr std::uint32_t dead_tag = 0xDEADu;

    static inline std::size_t alive{};
    std::uint32_t tag{live_tag};
    int value{};

    gamma() {
        ++alive;
    }

    explicit gamma(const int val)
        : value{val} {
        ++alive;
    }

    gamma(const gamma &other)
        : value{other.value} {
        ++alive;
    }

    gamma(gamma &&other) noexcept
        : value{other.value} {
        other.tag = moved_tag;
        ++alive;
    }

    gamma &operator=(const gamma &other) {
        value = other.value;
        return *this;
    }

    gamma &operator=(gamma &&other) noexcept {
        value = other.value;
        other.tag = moved_tag;
        return *this;
    }

    ~gamma() {
        if(tag != live_tag && tag != moved_tag) {
            std::fprintf(stderr, "gamma: double destruction detected\n");
            std::abort();
        }

        tag = dead_tag;
        --alive;
    }

    static void reset() {
        if(alive != 0u) {
            std::fprintf(stderr, "gamma: %zu instances leaked between groups\n", alive);
            std::abort();
        }
    }
};

struct tag {};

} // namespace

namespace entt {

template<>
struct component_traits<alpha, entity> {
    using element_type = alpha;
    using entity_type = entity;
    static constexpr bool in_place_delete = false;
    static constexpr std::size_t page_size = 4u;
};

template<>
struct component_traits<beta, entity> {
    using element_type = beta;
    using entity_type = entity;
    static constexpr bool in_place_delete = false;
    static constexpr std::size_t page_size = 4u;
};

template<>
struct component_traits<gamma, entity> {
    using element_type = gamma;
    using entity_type = entity;
    static constexpr bool in_place_delete = true;
    static constexpr std::size_t page_size = 4u;
};

} // namespace entt

namespace {

enum class component_kind : std::size_t {
    alpha,
    beta,
    gamma,
    tag,
    count
};

constexpr auto component_count = static_cast<std::size_t>(component_kind::count);

constexpr std::array<std::string_view, component_count> component_names{
    "alpha",
    "beta",
    "gamma",
    "tag",
};

constexpr std::array<bool, component_count> in_place_components{
    false,
    false,
    true,
    false,
};

enum class operation_kind : std::uint8_t {
    create,
    destroy,
    add,
    remove,
    patch,
    iterate,
};

constexpr std::array<operation_kind, operation_block> operation_pool{
    operation_kind::create,
    operation_kind::create,
    operation_kind::create,
    operation_kind::add,
    operation_kind::add,
    operation_kind::add,
    operation_kind::destroy,
    operation_kind::destroy,
    operation_kind::remove,
    operation_kind::remove,
    operation_kind::patch,
    operation_kind::patch,
    operation_kind::iterate,
    operation_kind::iterate,
    operation_kind::iterate,
    operation_kind::iterate,
};

struct splitmix64 {
    std::uint64_t state;

    explicit splitmix64(const std::uint64_t seed)
        : state{seed} {}

    std::uint64_t next() {
        std::uint64_t value = (state += 0x9E3779B97F4A7C15ull);
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31u);
    }

    std::size_t below(const std::size_t bound) {
        return bound == 0u ? 0u : static_cast<std::size_t>(next() % bound);
    }
};

struct entity_state {
    entt::entity entity{};
    std::array<bool, component_count> owned{};
    std::array<int, component_count> values{};
};

struct world_model {
    std::vector<entity_state> live{};
    std::vector<entt::entity> known{};

    [[nodiscard]] entity_state *find(const entt::entity entity) {
        const auto it = std::find_if(live.begin(), live.end(), [entity](const entity_state &elem) {
            return elem.entity == entity;
        });
        return it == live.end() ? nullptr : &*it;
    }

    [[nodiscard]] const entity_state *find(const entt::entity entity) const {
        const auto it = std::find_if(live.begin(), live.end(), [entity](const entity_state &elem) {
            return elem.entity == entity;
        });
        return it == live.end() ? nullptr : &*it;
    }
};

struct run_stats {
    std::size_t operations{};
    std::size_t creates{};
    std::size_t destroys{};
    std::size_t adds{};
    std::size_t removes{};
    std::size_t patches{};
    std::size_t iterations{};
    std::size_t visits{};
    std::size_t mutation_visits{};
};

struct property_failure: std::exception {
    property_failure(std::string failure_key, std::string failure_message)
        : key{std::move(failure_key)},
          message{std::move(failure_message)} {}

    [[nodiscard]] const char *what() const noexcept override {
        return message.c_str();
    }

    std::string key;
    std::string message;
};

[[noreturn]] void fail(std::string key, std::string message) {
    throw property_failure{std::move(key), std::move(message)};
}

struct trace_log {
    std::vector<std::string> lines{};

    void push(std::string line) {
        lines.push_back(std::move(line));
    }
};

[[nodiscard]] std::string entity_name(const entt::entity entity) {
    return "e" + std::to_string(entt::to_integral(entity));
}

[[nodiscard]] const char *component_name(const component_kind kind) {
    return component_names[static_cast<std::size_t>(kind)].data();
}

void log(trace_log *trace, std::string message) {
    if(trace != nullptr) {
        trace->push(std::move(message));
    }
}

std::size_t component_index(const component_kind kind) {
    return static_cast<std::size_t>(kind);
}

entity_state &model_state(world_model &model, const entt::entity entity) {
    auto *elem = model.find(entity);

    if(elem == nullptr) {
        fail("model-entity", "operation referenced an entity missing from the model");
    }

    return *elem;
}

std::size_t live_with(const world_model &model, const component_kind kind) {
    const auto idx = component_index(kind);
    return static_cast<std::size_t>(std::count_if(model.live.begin(), model.live.end(), [idx](const entity_state &elem) {
        return elem.owned[idx];
    }));
}

entt::entity create_entity(entt::registry &registry, world_model &model, run_stats &stats, trace_log *trace) {
    const auto entity = registry.create();
    model.known.push_back(entity);
    model.live.push_back(entity_state{entity});
    ++stats.creates;
    log(trace, "create " + entity_name(entity));
    return entity;
}

void emplace_component(entt::registry &registry, world_model &model, const entt::entity entity, const component_kind kind, const int value, run_stats &stats, trace_log *trace) {
    auto &state = model_state(model, entity);
    const auto idx = component_index(kind);

    if(state.owned[idx]) {
        fail("model-duplicate", "attempted to add a component already present in the model");
    }

    switch(kind) {
    case component_kind::alpha:
        registry.emplace<alpha>(entity, value);
        break;
    case component_kind::beta:
        registry.emplace<beta>(entity, value);
        break;
    case component_kind::gamma:
        registry.emplace<gamma>(entity, value);
        break;
    case component_kind::tag:
        registry.emplace<tag>(entity);
        break;
    case component_kind::count:
        std::abort();
    }

    state.owned[idx] = true;
    state.values[idx] = value;
    ++stats.adds;
    log(trace, "emplace<" + std::string{component_name(kind)} + ">(" + entity_name(entity) + (kind == component_kind::tag ? std::string{")"} : ", " + std::to_string(value) + ")"));
}

void remove_component(entt::registry &registry, world_model &model, const entt::entity entity, const component_kind kind, run_stats &stats, trace_log *trace) {
    auto &state = model_state(model, entity);
    const auto idx = component_index(kind);

    if(!state.owned[idx]) {
        fail("model-missing", "attempted to remove a component missing from the model");
    }

    switch(kind) {
    case component_kind::alpha:
        registry.remove<alpha>(entity);
        break;
    case component_kind::beta:
        registry.remove<beta>(entity);
        break;
    case component_kind::gamma:
        registry.remove<gamma>(entity);
        break;
    case component_kind::tag:
        registry.remove<tag>(entity);
        break;
    case component_kind::count:
        std::abort();
    }

    state.owned[idx] = false;
    state.values[idx] = 0;
    ++stats.removes;
    log(trace, "remove<" + std::string{component_name(kind)} + ">(" + entity_name(entity) + ")");
}

void patch_component(entt::registry &registry, world_model &model, const entt::entity entity, const component_kind kind, const int value, run_stats &stats, trace_log *trace) {
    auto &state = model_state(model, entity);
    const auto idx = component_index(kind);

    if(!state.owned[idx]) {
        fail("model-missing", "attempted to patch a component missing from the model");
    }

    switch(kind) {
    case component_kind::alpha:
        registry.patch<alpha>(entity, [value](alpha &elem) { elem.value = value; });
        break;
    case component_kind::beta:
        registry.patch<beta>(entity, [value](beta &elem) {
            elem.value = value;
            elem.text = std::to_string(value);
        });
        break;
    case component_kind::gamma:
        registry.patch<gamma>(entity, [value](gamma &elem) { elem.value = value; });
        break;
    case component_kind::tag:
        registry.patch<tag>(entity, []() {});
        break;
    case component_kind::count:
        std::abort();
    }

    state.values[idx] = value;
    ++stats.patches;
    log(trace, "patch<" + std::string{component_name(kind)} + ">(" + entity_name(entity) + ", " + std::to_string(value) + ")");
}

void destroy_entity(entt::registry &registry, world_model &model, const entt::entity entity, run_stats &stats, trace_log *trace) {
    const auto it = std::find_if(model.live.begin(), model.live.end(), [entity](const entity_state &elem) {
        return elem.entity == entity;
    });

    if(it == model.live.end()) {
        fail("model-missing", "attempted to destroy an entity missing from the model");
    }

    registry.destroy(entity);
    model.live.erase(it);
    ++stats.destroys;
    log(trace, "destroy(" + entity_name(entity) + ")");
}

template<typename Type>
[[nodiscard]] std::vector<entt::entity> view_entities(const entt::registry &registry) {
    std::vector<entt::entity> result{};
    const auto view = registry.view<Type>();

    for(const auto entity: view) {
        result.push_back(entity);
    }

    return result;
}

template<typename Type>
void check_view(const entt::registry &registry, const world_model &model, const component_kind kind) {
    const auto idx = component_index(kind);
    auto expected = std::vector<entt::entity>{};

    for(const auto &elem: model.live) {
        if(elem.owned[idx]) {
            expected.push_back(elem.entity);
        }
    }

    auto actual = view_entities<Type>(registry);
    const auto entity_less = [](const entt::entity lhs, const entt::entity rhs) {
        return entt::to_integral(lhs) < entt::to_integral(rhs);
    };

    std::sort(expected.begin(), expected.end(), entity_less);
    std::sort(actual.begin(), actual.end(), entity_less);

    if(expected != actual) {
        std::ostringstream message;
        message << "view<" << component_name(kind) << "> yielded " << actual.size()
                << " entities, expected " << expected.size();
        fail("view-set", message.str());
    }
}

void check_world(const entt::registry &registry, const world_model &model) {
    std::size_t gamma_count{};

    for(const auto entity: model.known) {
        const auto *state = model.find(entity);
        const auto valid = (state != nullptr);

        if(registry.valid(entity) != valid) {
            fail("entity-validity", "registry validity differs from the model for " + entity_name(entity));
        }

        for(std::size_t idx{}; idx < component_count; ++idx) {
            const auto kind = static_cast<component_kind>(idx);
            const auto expected = valid && state->owned[idx];
            auto actual = false;

            switch(kind) {
            case component_kind::alpha:
                actual = registry.all_of<alpha>(entity);
                break;
            case component_kind::beta:
                actual = registry.all_of<beta>(entity);
                break;
            case component_kind::gamma:
                actual = registry.all_of<gamma>(entity);
                break;
            case component_kind::tag:
                actual = registry.all_of<tag>(entity);
                break;
            case component_kind::count:
                std::abort();
            }

            if(actual != expected) {
                fail("component-presence", "component presence differs for " + entity_name(entity) + " and " + component_name(kind));
            }
        }

        if(state == nullptr) {
            continue;
        }

        if(const auto *elem = registry.try_get<alpha>(entity); state->owned[component_index(component_kind::alpha)]) {
            if(elem == nullptr || elem->value != state->values[component_index(component_kind::alpha)]) {
                fail("component-value", "alpha value differs for " + entity_name(entity));
            }
        } else if(elem != nullptr) {
            fail("component-value", "unexpected alpha value for " + entity_name(entity));
        }

        if(const auto *elem = registry.try_get<beta>(entity); state->owned[component_index(component_kind::beta)]) {
            const auto value = state->values[component_index(component_kind::beta)];
            if(elem == nullptr || elem->value != value || elem->text != std::to_string(value)) {
                fail("component-value", "beta value differs for " + entity_name(entity));
            }
        } else if(elem != nullptr) {
            fail("component-value", "unexpected beta value for " + entity_name(entity));
        }

        if(const auto *elem = registry.try_get<gamma>(entity); state->owned[component_index(component_kind::gamma)]) {
            ++gamma_count;

            if(elem == nullptr || elem->value != state->values[component_index(component_kind::gamma)]) {
                fail("component-value", "gamma value differs for " + entity_name(entity));
            }
        } else if(elem != nullptr) {
            fail("component-value", "unexpected gamma value for " + entity_name(entity));
        }
    }

    for(std::size_t idx{}; idx < component_count; ++idx) {
        const auto kind = static_cast<component_kind>(idx);
        const auto expected = live_with(model, kind);

        const auto *pool = [&]() -> const entt::basic_sparse_set<> * {
            switch(kind) {
            case component_kind::alpha:
                return registry.storage<alpha>();
            case component_kind::beta:
                return registry.storage<beta>();
            case component_kind::gamma:
                return registry.storage<gamma>();
            case component_kind::tag:
                return registry.storage<tag>();
            case component_kind::count:
                break;
            }

            return nullptr;
        }();

        if(!in_place_components[idx] && pool != nullptr && pool->size() != expected) {
            fail("storage-size", "storage size differs for " + std::string{component_name(kind)});
        }

        if(in_place_components[idx] && pool != nullptr && pool->size() < expected) {
            fail("storage-size", "in-place storage is smaller than the model");
        }
    }

    if(gamma::alive != gamma_count) {
        fail("object-lifetime", "gamma live-object count differs from the model");
    }

    check_view<alpha>(registry, model, component_kind::alpha);
    check_view<beta>(registry, model, component_kind::beta);
    check_view<gamma>(registry, model, component_kind::gamma);
    check_view<tag>(registry, model, component_kind::tag);
}

void mix(std::uint64_t &hash, const std::uint64_t value) {
    hash ^= value;
    hash *= 0x100000001B3ull;
}

void mix_entities(std::uint64_t &hash, std::vector<entt::entity> entities) {
    const auto entity_less = [](const entt::entity lhs, const entt::entity rhs) {
        return entt::to_integral(lhs) < entt::to_integral(rhs);
    };

    std::sort(entities.begin(), entities.end(), entity_less);
    mix(hash, entities.size());

    for(const auto entity: entities) {
        mix(hash, entt::to_integral(entity));
    }
}

[[nodiscard]] std::uint64_t digest_world(const entt::registry &registry, const world_model &model) {
    std::uint64_t hash{0xCBF29CE484222325ull};
    auto live = model.live;
    const auto entity_less = [](const entity_state &lhs, const entity_state &rhs) {
        return entt::to_integral(lhs.entity) < entt::to_integral(rhs.entity);
    };

    std::sort(live.begin(), live.end(), entity_less);

    for(const auto &elem: live) {
        mix(hash, entt::to_integral(elem.entity));

        for(std::size_t idx{}; idx < component_count; ++idx) {
            mix(hash, static_cast<std::uint64_t>(elem.owned[idx]));

            if(elem.owned[idx]) {
                mix(hash, static_cast<std::uint64_t>(elem.values[idx]));
            }
        }
    }

    mix_entities(hash, view_entities<alpha>(registry));
    mix_entities(hash, view_entities<beta>(registry));
    mix_entities(hash, view_entities<gamma>(registry));
    mix_entities(hash, view_entities<tag>(registry));
    mix(hash, gamma::alive);
    return hash;
}

template<typename Type>
void iterate_and_mutate(entt::registry &registry, world_model &model, splitmix64 &rng, run_stats &stats, trace_log *trace) {
    ++stats.iterations;
    constexpr auto kind = []() {
        if constexpr(std::is_same_v<Type, alpha>) {
            return component_kind::alpha;
        } else if constexpr(std::is_same_v<Type, beta>) {
            return component_kind::beta;
        } else if constexpr(std::is_same_v<Type, gamma>) {
            return component_kind::gamma;
        } else {
            return component_kind::tag;
        }
    }();

    log(trace, "iterate<" + std::string{component_name(kind)} + "> begin");
    const auto view = registry.view<Type>();

    for(const auto entity: view) {
        const auto *state = model.find(entity);

        if(state == nullptr || !state->owned[component_index(kind)]) {
            fail("iteration-state", "iteration yielded an entity that does not own the leading component");
        }

        ++stats.visits;
        ++stats.mutation_visits;
        log(trace, "  visit " + entity_name(entity));

        const auto roll = rng.below(10u);

        if(roll < 4u) {
            patch_component(registry, model, entity, kind, static_cast<int>(rng.below(1000u)), stats, trace);
        } else if(roll < 6u) {
            remove_component(registry, model, entity, kind, stats, trace);
        } else if(roll < 8u) {
            destroy_entity(registry, model, entity, stats, trace);
        } else if(roll < 9u) {
            if constexpr(std::is_same_v<Type, gamma>) {
                auto target = model.live[rng.below(model.live.size())].entity;

                if(rng.below(2u) == 0u) {
                    destroy_entity(registry, model, target, stats, trace);
                } else {
                    const auto *target_state = model.find(target);

                    if(target_state != nullptr && target_state->owned[component_index(component_kind::gamma)]) {
                        remove_component(registry, model, target, component_kind::gamma, stats, trace);
                    }
                }
            } else {
                const auto other = create_entity(registry, model, stats, trace);
                emplace_component(registry, model, other, kind, static_cast<int>(rng.below(1000u)), stats, trace);
            }
        } else {
            const auto other = create_entity(registry, model, stats, trace);
            emplace_component(registry, model, other, kind, static_cast<int>(rng.below(1000u)), stats, trace);
        }
    }

    log(trace, "iterate<" + std::string{component_name(kind)} + "> end");
}

void execute_iteration(entt::registry &registry, world_model &model, splitmix64 &rng, run_stats &stats, trace_log *trace) {
    const auto kind = static_cast<component_kind>(rng.below(component_count));

    if(live_with(model, kind) == 0u) {
        const auto entity = create_entity(registry, model, stats, trace);
        emplace_component(registry, model, entity, kind, static_cast<int>(rng.below(1000u)), stats, trace);
    }

    switch(kind) {
    case component_kind::alpha:
        iterate_and_mutate<alpha>(registry, model, rng, stats, trace);
        break;
    case component_kind::beta:
        iterate_and_mutate<beta>(registry, model, rng, stats, trace);
        break;
    case component_kind::gamma:
        iterate_and_mutate<gamma>(registry, model, rng, stats, trace);
        break;
    case component_kind::tag:
        iterate_and_mutate<tag>(registry, model, rng, stats, trace);
        break;
    case component_kind::count:
        std::abort();
    }
}

[[nodiscard]] std::array<operation_kind, operation_block> plan_block(splitmix64 &rng) {
    auto plan = operation_pool;

    for(std::size_t pos = plan.size(); pos > 1u; --pos) {
        const auto other = rng.below(pos);
        std::swap(plan[pos - 1u], plan[other]);
    }

    return plan;
}

void execute_operation(const operation_kind kind, entt::registry &registry, world_model &model, splitmix64 &rng, run_stats &stats, trace_log *trace) {
    ++stats.operations;

    if(model.live.empty() && kind != operation_kind::create) {
        (void)create_entity(registry, model, stats, trace);
        return;
    }

    switch(kind) {
    case operation_kind::create: {
        const auto entity = create_entity(registry, model, stats, trace);
        const auto count = rng.below(3u);

        for(std::size_t idx{}; idx < count; ++idx) {
            const auto comp = static_cast<component_kind>(rng.below(component_count));

            if(!model_state(model, entity).owned[component_index(comp)]) {
                emplace_component(registry, model, entity, comp, static_cast<int>(rng.below(1000u)), stats, trace);
            }
        }
    } break;
    case operation_kind::destroy: {
        const auto entity = model.live[rng.below(model.live.size())].entity;
        destroy_entity(registry, model, entity, stats, trace);
    } break;
    case operation_kind::add: {
        const auto entity = model.live[rng.below(model.live.size())].entity;
        auto free = std::array<component_kind, component_count>{};
        std::size_t length{};

        for(std::size_t idx{}; idx < component_count; ++idx) {
            if(!model_state(model, entity).owned[idx]) {
                free[length++] = static_cast<component_kind>(idx);
            }
        }

        if(length != 0u) {
            const auto comp = free[rng.below(length)];
            emplace_component(registry, model, entity, comp, static_cast<int>(rng.below(1000u)), stats, trace);
        }
    } break;
    case operation_kind::remove: {
        const auto entity = model.live[rng.below(model.live.size())].entity;
        auto owned = std::array<component_kind, component_count>{};
        std::size_t length{};

        for(std::size_t idx{}; idx < component_count; ++idx) {
            if(model_state(model, entity).owned[idx]) {
                owned[length++] = static_cast<component_kind>(idx);
            }
        }

        if(length != 0u) {
            remove_component(registry, model, entity, owned[rng.below(length)], stats, trace);
        }
    } break;
    case operation_kind::patch: {
        const auto entity = model.live[rng.below(model.live.size())].entity;
        auto owned = std::array<component_kind, component_count>{};
        std::size_t length{};

        for(std::size_t idx{}; idx < component_count; ++idx) {
            if(model_state(model, entity).owned[idx]) {
                owned[length++] = static_cast<component_kind>(idx);
            }
        }

        if(length != 0u) {
            patch_component(registry, model, entity, owned[rng.below(length)], static_cast<int>(rng.below(1000u)), stats, trace);
        }
    } break;
    case operation_kind::iterate:
        execute_iteration(registry, model, rng, stats, trace);
        break;
    }
}

struct run_result {
    bool failed{};
    std::string key{};
    std::string message{};
    std::size_t operation{};
    std::vector<std::uint64_t> digests{};
    run_stats stats{};
    std::vector<std::string> trace{};
};

[[nodiscard]] run_result run_case(const std::uint64_t seed, const std::vector<bool> &mask, const bool record_trace) {
    run_result result{};
    trace_log trace{};
    trace_log *trace_ptr = record_trace ? &trace : nullptr;
    splitmix64 rng{seed};
    gamma::reset();

    try {
        {
            entt::registry registry{};
            world_model model{};
            auto plan = plan_block(rng);
            result.digests.push_back(digest_world(registry, model));

            for(std::size_t pos{}; pos < operations_per_group; ++pos) {
                if((pos % operation_block) == 0u) {
                    plan = plan_block(rng);
                }

                if(pos >= mask.size() || mask[pos]) {
                    execute_operation(plan[pos % operation_block], registry, model, rng, result.stats, trace_ptr);
                }

                check_world(registry, model);
                result.digests.push_back(digest_world(registry, model));
            }
        }

        if(gamma::alive != 0u) {
            fail("object-leak", "gamma instances survived registry destruction");
        }
    } catch(const property_failure &error) {
        result.failed = true;
        result.key = error.key;
        result.message = error.message;
        result.operation = result.stats.operations;
        result.trace = std::move(trace.lines);
    }

    return result;
}

[[nodiscard]] std::size_t enabled_operations(const std::vector<bool> &mask) {
    return static_cast<std::size_t>(std::count(mask.begin(), mask.end(), true));
}

[[nodiscard]] std::vector<bool> minimize(const std::uint64_t seed, const run_result &failure) {
    std::vector<bool> mask(operations_per_group, true);
    auto current = run_case(seed, mask, false);
    std::size_t step = operations_per_group;

    while(step > 1u) {
        step = (step + 1u) / 2u;

        for(std::size_t begin{}; begin < operations_per_group; begin += step) {
            auto candidate = mask;
            const auto end = std::min(begin + step, operations_per_group);

            for(auto pos = begin; pos < end; ++pos) {
                candidate[pos] = false;
            }

            auto reduced = run_case(seed, candidate, false);

            if(reduced.failed && reduced.key == failure.key) {
                mask = std::move(candidate);
                current = std::move(reduced);
            }
        }
    }

    for(std::size_t pos{}; pos < operations_per_group; ++pos) {
        if(mask[pos]) {
            auto candidate = mask;
            candidate[pos] = false;
            auto reduced = run_case(seed, candidate, false);

            if(reduced.failed && reduced.key == failure.key) {
                mask = std::move(candidate);
                current = std::move(reduced);
            }
        }
    }

    return mask;
}

[[nodiscard]] std::string format_failure(const std::uint64_t seed, const std::vector<bool> &mask, const run_result &failure) {
    std::ostringstream output;
    output << "seed=" << seed << " key=" << failure.key << " reduced_operations=" << enabled_operations(mask) << "\n";
    output << "failure: " << failure.message << "\n";
    output << "minimal operation sequence:\n";

    for(const auto &line: failure.trace) {
        output << "  " << line << "\n";
    }

    return output.str();
}

struct suite_result {
    bool failed{};
    std::uint64_t seed{};
    std::vector<std::uint64_t> digests{};
    run_stats stats{};
    std::string report{};
};

[[nodiscard]] suite_result run_suite(const std::uint64_t seed_base) {
    suite_result result{};

    for(std::size_t group{}; group < groups; ++group) {
        const auto seed = seed_base + static_cast<std::uint64_t>(group) * 0x9E3779B97F4A7C15ull;
        const std::vector<bool> mask(operations_per_group, true);
        auto current = run_case(seed, mask, false);

        result.stats.operations += current.stats.operations;
        result.stats.creates += current.stats.creates;
        result.stats.destroys += current.stats.destroys;
        result.stats.adds += current.stats.adds;
        result.stats.removes += current.stats.removes;
        result.stats.patches += current.stats.patches;
        result.stats.iterations += current.stats.iterations;
        result.stats.visits += current.stats.visits;
        result.stats.mutation_visits += current.stats.mutation_visits;
        result.digests.insert(result.digests.end(), current.digests.begin(), current.digests.end());

        if(current.failed) {
            const auto reduced = minimize(seed, current);
            auto report = run_case(seed, reduced, true);
            result.failed = true;
            result.seed = seed;
            result.report = format_failure(seed, reduced, report);
            return result;
        }
    }

    return result;
}

[[nodiscard]] std::string format_summary(const suite_result &first, const suite_result *second) {
    std::ostringstream output;
    const auto &stats = first.stats;
    output << "property test summary\n"
           << "  groups: " << groups << "\n"
           << "  operations: " << stats.operations << "\n"
           << "  creates: " << stats.creates << "\n"
           << "  destroys: " << stats.destroys << "\n"
           << "  component adds: " << stats.adds << "\n"
           << "  component removes: " << stats.removes << "\n"
           << "  component patches: " << stats.patches << "\n"
           << "  traversal starts: " << stats.iterations << "\n"
           << "  traversal visits: " << stats.visits << "\n"
           << "  mutations during traversal: " << stats.mutation_visits << "\n"
           << "  process crashes: 0\n";

    if(second != nullptr) {
        output << "  two-run state digests: " << (first.digests == second->digests ? "identical" : "DIFFERENT") << "\n"
               << "  digest checkpoints: " << first.digests.size() << "\n";
    }

    return output.str();
}

} // namespace

TEST(EntityProperty, RandomCreateDestroyComponentsAndMutationDuringIteration) {
    const auto first = run_suite(0x4042C0FFEEull);

    if(first.failed) {
        FAIL() << first.report;
    }

    const auto second = run_suite(0x4042C0FFEEull);

    if(second.failed) {
        FAIL() << second.report;
    }

    ASSERT_EQ(first.digests, second.digests);
    ASSERT_EQ(first.stats.operations, second.stats.operations);
    ASSERT_EQ(first.stats.visits, second.stats.visits);
    ASSERT_EQ(first.stats.mutation_visits, second.stats.mutation_visits);
    ASSERT_EQ(first.stats.operations, groups * operations_per_group);

    std::cout << format_summary(first, &second) << std::flush;
}
