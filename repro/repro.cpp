#include <entt/entt.hpp>
#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <vector>

// movable component: default storage policy is swap_and_pop
struct tag {
    int value{0};
};

// opt-in tombstone (in_place) storage
struct pinned_like {
    int value{0};
    static constexpr bool in_place_delete = true;
};

static int to_id(const entt::entity entt) {
    return static_cast<int>(static_cast<std::underlying_type_t<entt::entity>>(entt) & 0xFFFFF);
}

static void print_order(const char *name, const std::vector<int> &order, const std::vector<int> &unique_seen) {
    std::printf("             %s order:", name);
    for(const int id: order) {
        std::printf(" %d", id);
    }
    std::printf("\n             unique entities visited=%zu\n", unique_seen.size());
}

static void track(const entt::entity entt, std::vector<int> &order, std::vector<int> &unique_seen) {
    const int id = to_id(entt);
    order.push_back(id);
    if(std::find(unique_seen.begin(), unique_seen.end(), id) == unique_seen.end()) {
        unique_seen.push_back(id);
    }
}

// Scenario 1: erase the current entity while iterating a swap_and_pop storage.
// swap_and_pop moves the last element into the erased slot while the iterator
// walks by index: the moved element is visited twice and another one skipped.
static void scenario_erase_current_swap_pop() {
    entt::registry registry;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
    }

    int visited = 0;
    std::vector<int> order;
    std::vector<int> unique_seen;

    for(auto &&[entt, comp]: registry.view<tag>().each()) {
        ++visited;
        track(entt, order, unique_seen);
        if(comp.value == 2) {
            registry.remove<tag>(entt);
        }
    }

    std::printf("[scenario 1] erase-current (swap_and_pop): loop iterations=%d expected=8, storage size=%zu\n",
                visited, registry.storage<tag>().size());
    print_order("visit", order, unique_seen);
}

// Scenario 2: destroy the current entity while iterating.
static void scenario_destroy_during_iteration() {
    entt::registry registry;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
    }

    int visited = 0;
    std::vector<int> order;
    std::vector<int> unique_seen;

    for(auto &&[entt, comp]: registry.view<tag>().each()) {
        ++visited;
        track(entt, order, unique_seen);
        if(comp.value == 3) {
            registry.destroy(entt);
        }
    }

    std::printf("[scenario 2] destroy-during-iteration: loop iterations=%d expected=8, alive entities=%zu\n",
                visited, registry.storage<entt::entity>().size());
    print_order("visit", order, unique_seen);
}

// Scenario 3: emplace into the same storage while iterating and force a
// reallocation. The sparse_set_iterator caches packed.data(), so after the
// vector moves its buffer every subsequent dereference is a dangling access.
static void scenario_emplace_during_iteration() {
    entt::registry registry;

    for(int i = 0; i < 1024; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
    }

    int visited = 0;
    int garbage_reads = 0;

    for(auto &&[entt, comp]: registry.view<tag>().each()) {
        ++visited;
        if(comp.value < -1 || comp.value >= 1024) {
            ++garbage_reads;
        }
        if(comp.value == 0) {
            // blow past current capacity to force the packed vector to reallocate
            for(int n = 0; n < 8192; ++n) {
                const auto next = registry.create();
                registry.emplace<tag>(next, -1);
            }
        }
    }

    std::printf("[scenario 3] emplace-during-iteration: loop iterations=%d (original set=1024), garbage component reads=%d, storage size=%zu\n",
                visited, garbage_reads, registry.storage<tag>().size());
}

// Scenario 4: multi-pool view, remove from the non-leading pool.
static void scenario_remove_from_other_pool() {
    entt::registry registry;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
        registry.emplace<pinned_like>(entt, i);
    }

    int visited = 0;
    std::vector<int> order;
    std::vector<int> unique_seen;

    auto view = registry.view<tag, pinned_like>();

    for(auto &&[entt, comp, pinned]: view.each()) {
        (void)pinned;
        ++visited;
        track(entt, order, unique_seen);
        if(comp.value == 1) {
            registry.remove<pinned_like>(entt);
        }
    }

    std::printf("[scenario 4] remove-from-other-pool: loop iterations=%d expected=8\n", visited);
    print_order("visit", order, unique_seen);
}

// Scenario 5: raw storage iteration with erase, shows swapped duplication.
static void scenario_raw_storage_erase() {
    entt::registry registry;

    for(int i = 0; i < 8; ++i) {
        const auto entt = registry.create();
        registry.emplace<tag>(entt, i);
    }

    auto &storage = registry.storage<tag>();
    int visited = 0;
    std::vector<int> order;
    std::vector<int> unique_seen;

    for(auto &&[entt, comp]: storage.each()) {
        ++visited;
        track(entt, order, unique_seen);
        if(comp.value == 2) {
            storage.remove(entt);
        }
    }

    std::printf("[scenario 5] raw-storage-erase: loop iterations=%d, storage size=%zu\n",
                visited, storage.size());
    print_order("visit", order, unique_seen);
}

int main() {
    scenario_erase_current_swap_pop();
    scenario_destroy_during_iteration();
    scenario_emplace_during_iteration();
    std::printf("----\n");
    scenario_remove_from_other_pool();
    scenario_raw_storage_erase();
    std::printf("done\n");
    return 0;
}
