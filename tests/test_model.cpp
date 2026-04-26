// SPDX-License-Identifier: Apache-2.0

#include <async_framework/model.hpp>
#include <cassert>
#include <catch2/catch_test_macros.hpp>

using namespace morph;

struct Foo {
    int x = 0;
};
struct Bar {
    double y = 0.0;
};

TEST_CASE("ModelHolder stores the model and returns correct type index", "[model]") {
    auto holder = std::make_unique<ModelHolder<Foo>>();
    REQUIRE(holder->type() == std::type_index(typeid(Foo)));
}

TEST_CASE("IModelHolder::into returns reference to the contained model", "[model]") {
    auto holder = std::make_unique<ModelHolder<Foo>>();
    holder->model.x = 42;
    Foo& ref = holder->into<Foo>();
    REQUIRE(ref.x == 42);
    ref.x = 99;
    REQUIRE(holder->model.x == 99);
}

TEST_CASE("ModelFactory::create returns correctly typed holder", "[model]") {
    auto holder = ModelFactory::create<Bar>();
    REQUIRE(holder->type() == std::type_index(typeid(Bar)));
    Bar& bar = holder->into<Bar>();
    bar.y = 3.14;
    REQUIRE(holder->into<Bar>().y == bar.y);
}

TEST_CASE("ModelHolder constructors with arguments forward correctly", "[model]") {
    struct Constructed {
        int a;
        double b;
        explicit Constructed(int intVal, double dblVal) : a{intVal}, b{dblVal} {}
    };
    auto holder = std::make_unique<ModelHolder<Constructed>>(7, 2.5);
    REQUIRE(holder->model.a == 7);
    REQUIRE(holder->model.b == 2.5);
}

TEST_CASE("ModelId equality and hash", "[model]") {
    ModelId idA{5};
    ModelId idB{5};
    ModelId idC{6};
    REQUIRE(idA == idB);
    REQUIRE_FALSE(idA == idC);

    ModelIdHash hasher;
    REQUIRE(hasher(idA) == hasher(idB));
    // Different values should (almost always) hash differently
    REQUIRE(hasher(idA) != hasher(idC));
}
