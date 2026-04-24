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

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <typename Model>
struct ModelTraits;
template <typename Action>
struct ActionTraits;

// Forward declarations so instance() methods inside the classes can reference them.
inline class ActionDispatcher& defaultDispatcher();
inline class ModelRegistryFactory& defaultRegistry();

class ActionDispatcher {
public:
    using Runner = std::function<std::string(IModelHolder&, std::string_view)>;

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

    std::string dispatch(std::string_view modelId, std::string_view actionId, IModelHolder& holder,
                         std::string_view payload) {
        Key key{std::string{modelId}, std::string{actionId}};
        auto iter = _runners.find(key);
        if (iter == _runners.end()) {
            throw std::runtime_error("unknown action: " + key.first + "/" + key.second);
        }
        return iter->second(holder, payload);
    }

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

class ModelRegistryFactory {
public:
    template <typename Model>
    void registerModel(std::string_view modelId) {
        _factories.insert_or_assign(std::string{modelId}, [] { return ModelFactory::create<Model>(); });
    }
    std::unique_ptr<IModelHolder> create(std::string_view modelId) {
        auto iter = _factories.find(std::string{modelId});
        if (iter == _factories.end()) {
            throw std::runtime_error("unknown model type: " + std::string{modelId});
        }
        return iter->second();
    }
    static ModelRegistryFactory& instance() { return defaultRegistry(); }

private:
    std::unordered_map<std::string, std::function<std::unique_ptr<IModelHolder>()>> _factories;
};

// Process-level default instances — what the BRIDGE_REGISTER_* macros target.
// Pass these to RemoteServer in production; pass fresh instances in tests.
inline ActionDispatcher& defaultDispatcher() {
    static ActionDispatcher inst;
    return inst;
}
inline ModelRegistryFactory& defaultRegistry() {
    static ModelRegistryFactory inst;
    return inst;
}

}  // namespace morph

// NOLINTBEGIN(bugprone-macro-parentheses)
#define BRIDGE_REGISTER_MODEL(M, NAME)                                  \
    template <>                                                         \
    struct morph::ModelTraits<M> {                                      \
        static constexpr std::string_view typeId() { return NAME; }     \
    };                                                                  \
    namespace {                                                         \
    [[maybe_unused]] const bool bridge_model_reg_##M = [] {             \
        morph::ModelRegistryFactory::instance().registerModel<M>(NAME); \
        return true;                                                    \
    }();                                                                \
    }

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
    [[maybe_unused]] const bool bridge_action_reg_##M##_##A = [] {                                       \
        morph::ActionDispatcher::instance().registerAction<M, A>(morph::ModelTraits<M>::typeId(), NAME); \
        return true;                                                                                     \
    }();                                                                                                 \
    }
// NOLINTEND(bugprone-macro-parentheses)
