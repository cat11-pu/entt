// Benchmark: multi-component view iteration at three scales.
// Measures range-for and func-based each() traversal, plus a deterministic
// order/semantics fingerprint for correctness comparison across revisions.

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

struct position {
    float x{};
    float y{};
};

struct velocity {
    float x{};
    float y{};
};

struct dead {};

namespace {

constexpr std::size_t scales[] = {10'000u, 100'000u, 1'000'000u};
constexpr int rounds = 9;

entt::registry make_registry(const std::size_t count) {
    entt::registry registry;
    std::vector<entt::entity> entities(count);
    registry.create(entities.begin(), entities.end());

    // deterministic shuffle: pools end up with different packed orders,
    // so cross-pool lookups are genuinely cache-unfriendly
    std::mt19937_64 prng{0x5eedu};
    std::shuffle(entities.begin(), entities.end(), prng);

    for(const auto entt: entities) {
        registry.emplace<position>(entt, 1.0f, 2.0f);
    }

    std::shuffle(entities.begin(), entities.end(), prng);

    for(const auto entt: entities) {
        registry.emplace<velocity>(entt, 0.5f, 0.25f);
    }

    // ~10% excluded via tag, ~5% destroyed to open holes in the packed arrays
    std::uniform_int_distribution<std::size_t> pick{0u, 99u};

    for(const auto entt: entities) {
        if(const auto roll = pick(prng); roll < 10u) {
            registry.emplace<dead>(entt);
        } else if(roll < 15u) {
            registry.destroy(entt);
        }
    }

    return registry;
}

template<typename Func>
double time_best(const std::size_t count, Func func) {
    auto best = std::chrono::duration<double, std::milli>{1e9};

    for(int round = 0; round < rounds; ++round) {
        auto registry = make_registry(count);
        const auto start = std::chrono::steady_clock::now();
        func(registry);
        const auto stop = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>{stop - start});
    }

    return best.count();
}

volatile float sink = 0.0f;

// order/semantics fingerprint: hashes the visited entity sequence and the
// resulting component state, so any behavioral drift shows up as a mismatch
std::uint64_t fingerprint_range_for(entt::registry &registry) {
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t visited = 0u;

    for(auto [entt, pos, vel]: registry.view<position, velocity>(entt::exclude<dead>).each()) {
        pos.x += vel.x;
        pos.y += vel.y;
        hash = (hash ^ static_cast<std::uint32_t>(entt)) * 1099511628211ull;
        hash = (hash ^ __builtin_bit_cast(std::uint32_t, pos.x)) * 1099511628211ull;
        hash = (hash ^ __builtin_bit_cast(std::uint32_t, pos.y)) * 1099511628211ull;
        ++visited;
    }

    return hash ^ (visited * 1099511628211ull);
}

std::uint64_t fingerprint_each(entt::registry &registry) {
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t visited = 0u;

    registry.view<position, velocity>(entt::exclude<dead>).each([&](const auto entt, auto &pos, auto &vel) {
        pos.x += vel.x;
        pos.y += vel.y;
        hash = (hash ^ static_cast<std::uint32_t>(entt)) * 1099511628211ull;
        hash = (hash ^ __builtin_bit_cast(std::uint32_t, pos.x)) * 1099511628211ull;
        hash = (hash ^ __builtin_bit_cast(std::uint32_t, pos.y)) * 1099511628211ull;
        ++visited;
    });

    return hash ^ (visited * 1099511628211ull);
}

} // namespace

int main(int argc, char **argv) {
    const bool dump = (argc > 1) && (argv[1][0] == 'd');

    if(dump) {
        for(const auto count: scales) {
            auto first = make_registry(count);
            auto second = make_registry(count);
            std::printf("%zu range-for %016zx\n", count, fingerprint_range_for(first));
            std::printf("%zu each-func %016zx\n", count, fingerprint_each(second));
        }

        return 0;
    }

    for(const auto count: scales) {
        const auto range_for = time_best(count, [](entt::registry &registry) {
            for(auto [entt, pos, vel]: registry.view<position, velocity>(entt::exclude<dead>).each()) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
            }
        });

        const auto each_func = time_best(count, [](entt::registry &registry) {
            registry.view<position, velocity>(entt::exclude<dead>).each([](auto &pos, auto &vel) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
            });
        });

        std::printf("%9zu entities | range-for %8.3f ms | each(func) %8.3f ms\n", count, range_for, each_func);
    }

    return 0;
}
