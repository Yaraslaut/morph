// SPDX-License-Identifier: Apache-2.0

#include <async_framework/registry.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>

using namespace morph;

struct RxAction {
    int val = 0;
};
struct RxModel {
    int execute(const RxAction& act) { return act.val + 1; }
};

template <>
struct morph::ModelTraits<RxModel> {
    static constexpr std::string_view typeId() { return "REG_RxModel"; }
};
template <>
struct morph::ActionTraits<RxAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "REG_RxAction"; }
    static std::string toJson(const RxAction& act) {
        std::string out;
        if (auto errCode = glz::write_json(act, out)) {
            throw morph::ParseError{glz::format_error(errCode, out)};
        }
        return out;
    }
    static RxAction fromJson(std::string_view json) {
        RxAction action{};
        if (auto errCode = glz::read_json(action, json)) {
            throw morph::ParseError{glz::format_error(errCode, json)};
        }
        return action;
    }
    static std::string resultToJson(const int& res) {
        std::string out;
        if (auto errCode = glz::write_json(res, out)) {
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

// ── ParseError ────────────────────────────────────────────────────────────────

TEST_CASE("ParseError is a std::runtime_error with message", "[registry]") {
    ParseError err{"bad json"};
    REQUIRE(std::string{err.what()} == "bad json");
    REQUIRE(dynamic_cast<const std::runtime_error*>(&err) != nullptr);
}

// ── ActionTraits: JSON round-trip via real glaze ───────────────────────────────

TEST_CASE("ActionTraits: toJson/fromJson round-trips correctly", "[registry]") {
    RxAction action{42};
    auto json = ActionTraits<RxAction>::toJson(action);
    auto action2 = ActionTraits<RxAction>::fromJson(json);
    REQUIRE(action2.val == 42);
}

TEST_CASE("ActionTraits: resultToJson/resultFromJson round-trips correctly", "[registry]") {
    int res = 99;
    auto json = ActionTraits<RxAction>::resultToJson(res);
    auto result2 = ActionTraits<RxAction>::resultFromJson(json);
    REQUIRE(result2 == 99);
}

TEST_CASE("ActionTraits: fromJson with bad input throws ParseError", "[registry]") {
    // Non-numeric JSON for an int field
    REQUIRE_THROWS_AS(ActionTraits<RxAction>::fromJson("not-json"), morph::ParseError);
}

TEST_CASE("ActionTraits: resultFromJson with bad input throws ParseError", "[registry]") {
    REQUIRE_THROWS_AS(ActionTraits<RxAction>::resultFromJson("not-a-number"), morph::ParseError);
}

// ── ModelRegistryFactory: insert_or_assign overwrites ─────────────────────────

TEST_CASE("ModelRegistryFactory: re-registering same typeId overwrites factory", "[registry]") {
    ModelRegistryFactory registry;
    registry.registerModel<RxModel>("REG_RxModel");
    // Second registration should not throw and should still produce a valid holder
    registry.registerModel<RxModel>("REG_RxModel");
    auto holder = registry.create("REG_RxModel");
    REQUIRE(holder != nullptr);
    REQUIRE(holder->type() == std::type_index(typeid(RxModel)));
}

// ── ActionDispatcher: KeyHash collision avoidance (basic) ─────────────────────

TEST_CASE("ActionDispatcher: different (model,action) pairs are independent", "[registry]") {
    ActionDispatcher dispatcher;
    ModelRegistryFactory registry;
    registry.registerModel<RxModel>("REG_RxModel");
    dispatcher.registerAction<RxModel, RxAction>("REG_RxModel", "REG_RxAction");

    auto holder = registry.create("REG_RxModel");

    // Known action returns value+1
    auto result = dispatcher.dispatch("REG_RxModel", "REG_RxAction", *holder, R"({"val":9})");
    REQUIRE(result == "10");

    // A different action name on same model should throw
    REQUIRE_THROWS_AS(dispatcher.dispatch("REG_RxModel", "REG_Unknown", *holder, "{}"), std::runtime_error);
}
