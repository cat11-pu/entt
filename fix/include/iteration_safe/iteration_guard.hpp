#ifndef ITERATION_SAFE_ITERATION_GUARD_HPP
#define ITERATION_SAFE_ITERATION_GUARD_HPP

#include <cstddef>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace iteration_safe {

/*! @brief Reasons an active traversal can be invalidated. */
enum class invalidation_reason : unsigned char {
    none,
    /*! @brief A swap_and_pop storage shrank while being traversed. */
    swap_pop_storage_shrunk,
    /*! @brief A swap_only storage free list moved while being traversed. */
    swap_only_free_list_moved,
    /*! @brief A traversed storage was removed from the registry. */
    storage_destroyed
};

/*! @brief Human readable description of an invalidation reason. */
[[nodiscard]] inline const char *to_string(const invalidation_reason reason) noexcept {
    switch(reason) {
    case invalidation_reason::none:
        return "no invalidation";
    case invalidation_reason::swap_pop_storage_shrunk:
        return "swap_and_pop storage shrank during traversal";
    case invalidation_reason::swap_only_free_list_moved:
        return "swap_only storage free list moved during traversal";
    case invalidation_reason::storage_destroyed:
        return "traversed storage was destroyed during traversal";
    }

    return "unknown invalidation";
}

/**
 * @brief Snapshot of the iterator-relevant state of one storage.
 *
 * A storage traversal stays valid only while the bounds its iterators rely on
 * do not move against the data. Tombstone (in_place) storages keep their
 * packed array stable on erase, so a size change alone cannot invalidate
 * their iterators and is intentionally not treated as a failure.
 */
struct storage_checkpoint {
    /*! @brief Type id used to look the storage up in the registry. */
    entt::id_type id{};
    /*! @brief Deletion policy captured before the traversal. */
    entt::deletion_policy policy{entt::deletion_policy::swap_and_pop};
    /*! @brief Packed array size captured before the traversal. */
    std::size_t size{};
    /*! @brief Free list captured before the traversal. */
    std::size_t free_list{};
};

/**
 * @brief Scoped guard that turns iterator invalidation into an explicit error.
 *
 * Take a checkpoint of every storage about to be traversed, run the loop, then
 * call @ref check. If a destructive mutation invalidated the iterators, the
 * guard reports the reason instead of letting the loop read out of bounds.
 * This is the fail-closed alternative to deferring mutations: the caller gets
 * a definite signal and can abort or retry rather than corrupting memory.
 */
class iteration_guard {
public:
    /*! @brief Result of validating a traversal. */
    struct check_result {
        /*! @brief Invalidation reason, or none when the traversal stayed valid. */
        invalidation_reason reason{invalidation_reason::none};

        /*! @brief True when no invalidation was detected. */
        [[nodiscard]] explicit operator bool() const noexcept {
            return reason == invalidation_reason::none;
        }
    };

    /**
     * @brief Snapshots every storage backing the component types.
     * @tparam Type Component types whose storages will be traversed.
     * @param registry Registry owning the storages.
     * @return A guard bound to the registry and the requested storages.
     */
    template<typename... Type>
    [[nodiscard]] static iteration_guard take(const entt::registry &registry) {
        iteration_guard guard{registry};
        (guard.snapshot<Type>(), ...);
        return guard;
    }

    /**
     * @brief Validates the traversal against the recorded checkpoints.
     *
     * @return A result whose reason explains the first invalidation found.
     * Storage destruction takes priority, then size/free-list movement.
     */
    [[nodiscard]] check_result check() const {
        for(const auto &point: checkpoints) {
            const auto *current = registry->storage(point.id);

            if(current == nullptr) {
                return {invalidation_reason::storage_destroyed};
            }

            if(current->policy() != point.policy) {
                // policy cannot change at runtime; a mismatch means a stale slot
                return {invalidation_reason::storage_destroyed};
            }

            switch(point.policy) {
            case entt::deletion_policy::in_place:
                // tombstones keep the packed array bounds stable; safe by design
                break;
            case entt::deletion_policy::swap_and_pop:
                if(current->size() != point.size) {
                    return {invalidation_reason::swap_pop_storage_shrunk};
                }
                break;
            case entt::deletion_policy::swap_only:
                if(current->free_list() != point.free_list) {
                    return {invalidation_reason::swap_only_free_list_moved};
                }
                break;
            }
        }

        return {invalidation_reason::none};
    }

    /*! @brief Number of storages being watched. */
    [[nodiscard]] std::size_t watched() const noexcept {
        return checkpoints.size();
    }

private:
    explicit iteration_guard(const entt::registry &source) noexcept
        : registry{&source} {}

    template<typename Type>
    void snapshot() {
        using bare_type = std::remove_cv_t<Type>;
        const auto *storage_ptr = registry->storage<bare_type>();

        // a storage that does not exist yet has nothing to invalidate; record a
        // tombstone checkpoint so its later creation is still observable
        if(storage_ptr == nullptr) {
            checkpoints.push_back(storage_checkpoint{
                entt::type_id<bare_type>().hash(),
                entt::deletion_policy::swap_and_pop,
                0u,
                0u});
            return;
        }

        checkpoints.push_back(storage_checkpoint{
            entt::type_id<bare_type>().hash(),
            storage_ptr->policy(),
            storage_ptr->size(),
            storage_ptr->free_list()});
    }

    const entt::registry *registry;
    std::vector<storage_checkpoint> checkpoints;
};

} // namespace iteration_safe

#endif
