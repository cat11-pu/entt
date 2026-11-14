#ifndef ENTT_ENTITY_QUERY_CACHE_HPP
#define ENTT_ENTITY_QUERY_CACHE_HPP

#include "../config/config.h"
#include "../container/dense_map.hpp"
#include "../core/algorithm.hpp"
#include "../core/type_info.hpp"
#include "../core/type_traits.hpp"
#include "../signal/sigh.hpp"
#include "../stl/cstddef.hpp"
#include "../stl/functional.hpp"
#include "../stl/memory.hpp"
#include "../stl/type_traits.hpp"
#include "../stl/utility.hpp"
#include "../stl/vector.hpp"
#include "entity.hpp"
#include "fwd.hpp"
#include "registry.hpp"
#include "view.hpp"

namespace entt {

/*! @cond ENTT_INTERNAL */
namespace internal {

struct query_cache_key {
    stl::vector<id_type> get{};
    stl::vector<id_type> exclude{};

    bool operator==(const query_cache_key &) const = default;
};

struct query_cache_key_hash {
    [[nodiscard]] stl::size_t operator()(const query_cache_key &key) const noexcept {
        stl::size_t value{};
        const auto combine = [&value](const id_type id) { value ^= static_cast<stl::size_t>(id) + stl::size_t{0x9e3779b9u} + (value << 6u) + (value >> 2u); };

        for(const auto id: key.get) {
            combine(id);
        }

        combine(id_type{});

        for(const auto id: key.exclude) {
            combine(id);
        }

        return value;
    }
};

} // namespace internal
/*! @endcond */

/**
 * @brief Query cache for component compositions.
 *
 * A query cache stores the result of a view for a given component composition
 * and reuses it on subsequent identical queries, instead of rebuilding the
 * view candidate set every time.<br/>
 * Cached results are automatically invalidated when an entity is created or
 * destroyed or when a component of interest is added to or removed from an
 * entity. Updating the value of a component does not invalidate the cache,
 * since it doesn't affect the matching set.
 *
 * @note The returned entities are the same ones a view would return for the
 * same composition. Their order matches the view iteration order as of the
 * last cache rebuild.
 *
 * @warning The cache is not thread-safe. Concurrent queries and mutations on
 * the same registry require external synchronization.
 *
 * @tparam Registry Type of registry to which the cache is bound.
 */
template<typename Registry>
class basic_query_cache {
    struct query_entry {
        stl::vector<typename Registry::entity_type> entities{};
        stl::vector<scoped_connection> connections{};
        bool dirty{true};

        void invalidate(Registry &, const typename Registry::entity_type) noexcept {
            dirty = true;
        }
    };

    using map_type = dense_map<internal::query_cache_key, stl::unique_ptr<query_entry>, internal::query_cache_key_hash, stl::equal_to<>, stl::allocator<stl::pair<const internal::query_cache_key, stl::unique_ptr<query_entry>>>>;

    template<typename... Get, typename... Exclude>
    [[nodiscard]] static internal::query_cache_key key_for(exclude_t<Exclude...>) {
        internal::query_cache_key key{{type_hash<Get>::value()...}, {type_hash<Exclude>::value()...}};
        std_sort{}(key.get.begin(), key.get.end());
        std_sort{}(key.exclude.begin(), key.exclude.end());
        return key;
    }

    template<typename... Get, typename... Exclude>
    void bind(query_entry &entry, exclude_t<Exclude...>) {
        (entry.connections.emplace_back(reg->template on_construct<Get>().template connect<&query_entry::invalidate>(entry)), ...);
        (entry.connections.emplace_back(reg->template on_destroy<Get>().template connect<&query_entry::invalidate>(entry)), ...);
        (entry.connections.emplace_back(reg->template on_construct<Exclude>().template connect<&query_entry::invalidate>(entry)), ...);
        (entry.connections.emplace_back(reg->template on_destroy<Exclude>().template connect<&query_entry::invalidate>(entry)), ...);
    }

    template<typename... Get, typename... Exclude>
    void rebuild(query_entry &entry, exclude_t<Exclude...> excludes) {
        entry.entities.clear();

        for(const auto entt: reg->template view<Get...>(excludes)) {
            entry.entities.push_back(entt);
        }

        entry.dirty = false;
    }

    template<typename... Get, typename... Exclude>
    [[nodiscard]] const stl::vector<typename Registry::entity_type> &fetch(exclude_t<Exclude...> excludes) {
        query_entry *entry{};

        if(auto it = entries.find(key_for<Get...>(excludes)); it != entries.end()) {
            entry = it->second.get();
        } else {
            auto elem = stl::make_unique<query_entry>();
            entry = elem.get();
            bind<Get...>(*entry, excludes);
            entries.emplace(key_for<Get...>(excludes), stl::move(elem));
        }

        if(entry->dirty) {
            rebuild<Get...>(*entry, excludes);
            ++miss_counter;
        } else {
            ++hit_counter;
        }

        return entry->entities;
    }

public:
    /*! @brief Registry type. */
    using registry_type = Registry;
    /*! @brief Underlying entity identifier. */
    using entity_type = typename registry_type::entity_type;
    /*! @brief Unsigned integer type. */
    using size_type = stl::size_t;

    /**
     * @brief Binds a cache to a given registry.
     * @param ref The registry to which the cache is bound.
     */
    explicit basic_query_cache(registry_type &ref) noexcept
        : reg{&ref} {}

    /*! @brief Default destructor. */
    ~basic_query_cache() = default;

    /*! @brief Default copy constructor, deleted on purpose. */
    basic_query_cache(const basic_query_cache &) = delete;

    /*! @brief Default move constructor. */
    basic_query_cache(basic_query_cache &&) = default;

    /**
     * @brief Default copy assignment operator, deleted on purpose.
     * @return This query cache.
     */
    basic_query_cache &operator=(const basic_query_cache &) = delete;

    /**
     * @brief Default move assignment operator.
     * @return This query cache.
     */
    basic_query_cache &operator=(basic_query_cache &&) = default;

    /**
     * @brief Returns the entities matching a component composition.
     *
     * The first query for a given composition builds the result by iterating
     * a view. Subsequent identical queries return the cached result, unless
     * the cache has been invalidated in the meantime.
     *
     * @tparam Get Types of components iterated by the query.
     * @tparam Exclude Types of components used to filter the query.
     * @return The entities matching the given component composition.
     */
    template<typename... Get, typename... Exclude>
    [[nodiscard]] const stl::vector<entity_type> &query(exclude_t<Exclude...> = exclude_t{}) {
        static_assert(sizeof...(Get) != 0u, "Query with no component type");
        return fetch<stl::remove_const_t<Get>...>(exclude_t<stl::remove_const_t<Exclude>...>{});
    }

    /**
     * @brief Forces a rebuild of the cached result for a component composition.
     * @tparam Get Types of components iterated by the query.
     * @tparam Exclude Types of components used to filter the query.
     * @return The entities matching the given component composition.
     */
    template<typename... Get, typename... Exclude>
    [[nodiscard]] const stl::vector<entity_type> &refresh(exclude_t<Exclude...> excludes = exclude_t{}) {
        static_assert(sizeof...(Get) != 0u, "Query with no component type");

        if(auto it = entries.find(key_for<stl::remove_const_t<Get>...>(exclude_t<stl::remove_const_t<Exclude>...>{})); it != entries.end()) {
            it->second->dirty = true;
        }

        return query<Get...>(excludes);
    }

    /*! @brief Drops all cached results and disconnects all listeners. */
    void clear() noexcept {
        entries.clear();
    }

    /**
     * @brief Returns the number of cached component compositions.
     * @return The number of cached component compositions.
     */
    [[nodiscard]] size_type size() const noexcept {
        return entries.size();
    }

    /**
     * @brief Checks whether the cache contains no composition.
     * @return True if the cache contains no composition, false otherwise.
     */
    [[nodiscard]] bool empty() const noexcept {
        return entries.empty();
    }

    /**
     * @brief Returns the number of queries served from the cache.
     * @return The number of queries served without rebuilding the result.
     */
    [[nodiscard]] size_type hits() const noexcept {
        return hit_counter;
    }

    /**
     * @brief Returns the number of queries that rebuilt the result.
     * @return The number of queries that rebuilt the cached result.
     */
    [[nodiscard]] size_type misses() const noexcept {
        return miss_counter;
    }

private:
    registry_type *reg;
    map_type entries{};
    size_type hit_counter{};
    size_type miss_counter{};
};

} // namespace entt

#endif
