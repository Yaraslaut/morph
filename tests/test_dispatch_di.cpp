#include <async_framework/registry.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>

using namespace morph;

// Local model + action — not registered via macros, no global state touched.
struct DiAction {
    int x = 0;
};
struct DiModel {
    int execute(DiAction action) { return action.x * 3; }
};

// Manual ActionTraits specialisation so ActionDispatcher::registerAction can use it.
template <>
struct morph::ActionTraits<DiAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "DiAction"; }
    static std::string toJson(const DiAction& action) {
        std::string out;
        if (auto errCode = glz::write_json(action, out)) {
            throw morph::ParseError{glz::format_error(errCode, out)};
        }
        return out;
    }
    static DiAction fromJson(std::string_view json) {
        DiAction action{};
        if (auto errCode = glz::read_json(action, json)) {
            throw morph::ParseError{glz::format_error(errCode, json)};
        }
        return action;
    }
    static std::string resultToJson(const int& result) {
        std::string out;
        if (auto errCode = glz::write_json(result, out)) {
            throw morph::ParseError{glz::format_error(errCode, out)};
        }
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        if (auto errCode = glz::read_json(result, json)) {
            throw morph::ParseError{glz::format_error(errCode, json)};
        }
        return result;
    }
};
template <>
struct morph::ModelTraits<DiModel> {
    static constexpr std::string_view typeId() { return "DiModel"; }
};

TEST_CASE("ActionDispatcher: routes known action", "[di]") {
    ActionDispatcher dispatcher;
    ModelRegistryFactory registry;

    registry.registerModel<DiModel>("DiModel");
    dispatcher.registerAction<DiModel, DiAction>("DiModel", "DiAction");

    auto holder = registry.create("DiModel");
    auto result = dispatcher.dispatch("DiModel", "DiAction", *holder, R"({"x":7})");
    REQUIRE(result == "21");
}

TEST_CASE("ActionDispatcher: unknown action throws", "[di]") {
    ActionDispatcher dispatcher;
    ModelRegistryFactory registry;
    registry.registerModel<DiModel>("DiModel");
    auto holder = registry.create("DiModel");

    REQUIRE_THROWS_AS(dispatcher.dispatch("DiModel", "NoSuchAction", *holder, "{}"), std::runtime_error);
}

TEST_CASE("ModelRegistryFactory: unknown model throws", "[di]") {
    ModelRegistryFactory registry;
    REQUIRE_THROWS_AS(registry.create("NoSuchModel"), std::runtime_error);
}

TEST_CASE("Two isolated dispatchers do not share state", "[di]") {
    ActionDispatcher dispatcher1;
    ActionDispatcher dispatcher2;
    ModelRegistryFactory registry;

    registry.registerModel<DiModel>("DiModel");
    dispatcher1.registerAction<DiModel, DiAction>("DiModel", "DiAction");

    auto holder = registry.create("DiModel");

    auto result = dispatcher1.dispatch("DiModel", "DiAction", *holder, R"({"x":4})");
    REQUIRE(result == "12");

    REQUIRE_THROWS_AS(dispatcher2.dispatch("DiModel", "DiAction", *holder, R"({"x":4})"), std::runtime_error);
}
