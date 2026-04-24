// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "model.hpp"

namespace morph {

/// @brief Exception thrown when JSON serialisation or deserialisation fails.
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief Traits specialisation that maps a model type to its string type-id.
///
/// Users must specialise this (or use `BRIDGE_REGISTER_MODEL`) before the model
/// can be registered with a backend.
/// @tparam Model Concrete model type.
template <typename Model>
struct ModelTraits;

/// @brief Traits specialisation that maps an action type to its id, JSON codec,
///        and result type.
///
/// Users must specialise this (or use `BRIDGE_REGISTER_ACTION`).
/// @tparam Action Concrete action type.
template <typename Action>
struct ActionTraits;

// Forward declarations so instance() methods inside the classes can reference them.
inline class ActionDispatcher& defaultDispatcher();
inline class ModelRegistryFactory& defaultRegistry();

/// @brief Registry that maps (modelId, actionId) pairs to type-erased runner functions.
///
/// Used by `RemoteServer` to dispatch incoming JSON requests without knowing the
/// concrete model or action types at the call site.
class ActionDispatcher {
public:
    /// @brief Type-erased action runner: deserialises, executes, and serialises the result.
    using Runner = std::function<std::string(IModelHolder&, std::string_view)>;

    /// @brief Registers a runner for `(Model, Action)` under the given string ids.
    ///
    /// The runner deserialises the JSON payload using `ActionTraits<Action>::fromJson`,
    /// calls `model.execute(action)`, and returns the JSON-encoded result.
    ///
    /// @tparam Model  Concrete model type.
    /// @tparam Action Concrete action type.
    /// @param modelId  String type-id of the model (from `ModelTraits<Model>::typeId()`).
    /// @param actionId String type-id of the action (from `ActionTraits<Action>::typeId()`).
    template <typename Model, typename Action>
    void registerAction(std::string_view modelId, std::string_view actionId) {
        Key key{std::string{modelId}, std::string{actionId}};
        _runners[key] = [](IModelHolder& holder, std::string_view payloadJson) {
            auto action = ActionTraits<Action>::fromJson(payloadJson);
            auto& model = holder.template into<Model>();
            auto result = model.execute(action);
            return ActionTraits<Action>::resultToJson(result);
        };
    }

    /// @brief Dispatches an action against @p holder and returns the JSON-encoded result.
    ///
    /// @param modelId  String id identifying the model type.
    /// @param actionId String id identifying the action type.
    /// @param holder   Model instance to execute against.
    /// @param payload  JSON-encoded action payload.
    /// @return JSON-encoded result string.
    /// @throws std::runtime_error if no runner is registered for `(modelId, actionId)`.
    std::string dispatch(std::string_view modelId, std::string_view actionId, IModelHolder& holder,
                         std::string_view payload) {
        Key key{std::string{modelId}, std::string{actionId}};
        auto iter = _runners.find(key);
        if (iter == _runners.end()) {
            throw std::runtime_error("unknown action: " + key.first + "/" + key.second);
        }
        return iter->second(holder, payload);
    }

    /// @brief Returns the process-level singleton dispatcher.
    static ActionDispatcher& instance() { return defaultDispatcher(); }

private:
    using Key = std::pair<std::string, std::string>;
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            // boost::hash_combine formula — avoids collisions from XOR on similar strings
            std::size_t seed = std::hash<std::string>{}(key.first);
            seed ^= std::hash<std::string>{}(key.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
    std::unordered_map<Key, Runner, KeyHash> _runners;
};

/// @brief Registry that creates `IModelHolder` instances by string type-id.
///
/// Used by `RemoteServer` to instantiate models on demand from incoming
/// `"register"` messages.
class ModelRegistryFactory {
public:
    /// @brief Registers a default-construction factory for `Model` under @p modelId.
    ///
    /// Overwrites any previously registered factory for the same id.
    /// @tparam Model Concrete model type to register.
    /// @param modelId String id used to look up this factory.
    template <typename Model>
    void registerModel(std::string_view modelId) {
        _factories.insert_or_assign(std::string{modelId}, [] { return ModelFactory::create<Model>(); });
    }

    /// @brief Creates a new model holder for the type registered under @p modelId.
    ///
    /// @param modelId String id of the model type.
    /// @return Owning pointer to the new holder.
    /// @throws std::runtime_error if @p modelId has not been registered.
    std::unique_ptr<IModelHolder> create(std::string_view modelId) {
        auto iter = _factories.find(std::string{modelId});
        if (iter == _factories.end()) {
            throw std::runtime_error("unknown model type: " + std::string{modelId});
        }
        return iter->second();
    }

    /// @brief Returns the process-level singleton registry.
    static ModelRegistryFactory& instance() { return defaultRegistry(); }

private:
    std::unordered_map<std::string, std::function<std::unique_ptr<IModelHolder>()>> _factories;
};

/// @brief Returns the process-level `ActionDispatcher` singleton.
///
/// Pass a local instance to `RemoteServer` in tests to avoid global state pollution.
inline ActionDispatcher& defaultDispatcher() {
    static ActionDispatcher inst;
    return inst;
}

namespace detail {

/// @brief Static-init helper for `BRIDGE_REGISTER_MODEL`. Marked noexcept so the check
/// `bugprone-throwing-static-initialization` is satisfied; a throw here aborts via
/// `std::terminate`, which matches the existing behaviour for static-init failures.
template <typename Model>
inline bool registerModelOnce(std::string_view modelId) noexcept {
    ModelRegistryFactory::instance().registerModel<Model>(modelId);
    return true;
}

/// @brief Static-init helper for `BRIDGE_REGISTER_ACTION`. See `registerModelOnce`.
template <typename Model, typename Action>
inline bool registerActionOnce(std::string_view modelId, std::string_view actionId) noexcept {
    ActionDispatcher::instance().registerAction<Model, Action>(modelId, actionId);
    return true;
}

}  // namespace detail

/// @brief Returns the process-level `ModelRegistryFactory` singleton.
///
/// Pass a local instance to `RemoteServer` in tests to avoid global state pollution.
inline ModelRegistryFactory& defaultRegistry() {
    static ModelRegistryFactory inst;
    return inst;
}

}  // namespace morph

// NOLINTBEGIN(bugprone-macro-parentheses)

/// @brief Registers model type @p M with the string id @p NAME.
///
/// Specialises `morph::ModelTraits<M>` and registers a factory with the
/// process-level `ModelRegistryFactory` at static-init time.
///
/// @param M    Concrete model type.
/// @param NAME String literal used as the type-id.
#define BRIDGE_REGISTER_MODEL(M, NAME)                                       \
    template <>                                                              \
    struct morph::ModelTraits<M> {                                           \
        static constexpr std::string_view typeId() noexcept { return NAME; } \
    };                                                                       \
    namespace {                                                         \
    [[maybe_unused]] const bool bridge_model_reg_##M =                  \
        morph::detail::registerModelOnce<M>(NAME);                      \
    }

/// @brief Registers action type @p A (for model @p M) with the string id @p NAME.
///
/// Specialises `morph::ActionTraits<A>` with JSON codec functions and registers
/// the action with the process-level `ActionDispatcher` at static-init time.
/// `BRIDGE_REGISTER_MODEL(M, ...)` must be called before this macro.
///
/// @param M    Concrete model type that handles the action.
/// @param A    Concrete action type.
/// @param NAME String literal used as the action type-id.
#define BRIDGE_REGISTER_ACTION(M, A, NAME)                                                               \
    template <>                                                                                          \
    struct morph::ActionTraits<A> {                                                                      \
        using Result = decltype(std::declval<M&>().execute(std::declval<A>()));                          \
        static constexpr std::string_view typeId() { return NAME; }                                      \
        static std::string toJson(const A& action) {                                                     \
            std::string out;                                                                             \
            if (auto errCode = glz::write_json(action, out)) {                                           \
                throw morph::ParseError{glz::format_error(errCode, out)};                                \
            }                                                                                            \
            return out;                                                                                  \
        }                                                                                                \
        static A fromJson(std::string_view jsonStr) {                                                    \
            A action{};                                                                                  \
            if (auto errCode = glz::read_json(action, jsonStr)) {                                        \
                throw morph::ParseError{glz::format_error(errCode, jsonStr)};                            \
            }                                                                                            \
            return action;                                                                               \
        }                                                                                                \
        static std::string resultToJson(const Result& result) {                                          \
            std::string out;                                                                             \
            if (auto errCode = glz::write_json(result, out)) {                                           \
                throw morph::ParseError{glz::format_error(errCode, out)};                                \
            }                                                                                            \
            return out;                                                                                  \
        }                                                                                                \
        static Result resultFromJson(std::string_view jsonStr) {                                         \
            Result result{};                                                                             \
            if (auto errCode = glz::read_json(result, jsonStr)) {                                        \
                throw morph::ParseError{glz::format_error(errCode, jsonStr)};                            \
            }                                                                                            \
            return result;                                                                               \
        }                                                                                                \
    };                                                                                                   \
    namespace {                                                                                          \
    [[maybe_unused]] const bool bridge_action_reg_##M##_##A =                                            \
        morph::detail::registerActionOnce<M, A>(morph::ModelTraits<M>::typeId(), NAME);                  \
    }
// NOLINTEND(bugprone-macro-parentheses)
