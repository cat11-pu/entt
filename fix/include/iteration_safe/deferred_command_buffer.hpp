#ifndef ITERATION_SAFE_DEFERRED_COMMAND_BUFFER_HPP
#define ITERATION_SAFE_DEFERRED_COMMAND_BUFFER_HPP

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <entt/entt.hpp>

namespace iteration_safe {

/**
 * @brief Deferred, iteration-safe mutation queue for an EnTT registry.
 *
 * Every request is only recorded while a traversal is in progress. The
 * underlying storages are never touched until @ref flush runs, after the
 * traversal has finished and every view/storage iterator has been destroyed.
 * Because the packed arrays do not change mid-loop, no iterator is ever
 * invalidated, no element is visited twice and no element is skipped.
 *
 * Requests are applied in FIFO order. A request whose precondition no longer
 * holds at flush time (for example, removing a component from an entity that
 * was destroyed by an earlier request) is skipped and reported instead of
 * dereferencing invalid storage state.
 */
template<typename Registry>
class basic_deferred_command_buffer {
    using entity_type = typename Registry::entity_type;

    enum class kind : unsigned char {
        destroy_entity,
        remove_component,
        generic
    };

    struct command {
        kind op;
        entity_type entt;
        // component key for remove_component, unused otherwise
        entt::id_type type;
        std::function<void(Registry &)> apply;
        // precondition evaluated right before apply; returning false skips it
        std::function<bool(const Registry &)> valid;
    };

public:
    /*! @brief Outcome of flushing the queue. */
    struct flush_result {
        /*! @brief Commands applied successfully. */
        std::size_t applied{};
        /*! @brief Commands skipped because their precondition failed. */
        std::size_t skipped{};

        /*! @brief True when every recorded command was applied. */
        [[nodiscard]] explicit operator bool() const noexcept {
            return skipped == 0u;
        }
    };

    /**
     * @brief Creates a command bound to the given registry.
     * @param source Registry mutated on flush.
     */
    explicit basic_deferred_command_buffer(Registry &source) noexcept
        : registry{&source} {}

    /*! @brief Number of recorded, not yet flushed commands. */
    [[nodiscard]] std::size_t size() const noexcept {
        return commands.size();
    }

    /*! @brief True when no command is pending. */
    [[nodiscard]] bool empty() const noexcept {
        return commands.empty();
    }

    /**
     * @brief Defers destruction of an entity.
     * @param entt Entity to destroy on flush.
     */
    void destroy(const entity_type entt) {
        commands.push_back(command{
            kind::destroy_entity,
            entt,
            {},
            [entt](Registry &reg) { reg.destroy(entt); },
            [entt](const Registry &reg) { return reg.valid(entt); }});
    }

    /**
     * @brief Defers removal of a component from an entity.
     * @tparam Type Component type to remove.
     * @param entt Owner of the component.
     */
    template<typename Type>
    void remove(const entity_type entt) {
        commands.push_back(command{
            kind::remove_component,
            entt,
            entt::type_id<Type>().hash(),
            [entt](Registry &reg) { reg.template remove<Type>(entt); },
            [entt](const Registry &reg) { return reg.valid(entt) && reg.template all_of<Type>(entt); }});
    }

    /**
     * @brief Defers an arbitrary registry mutation.
     * @tparam Func Callable invoked with the registry on flush.
     * @param func Mutation to run after the traversal completes.
     */
    template<typename Func>
    void defer(Func &&func) {
        commands.push_back(command{
            kind::generic,
            entt::null,
            {},
            std::function<void(Registry &)>{std::forward<Func>(func)},
            {}});
    }

    /**
     * @brief Defers an arbitrary mutation guarded by a precondition.
     * @tparam Func Mutation callable.
     * @tparam Guard Precondition callable.
     * @param func Mutation to run when the guard passes.
     * @param guard Evaluated against the registry right before applying.
     */
    template<typename Func, typename Guard>
    void defer(Func &&func, Guard &&guard) {
        commands.push_back(command{
            kind::generic,
            entt::null,
            {},
            std::function<void(Registry &)>{std::forward<Func>(func)},
            std::function<bool(const Registry &)>{std::forward<Guard>(guard)}});
    }

    /**
     * @brief Applies every pending command in order.
     *
     * Commands whose precondition fails are skipped, never applied to stale
     * state. The queue is emptied regardless of individual outcomes.
     *
     * @return Counters for applied and skipped commands.
     */
    flush_result flush() {
        flush_result result{};

        for(auto &cmd: commands) {
            if(cmd.valid && !cmd.valid(*registry)) {
                ++result.skipped;
                continue;
            }

            cmd.apply(*registry);
            ++result.applied;
        }

        commands.clear();
        return result;
    }

    /*! @brief Discards every pending command without applying it. */
    void clear() noexcept {
        commands.clear();
    }

private:
    Registry *registry;
    std::vector<command> commands;
};

/*! @brief Deduction guide for the default registry type. */
basic_deferred_command_buffer(entt::registry &) -> basic_deferred_command_buffer<entt::registry>;

} // namespace iteration_safe

#endif
