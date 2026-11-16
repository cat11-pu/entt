#ifndef ENTT_ENTITY_COMMAND_BUFFER_HPP
#define ENTT_ENTITY_COMMAND_BUFFER_HPP

#include "../container/dense_map.hpp"
#include "../stl/cstddef.hpp"
#include "../stl/functional.hpp"
#include "../stl/memory.hpp"
#include "../stl/utility.hpp"
#include "../stl/vector.hpp"
#include "entity.hpp"
#include "fwd.hpp"

namespace entt {

/**
 * @brief Buffer for commands to execute later against a registry.
 *
 * Commands are indexed by entity while a global submission order is retained.
 * This means the buffer can expose per-entity groups without changing the
 * observable result with respect to immediate execution.<br/>
 * Commands submitted while a commit is in progress are retained and executed
 * on the next commit.
 *
 * @tparam Registry Basic registry type.
 * @tparam Allocator Type of allocator used to manage memory and elements.
 */
template<typename Registry, typename Allocator>
class basic_command_buffer final {
    using alloc_traits = stl::allocator_traits<Allocator>;
    using command_type = stl::function<void(Registry &)>;
    using command_allocator = typename alloc_traits::template rebind_alloc<command_type>;
    using command_container = stl::vector<command_type, command_allocator>;
    using index_allocator = typename alloc_traits::template rebind_alloc<stl::size_t>;
    using index_container = stl::vector<stl::size_t, index_allocator>;
    using group_allocator = typename alloc_traits::template rebind_alloc<stl::pair<const typename Registry::entity_type, index_container>>;
    using group_container = dense_map<typename Registry::entity_type, index_container, stl::hash<typename Registry::entity_type>, stl::equal_to<>, group_allocator>;

public:
    /*! @brief Registry type. */
    using registry_type = Registry;
    /*! @brief Allocator type. */
    using allocator_type = Allocator;
    /*! @brief Underlying entity identifier. */
    using entity_type = typename registry_type::entity_type;
    /*! @brief Unsigned integer type. */
    using size_type = stl::size_t;

    /**
     * @brief Constructs a command buffer for a given registry.
     * @param value A valid registry.
     * @param value_allocator The allocator to use.
     */
    explicit basic_command_buffer(registry_type &value, const allocator_type &value_allocator = allocator_type{})
        : reg{&value},
          allocator{value_allocator},
          commands{command_allocator{value_allocator}},
          groups{group_allocator{value_allocator}} {}

    /*! @brief Default copy constructor, deleted on purpose. */
    basic_command_buffer(const basic_command_buffer &) = delete;

    /*! @brief Default move constructor. */
    basic_command_buffer(basic_command_buffer &&) noexcept = default;

    /*! @brief Default destructor. */
    ~basic_command_buffer() = default;

    /*! @brief Default copy assignment operator, deleted on purpose. */
    basic_command_buffer &operator=(const basic_command_buffer &) = delete;

    /*! @brief Default move assignment operator. */
    basic_command_buffer &operator=(basic_command_buffer &&) noexcept = default;

    /**
     * @brief Returns the associated allocator.
     * @return The associated allocator.
     */
    [[nodiscard]] constexpr allocator_type get_allocator() const noexcept {
        return allocator;
    }

    /**
     * @brief Returns the entity used to group registry-wide commands.
     * @return The entity used to group registry-wide commands.
     */
    [[nodiscard]] static constexpr entity_type global_entity() noexcept {
        return static_cast<entity_type>(entt::null);
    }

    /**
     * @brief Submits a command to execute at the next commit.
     *
     * The command is grouped under the given entity. It is still executed in
     * the exact order in which it was submitted.
     *
     * @tparam Func Type of the function object to invoke.
     * @param entity The entity used to group the command.
     * @param func A valid function object with signature `void(Registry &)`.
     * @return The global sequence number assigned to the command.
     */
    template<typename Func>
    size_type submit(const entity_type entity, Func &&func) {
        const auto sequence = next_sequence;
        auto [it, inserted] = groups.try_emplace(entity, index_allocator{allocator});

        try {
            commands.emplace_back(stl::forward<Func>(func));

            try {
                it->second.push_back(commands.size() - 1u);
            } catch(...) {
                commands.pop_back();
                throw;
            }
        } catch(...) {
            if(inserted && it->second.empty()) {
                groups.erase(it);
            }

            throw;
        }

        ++next_sequence;
        return sequence;
    }

    /**
     * @brief Submits a registry-wide command to execute at the next commit.
     *
     * The command is grouped under `global_entity()`.
     *
     * @tparam Func Type of the function object to invoke.
     * @param func A valid function object with signature `void(Registry &)`.
     * @return The global sequence number assigned to the command.
     */
    template<typename Func>
    size_type submit(Func &&func) {
        return submit(global_entity(), stl::forward<Func>(func));
    }

    /**
     * @brief Executes all pending commands in submission order.
     *
     * The pending batch is detached before execution. Commands submitted by
     * the executing commands are therefore retained for the next commit.
     *
     * @warning
     * If a command throws, the exception is propagated and the commands that
     * have already been executed are not rolled back.
     *
     * @return The number of commands executed by this call.
     */
    size_type commit() {
        if(commands.empty()) {
            return {};
        }

        command_container pending{command_allocator{allocator}};
        group_container batch{group_allocator{allocator}};

        pending.swap(commands);
        batch.swap(groups);

        const auto length = pending.size();

        for(auto &&command: pending) {
            command(*reg);
        }

        pending.clear();
        batch.clear();

        if(commands.empty()) {
            commands.swap(pending);
        }

        if(groups.empty()) {
            groups.swap(batch);
        }

        return length;
    }

    /**
     * @brief Discards all pending commands without executing them.
     * @return The number of commands discarded.
     */
    size_type rollback() noexcept {
        const auto length = commands.size();
        commands.clear();
        groups.clear();
        return length;
    }

    /**
     * @brief Returns the number of pending commands.
     * @return The number of pending commands.
     */
    [[nodiscard]] size_type size() const noexcept {
        return commands.size();
    }

    /**
     * @brief Returns the number of pending commands for a given entity.
     * @param entity The entity used to group commands.
     * @return The number of pending commands for the given entity.
     */
    [[nodiscard]] size_type size(const entity_type entity) const {
        const auto it = groups.find(entity);
        return it != groups.cend() ? it->second.size() : size_type{};
    }

    /**
     * @brief Checks whether the buffer contains no pending commands.
     * @return True if the buffer contains no pending commands, false otherwise.
     */
    [[nodiscard]] bool empty() const noexcept {
        return commands.empty();
    }

    /**
     * @brief Returns the number of non-empty entity groups.
     * @return The number of non-empty entity groups.
     */
    [[nodiscard]] size_type group_count() const noexcept {
        return groups.size();
    }

    /**
     * @brief Returns the capacity of the underlying command storage.
     * @return The capacity of the underlying command storage.
     */
    [[nodiscard]] size_type capacity() const noexcept {
        return commands.capacity();
    }

    /**
     * @brief Returns the next global sequence number.
     * @return The next global sequence number.
     */
    [[nodiscard]] size_type sequence() const noexcept {
        return next_sequence;
    }

private:
    registry_type *reg;
    allocator_type allocator;
    command_container commands;
    group_container groups;
    size_type next_sequence{};
};

} // namespace entt

#endif
