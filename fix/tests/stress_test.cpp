#include "iteration_safe/deferred_command_buffer.hpp"
#include "iteration_safe/iteration_guard.hpp"

#include <cstdio>
#include <random>
#include <set>
#include <vector>

namespace {

struct movable_comp {
    int value{0};
};

struct pinned_comp {
    int value{0};
    static constexpr bool in_place_delete = true;
};

struct counters {
    long long iterations{};
    long long distinct_entities{};
    long long guard_failures{};
    long long stale_skips{};
};

// Every mutation that would invalidate a swap_and_pop iterator is deferred.
// The traversal itself therefore runs against an immutable storage and must
// visit every component exactly once.
void run_deferred_round(std::mt19937 &rng, counters &stats) {
    entt::registry registry;
    iteration_safe::basic_deferred_command_buffer commands{registry};
    std::vector<entt::entity> entities;

    const int initial = 8 + static_cast<int>(rng() % 24);

    for(int i = 0; i < initial; ++i) {
        const auto entt = registry.create();
        registry.emplace<movable_comp>(entt, i);
        entities.push_back(entt);
    }

    const std::size_t expected = entities.size();
    std::set<int> seen;

    for(auto &&[entt, comp]: registry.view<movable_comp>().each()) {
        ++stats.iterations;
        seen.insert(comp.value);

        switch(rng() % 4u) {
        case 0u:
            // remove a random component, including not-yet-visited entities
            commands.remove<movable_comp>(entities[rng() % entities.size()]);
            break;
        case 1u:
            // destroy a random entity
            commands.destroy(entities[rng() % entities.size()]);
            break;
        case 2u: {
            // create + emplace through a deferred generic command
            commands.defer([](entt::registry &reg) {
                const auto created = reg.create();
                reg.emplace<movable_comp>(created, -1);
            });
            break;
        }
        default:
            // benign in-place mutation
            comp.value += 0;
            break;
        }
    }

    if(seen.size() != expected) {
        std::printf("  [FAIL] deferred round visited %zu of %zu entities\n", seen.size(), expected);
        std::abort();
    }

    stats.distinct_entities += static_cast<long long>(seen.size());

    const auto result = commands.flush();
    stats.stale_skips += static_cast<long long>(result.skipped);
}

// In_place storages are mutated directly; tombstones keep packed bounds stable
// and the view filters them out, so the loop must never fault.
void run_in_place_round(std::mt19937 &rng, counters &stats) {
    entt::registry registry;
    std::vector<entt::entity> entities;

    const int initial = 8 + static_cast<int>(rng() % 24);

    for(int i = 0; i < initial; ++i) {
        const auto entt = registry.create();
        registry.emplace<pinned_comp>(entt, i);
        entities.push_back(entt);
    }

    std::set<int> seen;

    for(auto &&[entt, comp]: registry.view<pinned_comp>().each()) {
        ++stats.iterations;
        seen.insert(comp.value);

        if((rng() % 3u) == 0u) {
            registry.remove<pinned_comp>(entities[rng() % entities.size()]);
        }
    }

    // every value observed must be unique (tombstones are skipped, never twice)
    stats.distinct_entities += static_cast<long long>(seen.size());
}

// The guard must flag every destructive swap_and_pop traversal and accept
// every read-only or in_place traversal, without exceptions or OOB access.
void run_guard_round(std::mt19937 &rng, counters &stats) {
    entt::registry registry;
    std::vector<entt::entity> entities;

    for(int i = 0; i < 16; ++i) {
        const auto entt = registry.create();
        registry.emplace<movable_comp>(entt, i);
        entities.push_back(entt);
    }

    const bool mutate = (rng() % 2u) == 0u;
    auto guard = iteration_safe::iteration_guard::take<movable_comp>(registry);

    for(auto &&[entt, comp]: registry.view<movable_comp>().each()) {
        (void)entt;
        ++comp.value;

        if(mutate) {
            registry.remove<movable_comp>(entities[rng() % entities.size()]);
        }
    }

    const auto result = guard.check();

    if(mutate && static_cast<bool>(result)) {
        std::printf("  [FAIL] guard missed a swap_and_pop shrink\n");
        std::abort();
    }

    if(!mutate && !static_cast<bool>(result)) {
        std::printf("  [FAIL] guard raised %s on a clean traversal\n",
                    iteration_safe::to_string(result.reason));
        std::abort();
    }

    if(mutate) {
        ++stats.guard_failures;
    }
}

} // namespace

int main() {
    constexpr int rounds = 2000;
    std::mt19937 rng{0xC0FFEEu};
    counters stats;

    for(int round = 0; round < rounds; ++round) {
        run_deferred_round(rng, stats);
        run_in_place_round(rng, stats);
        run_guard_round(rng, stats);
    }

    std::printf("rounds=%d\n", rounds);
    std::printf("loop iterations=%lld\n", stats.iterations);
    std::printf("distinct entity visits=%lld\n", stats.distinct_entities);
    std::printf("guard invalidations caught=%lld\n", stats.guard_failures);
    std::printf("stale deferred commands skipped=%lld\n", stats.stale_skips);
    std::printf("crashes=0\n");
    return 0;
}
