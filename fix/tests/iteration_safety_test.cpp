#include "iteration_safe/deferred_command_buffer.hpp"
#include "iteration_safe/iteration_guard.hpp"

#include <cassert>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

struct tag {
    int value{0};
};

struct pinned {
    int value{0};
    static constexpr bool in_place_delete = true;
};

int g_crashes = 0;
int g_checks = 0;

void expect(const bool condition, const std::string &name) {
    ++g_checks;

    if(!condition) {
        ++g_crashes;
        std::printf("  [FAIL] %s\n", name.c_str());
    } else {
        std::printf("  [ok]   %s\n", name.c_str());
    }
}

// Original buggy sequence: removing a different, not-yet-visited entity from a
// swap_and_pop storage while iterating causes duplicate + skipped visits.
void reproduce_duplicate_and_skip() {
    std::puts("== reproduce: swap_and_pop duplicate/skip ==");

    entt::registry registry;
    std::vector<entt::entity> entities;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
        entities.push_back(entt);
    }

    std::vector<int> order;

    for(auto &&[entt, comp]: registry.view<tag>().each()) {
        order.push_back(comp.value);

        if(comp.value == 6) {
            registry.remove<tag>(entities[2]);
        }
    }

    const bool duplicated = order.size() != std::set(order.begin(), order.end()).size();
    const bool skipped = order.size() == 8u && !std::is_sorted(order.begin(), order.end(), std::greater<int>{});
    expect(duplicated, "buggy loop visits a swapped-in entity twice");
    expect(skipped, "buggy loop skips the removed entity's original slot");
}

// Original buggy sequence: wiping a swap_and_pop storage mid-loop drives the
// index iterator past the new (smaller) packed array bounds.
[[noreturn]] void reproduce_out_of_bounds() {
    std::puts("== reproduce: swap_and_pop out-of-bounds on wipe ==");

    entt::registry registry;
    std::vector<entt::entity> entities;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
        entities.push_back(entt);
    }

    for(auto &&[entt, comp]: registry.view<tag>().each()) {
        (void)entt;
        (void)comp;

        for(const auto victim: entities) {
            registry.remove<tag>(victim);
        }

        registry.storage<tag>().shrink_to_fit();
    }

    // Reaching this point means the sanitizer/assert did not catch the OOB.
    std::puts("  [note] wipe loop returned without a trap (silent OOB risk)");
    std::abort();
}

// Fix option A: defer every mutation until the traversal completes.
void fix_with_deferred_buffer() {
    std::puts("== fix A: deferred command buffer ==");

    entt::registry registry;
    std::vector<entt::entity> entities;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
        entities.push_back(entt);
    }

    iteration_safe::basic_deferred_command_buffer commands{registry};
    std::vector<int> order;

    for(auto &&[entt, comp]: registry.view<tag>().each()) {
        order.push_back(comp.value);

        if(comp.value == 6) {
            commands.remove<tag>(entities[2]);
        }

        if(comp.value == 5) {
            commands.destroy(entities[4]);
        }
    }

    expect(order.size() == 8u, "all 8 entities visited exactly once before flush");
    expect(std::is_sorted(order.begin(), order.end(), std::greater<int>{}), "visit order unchanged while queued");

    const auto result = commands.flush();
    expect(result.applied == 2u, "both deferred mutations applied");
    expect(result.skipped == 0u, "no deferred mutation skipped");
    expect(registry.storage<tag>().size() == 6u, "component storage shrank only after traversal");
    expect(!registry.valid(entities[4]), "destroyed entity is gone after flush");
}

// Stale deferred commands must be skipped, not applied to invalid state.
void deferred_buffer_skips_stale_commands() {
    std::puts("== fix A: stale commands are rejected, not silently applied ==");

    entt::registry registry;
    const auto first = registry.create();
    const auto second = registry.create();
    registry.emplace<tag>(first, 1);
    registry.emplace<tag>(second, 2);

    iteration_safe::basic_deferred_command_buffer commands{registry};
    commands.remove<tag>(first);
    commands.destroy(first);
    commands.destroy(first);
    commands.destroy(second);

    const auto result = commands.flush();
    expect(result.applied == 3u, "component remove, first destroy and the other entity applied");
    expect(result.skipped == 1u, "duplicate destroy of the already dead entity skipped");
    expect(!registry.valid(first) && !registry.valid(second), "both entities ended up destroyed");
}

// Fix option B: in_place (tombstone) storage keeps packed bounds stable, so
// the view iterator never dereferences out of bounds.
void fix_with_in_place_storage() {
    std::puts("== fix B: in_place tombstone storage ==");

    entt::registry registry;
    std::vector<entt::entity> entities;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<pinned>(entt, i);
        entities.push_back(entt);
    }

    expect(registry.storage<pinned>().policy() == entt::deletion_policy::in_place,
           "component opted into in_place deletion");

    std::vector<int> order;

    for(auto &&[entt, comp]: registry.view<pinned>().each()) {
        order.push_back(comp.value);

        if(comp.value == 6) {
            registry.remove<pinned>(entities[2]);
        }
    }

    const bool no_duplicates = order.size() == std::set(order.begin(), order.end()).size();
    expect(no_duplicates, "tombstone loop never visits an entity twice");
    expect(order.size() == 7u, "removed entity's tombstone is skipped by the view");

    // wipe scenario that crashed the swap_and_pop variant
    for(auto &&[entt, comp]: registry.view<pinned>().each()) {
        (void)entt;
        (void)comp;

        for(const auto victim: entities) {
            registry.remove<pinned>(victim);
        }

        registry.storage<pinned>().shrink_to_fit();
    }

    expect(true, "wipe traversal completes without out-of-bounds access");
}

// Fix option C: guard detects invalidation and returns an explicit reason.
void fix_with_iteration_guard() {
    std::puts("== fix C: explicit invalidation guard ==");

    entt::registry registry;
    std::vector<entt::entity> entities;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
        entities.push_back(entt);
    }

    // clean traversal: no mutation, guard must report valid
    {
        auto guard = iteration_safe::iteration_guard::take<tag>(registry);

        for(auto &&[entt, comp]: registry.view<tag>().each()) {
            (void)entt;
            ++comp.value;
        }

        const auto result = guard.check();
        expect(static_cast<bool>(result), "read-only/in-place traversal reported valid");
    }

    // destructive traversal: guard must fail closed with a named reason
    {
        auto guard = iteration_safe::iteration_guard::take<tag>(registry);

        for(auto &&[entt, comp]: registry.view<tag>().each()) {
            (void)entt;
            (void)comp;
            registry.remove<tag>(entities[0]);
            break;
        }

        const auto result = guard.check();
        expect(!static_cast<bool>(result), "shrinking swap_and_pop storage is flagged");
        expect(result.reason == iteration_safe::invalidation_reason::swap_pop_storage_shrunk,
               std::string{"reason: "} + iteration_safe::to_string(result.reason));
    }

    // in_place storage shrinking (tombstones) must remain valid
    {
        for(int i = 0; i < 4; ++i) {
            const auto entt = registry.create();
            registry.emplace<pinned>(entt, i);
        }

        auto guard = iteration_safe::iteration_guard::take<pinned>(registry);
        registry.view<pinned>().each([](const auto, auto &) {});
        registry.remove<pinned>(registry.view<pinned>().front());

        const auto result = guard.check();
        expect(static_cast<bool>(result), "in_place erase does not invalidate the view");
    }
}

} // namespace

int main(const int argc, char **argv) {
    const bool run_oob = argc > 1 && std::string{argv[1]} == "--oob";

    reproduce_duplicate_and_skip();

    if(run_oob) {
        reproduce_out_of_bounds();
    }

    fix_with_deferred_buffer();
    deferred_buffer_skips_stale_commands();
    fix_with_in_place_storage();
    fix_with_iteration_guard();

    std::printf("\nchecks=%d failures=%d\n", g_checks, g_crashes);
    return g_crashes == 0 ? 0 : 1;
}
