#pragma once
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <typeinfo>

#include "strand.hpp"

namespace morph {

template <typename Model>
struct ModelHolder;

// ── Backend-change notification interface ─────────────────────────────────────
//
// Implemented by ModelHolder<M> when M declares void onBackendChanged().
// Bridge::switchBackend uses dynamic_cast to discover this capability without
// any coupling to the concrete model type M.
struct IBackendChangedSink {
    virtual ~IBackendChangedSink() = default;
    virtual void onBackendChanged() = 0;
};

// ── Concept ───────────────────────────────────────────────────────────────────

template <typename M>
concept BackendChangedNotifiable = requires(M& model) {
    { model.onBackendChanged() } -> std::same_as<void>;
};

// ── Conditional mixin ─────────────────────────────────────────────────────────
//
// Primary template: M does NOT declare onBackendChanged() — empty base, no overhead.
template <typename M, bool = BackendChangedNotifiable<M>>
struct BackendChangedMixin {};

// Specialisation: M DOES declare onBackendChanged() — inherit IBackendChangedSink
// and forward the virtual call to the concrete model instance.
template <typename M>
struct BackendChangedMixin<M, true> : IBackendChangedSink {
    void onBackendChanged() override { static_cast<ModelHolder<M>*>(this)->model.onBackendChanged(); }
};

// ── Core type-erasure ─────────────────────────────────────────────────────────

struct IModelHolder {
    virtual ~IModelHolder() = default;
    [[nodiscard]] virtual std::type_index type() const noexcept = 0;
    template <typename Model>
    Model& into() {
        if (type() != std::type_index(typeid(Model))) {
            throw std::bad_cast{};
        }
        return static_cast<ModelHolder<Model>*>(this)->model;
    }
};

template <typename Model>
struct ModelHolder : IModelHolder, BackendChangedMixin<Model> {
    Model model;
    template <typename... Args>
    explicit ModelHolder(Args&&... args) : model{std::forward<Args>(args)...} {}
    [[nodiscard]] std::type_index type() const noexcept override { return typeid(Model); }
};

class ModelFactory {
public:
    template <typename Model>
    static std::unique_ptr<IModelHolder> create() {
        return std::make_unique<ModelHolder<Model>>();
    }
};

}  // namespace morph
