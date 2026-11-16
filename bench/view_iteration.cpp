// Three-scale component traversal benchmark.
//
// The data set shuffles component insertion order, excludes tagged entities,
// and destroys a few entities so that the packed arrays differ and sparse
// lookups are not accidentally sequential.

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
constexpr int rounds = 11;

entt::registry make_registry(const std::size_t count) {
    entt::registry registry;
    std::vector<entt::entity> entities(count);
    registry.create(entities.begin(), entities.end());

    std::mt19937_64 prng{0x5eedu};
    std::shuffle(entities.begin(), entities.end(), prng);

    for(const auto entt: entities) {
        registry.emplace<position>(entt, 1.0f, 2.0f);
    }

    std::shuffle(entities.begin(), entities.end(), prng);

    for(const auto entt: entities) {
        registry.emplace<velocity>(entt, 0.5f, 0.25f);
    }

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

template<typename Setup, typename Run>
double best_of(const std::size_t count, Setup setup, Run run) {
    auto best = std::chrono::duration<double, std::milli>{1e9};

    for(int round = 0; round < rounds; ++round) {
        auto registry = make_registry(count);
        auto state = setup(registry);

        const auto start = std::chrono::steady_clock::now();
        run(registry, state);
        const auto stop = std::chrono::steady_clock::now();

        best = std::min(best, std::chrono::duration<double, std::milli>{stop - start});
    }

    return best.count();
}

volatile float sink{};

double view_range_for(const std::size_t count) {
    return best_of(
        count,
        [](entt::registry &registry) {
            return registry.view<position, velocity>(entt::exclude<dead>);
        },
        [](entt::registry &, auto &view) {
            for(auto [entt, pos, vel]: view.each()) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
                static_cast<void>(entt);
            }
        });
}

double view_each_func(const std::size_t count) {
    return best_of(
        count,
        [](entt::registry &registry) {
            return registry.view<position, velocity>(entt::exclude<dead>);
        },
        [](entt::registry &, auto &view) {
            view.each([](auto &pos, auto &vel) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
            });
        });
}

double group_range_for(const std::size_t count) {
    return best_of(
        count,
        [](entt::registry &registry) {
            return registry.group<position, velocity>(entt::get<>, entt::exclude<dead>);
        },
        [](entt::registry &, auto &group) {
            for(auto [entt, pos, vel]: group.each()) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
                static_cast<void>(entt);
            }
        });
}

void order_by_entity(entt::registry &registry) {
    registry.sort<position>([](const entt::entity lhs, const entt::entity rhs) {
        return static_cast<std::uint32_t>(lhs) < static_cast<std::uint32_t>(rhs);
    });
    registry.sort<velocity, position>();
}

double sorted_view_range_for(const std::size_t count) {
    return best_of(
        count,
        [](entt::registry &registry) {
            order_by_entity(registry);
            return registry.view<position, velocity>(entt::exclude<dead>);
        },
        [](entt::registry &, auto &view) {
            for(auto [entt, pos, vel]: view.each()) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
                static_cast<void>(entt);
            }
        });
}

double sorted_view_each_func(const std::size_t count) {
    return best_of(
        count,
        [](entt::registry &registry) {
            order_by_entity(registry);
            return registry.view<position, velocity>(entt::exclude<dead>);
        },
        [](entt::registry &, auto &view) {
            view.each([](auto &pos, auto &vel) {
                pos.x += vel.x;
                pos.y += vel.y;
                sink = pos.x + pos.y;
            });
        });
}

void mix(std::uint64_t &hash, const entt::entity entt, const position &pos) {
    hash = (hash ^ static_cast<std::uint32_t>(entt)) * 1099511628211ull;
    hash = (hash ^ __builtin_bit_cast(std::uint32_t, pos.x)) * 1099511628211ull;
    hash = (hash ^ __builtin_bit_cast(std::uint32_t, pos.y)) * 1099511628211ull;
}

template<typename Each>
std::uint64_t fingerprint_each(Each each) {
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t visited{};

    each([&](const auto entt, auto &pos, auto &vel) {
        pos.x += vel.x;
        pos.y += vel.y;
        mix(hash, entt, pos);
        ++visited;
    });

    return hash ^ (visited * 1099511628211ull);
}

template<typename Range>
std::uint64_t fingerprint_range(Range range) {
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t visited{};

    for(auto [entt, pos, vel]: range) {
        pos.x += vel.x;
        pos.y += vel.y;
        mix(hash, entt, pos);
        ++visited;
    }

    return hash ^ (visited * 1099511628211ull);
}

void dump_fingerprints() {
    for(const auto count: scales) {
        {
            auto registry = make_registry(count);
            const auto hash = fingerprint_range(registry.view<position, velocity>(entt::exclude<dead>).each());
            std::printf("%zu view-range %016zx\n", count, hash);
        }

        {
            auto registry = make_registry(count);
            auto view = registry.view<position, velocity>(entt::exclude<dead>);
            const auto hash = fingerprint_each([&](auto &&func) { view.each(func); });
            std::printf("%zu view-each  %016zx\n", count, hash);
        }

        {
            auto registry = make_registry(count);
            const auto hash = fingerprint_range(registry.group<position, velocity>(entt::get<>, entt::exclude<dead>).each());
            std::printf("%zu group      %016zx\n", count, hash);
        }

        {
            auto registry = make_registry(count);
            order_by_entity(registry);
            const auto hash = fingerprint_range(registry.view<position, velocity>(entt::exclude<dead>).each());
            std::printf("%zu sorted     %016zx\n", count, hash);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if((argc > 1) && (argv[1][0] == 'd')) {
        dump_fingerprints();
        return 0;
    }

    std::puts("entities,view_range_ms,view_each_ms,sorted_range_ms,sorted_each_ms,group_range_ms,view_range_ns,view_each_ns,sorted_range_ns,sorted_each_ns,group_range_ns");

    for(const auto count: scales) {
        const auto range = view_range_for(count);
        const auto each = view_each_func(count);
        const auto sorted_range = sorted_view_range_for(count);
        const auto sorted_each = sorted_view_each_func(count);
        const auto group = group_range_for(count);
        const auto scale = static_cast<double>(count);

        std::printf("%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                    count,
                    range,
                    each,
                    sorted_range,
                    sorted_each,
                    group,
                    range * 1e6 / scale,
                    each * 1e6 / scale,
                    sorted_range * 1e6 / scale,
                    sorted_each * 1e6 / scale,
                    group * 1e6 / scale);
    }

    return 0;
}
