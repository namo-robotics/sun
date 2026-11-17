/** Exercises explicit interface ownership and retained allocator lifetimes. */
#include <gtest/gtest.h>

#include "driver/execution_utils.h"

/** Keeps shared Sun fixture declarations local to these tests. */
namespace {
/** Defines concrete implementations with observable destruction. */
const std::string fixture = R"(
using std;
var drops: i32 = 0;
/** Provides a dynamically dispatched value. */
interface Value { /** Reads the value. */ public const method read() i32; }
/** Owns a value and records destruction. */
class Item implements Value {
  var value: i32;
  /** Initializes the stored value. */ init(value: i32) { this.value = value; }
  /** Records exactly one destruction. */ deinit() { drops += 1; }
  /** Reads the stored value. */ public const method read() i32 { return this.value; }
}
/** Supplies a second concrete implementation. */
class Other implements Value {
  /** Initializes an empty implementation. */ init() {}
  /** Records exactly one destruction. */ deinit() { drops += 1; }
  /** Returns another value. */ public const method read() i32 { return 23; }
}
)";
}  // namespace

/** Heap owners retain concrete dispatch through vector growth and removal. */
TEST(OwnedInterfaces, heterogeneous_vector) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Moves owners into a growing collection and extracts one. */
    function run() i32 throws {
      var values = Vec<Value>(std.mem.global_allocator, 0);
      values.push(std.mem.create_interface<Value>(std.mem.global_allocator, Item(19)));
      values.push(std.mem.create_interface<Value>(std.mem.global_allocator, Other()));
      var last = values.remove(1);
      return values.get(0).read() + last.read();
    }
    /** Checks the result and exactly-once destruction. */
    function main() i32 throws { var result = run(); return result + drops * 100; }
  )"),
            242);
}

/** Replacement drops the old owner and moves the replacement exactly once. */
TEST(OwnedInterfaces, replacement_and_return) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Returns an owner backed by the global allocator. */
    function make() Value throws {
      return std.mem.create_interface<Value>(std.mem.global_allocator, Item(42));
    }
    /** Replaces an existing owning handle. */
    function run() i32 throws {
      var value = std.mem.create_interface<Value>(std.mem.global_allocator, Other());
      value = make();
      return value.read();
    }
    /** Checks replacement and final destruction. */
    function main() i32 throws { var result = run(); return result + drops * 100; }
  )"),
            242);
}

/** Arena owners release concrete values before reset reuses their storage. */
TEST(OwnedInterfaces, arena_reset_after_scope) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Reuses a fixed arena after its owners leave scope. */
    function main() i32 throws {
      var arena = std.mem.ArenaAllocator(64);
      if (true) {
        var first = std.mem.create_interface<Value>(arena, Item(19));
        var second = std.mem.create_interface<Value>(arena, Other());
        if (first.read() + second.read() != 42) { return 0; }
      }
      arena.reset();
      if (true) { var again = std.mem.create_interface<Value>(arena, Item(42)); }
      return drops;
    }
  )"),
            3);
}

/** Failed arena allocation consumes and destroys the argument. */
TEST(OwnedInterfaces, arena_exhaustion_consumes_argument) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Checks ownership after a failed allocation. */
    function main() i32 throws {
      var arena = std.mem.ArenaAllocator(0);
      try { var value = std.mem.create_interface<Value>(arena, Item(42)); }
      catch (error: AllocationError) { return drops; }
      return 0;
    }
  )"),
            1);
}

/** An existing owner keeps the arena borrowed until its scope ends. */
TEST(OwnedInterfaces, reset_with_live_owner_rejected) {
  EXPECT_THROW(sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to invalidate a live owner's storage. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var value = std.mem.create_interface<Value>(arena, Item(42));
      arena.reset();
      value.read();
    }
  )"),
               sun::support::SunError);
}

/** Owning the concrete object does not permit escaping its local arena. */
TEST(OwnedInterfaces, local_arena_escape_rejected) {
  EXPECT_THROW(sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to return storage owned by a local arena. */
    function escape() Value throws {
      var arena = std.mem.ArenaAllocator(64);
      return std.mem.create_interface<Value>(arena, Item(42));
    }
  )"),
               sun::support::SunError);
}

/** Collection insertion must retain the arena dependency. */
TEST(OwnedInterfaces, collection_escape_rejected) {
  EXPECT_THROW(sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to store an arena owner beyond the arena's scope. */
    function main() void throws {
      var values = Vec<Value>(std.mem.global_allocator, 0);
      if (true) {
        var arena = std.mem.ArenaAllocator(64);
        values.push(std.mem.create_interface<Value>(arena, Item(42)));
      }
    }
  )"),
               sun::support::SunError);
}

/** References inside the erased concrete class retain their original lifetime.
 */
TEST(OwnedInterfaces, concrete_borrow_escape_rejected) {
  EXPECT_THROW(sun::driver::compileStringWithStdlib(fixture + R"(

    /** Retains a reference to external storage. */
    class Borrowed implements Value {
      var source: const ref i32;
      /** Borrows the supplied value. */ init(source: const ref i32) { this.source = source; }
      /** Reads the borrowed value. */ public const method read() i32 { return this.source; }
    }
    /** Attempts to hide a dangling reference in a heap owner. */
    function escape() Value throws {
      var local: i32 = 42;
      return std.mem.create_interface<Value>(std.mem.global_allocator, Borrowed(local));
    }
  )"),
               sun::support::SunError);
}

/** Explicit allocation remains the only concrete-to-owner conversion. */
TEST(OwnedInterfaces, implicit_construction_rejected) {
  EXPECT_THROW(sun::driver::compileStringWithStdlib(fixture + R"(
    /** Attempts an implicit allocation. */
    function main() void { var value: Value = Item(42); }
  )"),
               sun::support::SunError);
}

/** A mutable reference cannot be used to reset an arena with live owners. */
TEST(OwnedInterfaces, reset_through_alias_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to reset through an existing mutable alias. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var alias: ref std.mem.ArenaAllocator = arena;
      var value = std.mem.create_interface<Value>(alias, Item(42));
      alias.reset();
    }
  )"),
      "retains its storage");
}

/** Passing the arena to a mutating function must respect retained owners. */
TEST(OwnedInterfaces, reset_through_function_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Resets the supplied arena. */
    function reset(arena: ref std.mem.ArenaAllocator) void { arena.reset(); }
    /** Attempts to invalidate a live owner through a call. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var value = std.mem.create_interface<Value>(arena, Item(42));
      reset(arena);
    }
  )"),
      "retains its storage");
}

/** Explicit generic forwarding must preserve the arena dependency. */
TEST(OwnedInterfaces, generic_escape_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Transfers its argument to the caller. */
    function forward<T>(value: T) T { return value; }
    /** Attempts to erase the local arena dependency through a generic call. */
    function escape() Value throws {
      var arena = std.mem.ArenaAllocator(64);
      return forward<Value>(std.mem.create_interface<Value>(arena, Item(42)));
    }
  )"),
      "local storage");
}

/** Destruction order also matters when a collection and arena share a scope. */
TEST(OwnedInterfaces, same_scope_arena_dies_first_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Declares the arena after a collection that would retain its storage. */
    function main() void throws {
      var values = Vec<Value>(std.mem.global_allocator, 0);
      var arena = std.mem.ArenaAllocator(64);
      values.push(std.mem.create_interface<Value>(arena, Item(42)));
    }
  )"),
      "beyond the lifetime");
}

/** Moving an inner-scope owner into an outer collection extends its arena loan.
 */
TEST(OwnedInterfaces, collection_keeps_arena_borrowed) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts reset after transferring an owner out of its original scope. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var values = Vec<Value>(std.mem.global_allocator, 0);
      if (true) { values.push(std.mem.create_interface<Value>(arena, Item(42))); }
      arena.reset();
    }
  )"),
      "retains its storage");
}

/** A caller-owned arena may back a returned owner through generic forwarding.
 */
TEST(OwnedInterfaces, caller_arena_return) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Transfers ownership without allocating. */
    function forward<T>(value: T) T { return value; }
    /** Returns storage borrowed from the caller's arena. */
    function make(arena: const ref std.mem.ArenaAllocator) Value throws {
      return forward<Value>(std.mem.create_interface<Value>(arena, Item(42)));
    }
    /** Keeps the arena alive until the returned owner is destroyed. */
    function main() i32 throws {
      var arena = std.mem.ArenaAllocator(64);
      if (true) { var value = make(arena); if (value.read() != 42) { return 0; } }
      arena.reset();
      return drops;
    }
  )"),
            1);
}

/** Upcasts transfer both the concrete allocation and its cleanup function. */
TEST(OwnedInterfaces, owning_upcast_moves_cleanup) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Extends the base contract. */ interface Child extends Value {}
    /** Implements the child contract. */ class ChildItem implements Child {
      /** Initializes the object. */ init() {}
      /** Records concrete destruction. */ deinit() { drops += 1; }
      /** Reads the object. */ public const method read() i32 { return 42; }
    }
    /** Upcasts in a return position. */ function widen(value: Child) Value { return value; }
    /** Moves the upcast into another variable. */ function run() i32 throws {
      var child = std.mem.create_interface<Child>(std.mem.global_allocator, ChildItem());
      var parent = widen(child);
      var moved: Value = parent;
      return moved.read();
    }
    /** Checks that moving an upcast does not duplicate cleanup. */
    function main() i32 throws { var result = run(); return result + drops * 100; }
  )"),
            142);
}

/** Interface method results may themselves transfer an explicitly allocated
 * owner. */
TEST(OwnedInterfaces, dynamic_owner_return) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Produces owned values. */ interface Factory {
      /** Returns a new owner. */ public const method make() Value throws;
    }
    /** Implements a dynamically dispatched factory. */ class Maker implements Factory {
      /** Initializes the factory. */ init() {}
      /** Explicitly allocates its result. */ public const method make() Value throws {
        return std.mem.create_interface<Value>(std.mem.global_allocator, Item(42));
      }
    }
    /** Calls through the erased factory. */ function run() i32 throws {
      var factory = std.mem.create_interface<Factory>(std.mem.global_allocator, Maker());
      var value = factory.make();
      return value.read();
    }
    /** Observes cleanup of the returned owner. */
    function main() i32 throws { var result = run(); return result + drops * 100; }
  )"),
            142);
}

/** Application code cannot adopt raw storage using the internal intrinsic. */
TEST(OwnedInterfaces, internal_construction_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(
    /** Attempts to bypass the allocator API. */
    function adopt(pointer: raw_ptr<Item>) Value {
      return unsafe { _create_interface<Value, Item>(pointer, true); };
    }
  )"),
      "requires one Unique owner");
}

/** Discarded factory results still destroy their concrete objects. */
TEST(OwnedInterfaces, discarded_owner_cleanup) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Discards a result before leaving its scope. */
    function run() void throws { std.mem.create_interface<Value>(std.mem.global_allocator, Item(42)); }
    /** Checks the discarded owner's destruction. */
    function main() i32 throws { run(); return drops; }
  )"),
            1);
}

/** Lambda calls cannot reset storage retained by an owner. */
TEST(OwnedInterfaces, reset_through_lambda_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to reset through a callable value. */
    function main() void throws {
      var reset = (arena: ref std.mem.ArenaAllocator) => void { arena.reset(); };
      var arena = std.mem.ArenaAllocator(64);
      var alias: ref std.mem.ArenaAllocator = arena;
      var value = std.mem.create_interface<Value>(alias, Item(42));
      reset(alias);
    }
  )"),
      "retains its storage");
}

/** Explicit generic forwarding consumes its named owner argument. */
TEST(OwnedInterfaces, generic_use_after_move_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Transfers the supplied value to the caller. */
    function forward<T>(value: T) T { return value; }
    /** Attempts to use the old handle after transfer. */
    function main() i32 throws {
      var value = std.mem.create_interface<Value>(std.mem.global_allocator, Item(42));
      var moved = forward<Value>(value);
      return value.read();
    }
  )"),
      "moved");
}

/** A conditional result retains the storage of either possible owner. */
TEST(OwnedInterfaces, conditional_arena_escape_rejected) {
  EXPECT_THROW(sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to return an arena owner through a conditional expression. */
    function escape(choose: bool) Value throws {
      var arena = std.mem.ArenaAllocator(64);
      return choose ? std.mem.create_interface<Value>(arena, Item(42))
                    : std.mem.create_interface<Value>(std.mem.global_allocator, Other());
    }
  )"),
               sun::support::SunError);
}

/** Calls through lambda values transfer ownership of by-value arguments. */
TEST(OwnedInterfaces, lambda_use_after_move_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to use a handle after passing it to a lambda. */
    function main() i32 throws {
      var read = (value: Value) => i32 { return value.read(); };
      var value = std.mem.create_interface<Value>(std.mem.global_allocator, Item(42));
      read(value);
      return value.read();
    }
  )"),
      "moved");
}

/** An interface field drops its previous and final concrete values once. */
TEST(OwnedInterfaces, owning_field_replacement) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Owns an erased implementation as a field. */
    class Holder {
      public var value: Value;
      /** Takes ownership of the supplied implementation. */
      init(value: Value) { this.value = value; }
    }
    /** Replaces an owned field and reads the new implementation. */
    function run() i32 throws {
      var holder = Holder(std.mem.create_interface<Value>(std.mem.global_allocator, Other()));
      holder.value = std.mem.create_interface<Value>(std.mem.global_allocator, Item(42));
      return holder.value.read();
    }
    /** Checks destruction of both field values. */
    function main() i32 throws { var result = run(); return result + drops * 100; }
  )"),
            242);
}

/** Enum payload destruction releases its erased concrete owner. */
TEST(OwnedInterfaces, owning_enum_payload) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Stores an optional erased implementation. */
    enum Entry { Empty, Present(Value) }
    /** Leaves an owning enum payload to be destroyed at scope exit. */
    function run() void throws {
      var entry = Entry.Present(std.mem.create_interface<Value>(std.mem.global_allocator, Item(42)));
    }
    /** Checks destruction through the payload. */
    function main() i32 throws { run(); return drops; }
  )"),
            1);
}

/** Heap factory constraints reject a non-interface target before its body. */
TEST(OwnedInterfaces, heap_factory_requires_interface) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Supplies a concrete type as the interface target. */
    function main() void throws { var value = std.mem.create_interface<Item>(std.mem.global_allocator, Item(42)); }
  )"),
      "does not satisfy constraint 'interface'");
}

/** Arena factory constraints reject a non-interface target before its body. */
TEST(OwnedInterfaces, arena_factory_requires_interface) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Supplies a concrete type as the interface target. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var value = std.mem.create_interface<Item>(arena, Item(42));
    }
  )"),
      "does not satisfy constraint 'interface'");
}

/** Heap factory constraints reject a class without the requested interface. */
TEST(OwnedInterfaces, heap_factory_requires_implementation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Defines an interface not implemented by Item. */ interface Unrelated {}
    /** Supplies an unrelated concrete implementation. */
    function main() void throws { var value = std.mem.create_interface<Unrelated>(std.mem.global_allocator, Item(42)); }
  )"),
      "does not satisfy constraint 'I'");
}

/** Arena factory constraints reject a class without the requested interface. */
TEST(OwnedInterfaces, arena_factory_requires_implementation) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Defines an interface not implemented by Item. */ interface Unrelated {}
    /** Supplies an unrelated concrete implementation. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var value = std.mem.create_interface<Unrelated>(arena, Item(42));
    }
  )"),
      "does not satisfy constraint 'I'");
}

/** A custom allocator binding survives erasure and receives deallocation. */
TEST(OwnedInterfaces, custom_allocator_cleanup) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    var released: i32 = 0;
    /** Delegates storage to the heap while recording which allocator releases it. */
    class CountingAllocator implements std.mem.IAllocator {
      var identity: i32;
      /** Stores observable allocator state. */ init(identity: i32) { this.identity = identity; }
      /** Allocate aligned bytes through the system allocator. */
      public const method alloc_raw(size: i64, alignment: i64) raw_ptr<u8> throws {
        return std.mem.global_allocator.alloc_raw(size, alignment);
      }
      /** Release bytes through the original allocator instance. */
      public const method dealloc_raw(pointer: raw_ptr<u8>, size: i64, alignment: i64) void {
        released += this.identity;
        std.mem.global_allocator.dealloc_raw(pointer, size, alignment);
      }
    }
    /** Move a custom-allocator owner through interface erasure and a collection. */
    function run() i32 throws {
      var allocator = CountingAllocator(7);
      var owner = std.mem.Unique<Item, const ref std.mem.IAllocator>(Item(42), allocator);
      var values = Vec<Value>(std.mem.global_allocator, 0);
      values.push(_create_interface<Value>(owner));
      return values.get(0).read();
    }
    /** Verify concrete destruction and allocator-specific release. */
    function main() i32 throws { var result = run(); return result + drops * 100 + released * 1000; }
  )"),
            7142);
}

/** Owner conversion consumes the original Unique handle. */
TEST(OwnedInterfaces, unique_conversion_consumes_owner) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to use an owner after interface conversion. */
    function main() void throws {
      var owner = std.mem.create_unique<Item>(std.mem.global_allocator, 42);
      var erased = _create_interface<Value>(owner);
      owner.get();
    }
  )"),
      "moved");
}

/** Direct owner conversion retains the source allocator's lifetime. */
TEST(OwnedInterfaces, unique_conversion_preserves_arena_borrow) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to reset storage after its owner has been erased. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var owner = std.mem.create_unique<Item>(arena, 42);
      var erased = _create_interface<Value>(owner);
      arena.reset();
    }
  )"),
      "borrow");
}

/** Raw adoption must be explicitly unsafe even though destruction is automatic.
 */
TEST(OwnedInterfaces, unique_raw_adoption_requires_unsafe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to adopt raw storage in safe code. */
    function main() void throws {
      var pointer = std.mem.create<Item>(std.mem.global_allocator, 42);
      var owner = std.mem.Unique<Item, std.mem.HeapAllocator>(pointer, std.mem.HeapAllocator());
    }
  )"),
      "unsafe constructor");
}

/** Discarding a directly erased owner must still destroy its allocation. */
TEST(OwnedInterfaces, discarded_direct_conversion_cleanup) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Discards the interface handle after transferring ownership to it. */
    function run() void throws {
      var owner = std.mem.create_unique<Item>(std.mem.global_allocator, 42);
      _create_interface<Value>(owner);
    }
    /** Checks cleanup of the discarded handle. */
    function main() i32 throws { run(); return drops; }
  )"),
            1);
}

/** A directly constructed owner keeps its allocator borrowed before erasure. */
TEST(OwnedInterfaces, direct_unique_keeps_arena_borrowed) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to invalidate a direct generic constructor's allocation. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var owner = std.mem.Unique<Item, const ref std.mem.ArenaAllocator>(Item(42), arena);
      arena.reset();
    }
  )"),
      "borrow");
}

/** Interface erasure cannot hide the lifetime of a direct Unique allocator. */
TEST(OwnedInterfaces, direct_unique_arena_escape_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to return an erased owner of local arena storage. */
    function escape() Value throws {
      var arena = std.mem.ArenaAllocator(64);
      var owner = std.mem.Unique<Item, const ref std.mem.ArenaAllocator>(Item(42), arena);
      return _create_interface<Value>(owner);
    }
  )"),
      "local storage");
}

/** Moving an allocator would invalidate references retained by direct owners.
 */
TEST(OwnedInterfaces, direct_unique_prevents_allocator_move) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to move the allocator while an owner borrows it. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var owner = std.mem.Unique<Item, const ref std.mem.ArenaAllocator>(Item(42), arena);
      var moved = arena;
    }
  )"),
      "borrowed");
}

/** An unnamed allocator still bounds the lifetime of an erased owner. */
TEST(OwnedInterfaces, temporary_unique_allocator_escape_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to return an owner borrowing a temporary arena. */
    function escape() Value throws {
      var owner = std.mem.Unique<Item, const ref std.mem.ArenaAllocator>(Item(42), std.mem.ArenaAllocator(64));
      return _create_interface<Value>(owner);
    }
  )"),
      "local storage");
}

/** Reference fields cannot point into their own movable containing object. */
TEST(OwnedInterfaces, sibling_reference_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::compileStringWithStdlib(R"(
    /** Returns a borrow without changing its target. */
    function borrow(value: const ref i32) const ref i32 { return value; }
    /** Attempts to retain an address that changes when this object moves. */
    class SelfBorrow {
      var value: i32;
      var view: const ref i32;
      /** Binds a reference to a sibling field. */
      init() { this.value = 42; this.view = borrow(this.value); }
    }
    /** Instantiates the invalid object. */ function main() void { var value = SelfBorrow(); }
  )"),
                                "sibling reference");
}

/** A nested owner must not borrow its allocator from a sibling field. */
TEST(OwnedInterfaces, sibling_allocator_reference_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Attempts to put a borrow of a sibling allocator into an owned field. */
    class SelfBorrow {
      var allocator: std.mem.ArenaAllocator;
      var owner: std.mem.Unique<Item, const ref std.mem.ArenaAllocator>;
      /** Creates a nested owner that would dangle after a move. */
      init() throws {
        this.allocator = std.mem.ArenaAllocator(64);
        this.owner = std.mem.Unique<Item, const ref std.mem.ArenaAllocator>(Item(42), this.allocator);
      }
    }
    /** Instantiates the invalid object. */ function main() void throws { var value = SelfBorrow(); }
  )"),
      "same movable object");
}

/** A local reference alias does not make a sibling borrow safe to retain. */
TEST(OwnedInterfaces, sibling_reference_alias_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::compileStringWithStdlib(R"(
    /** Attempts to hide a sibling borrow behind a local reference. */
    class SelfBorrow {
      var value: i32;
      var view: const ref i32;
      /** Copies the local alias into a field. */
      init() {
        this.value = 42;
        var alias: const ref i32 = this.value;
        this.view = alias;
      }
    }
    /** Instantiates the invalid object. */ function main() void { var value = SelfBorrow(); }
  )"),
                                "sibling reference");
}

/** Copying a reference to external storage does not create a sibling borrow. */
TEST(OwnedInterfaces, external_reference_fields_move_safely) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    /** Holds two references to the same external value. */
    class Borrowed {
      var first: const ref i32;
      var second: const ref i32;
      /** Copies the external reference, without borrowing its field slot. */
      init(value: const ref i32) { this.first = value; this.second = this.first; }
      /** Reads through the copied reference. */ public const method read() i32 { return this.second; }
    }
    /** Moves the holder while keeping its external source alive. */
    function main() i32 { var value: i32 = 42; var first = Borrowed(value); var second = first; return second.read(); }
  )"),
            42);
}

/** Passing an arena owner through an executor method preserves cleanup. */
TEST(OwnedInterfaces, executor_retains_arena_owner) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(

    /** Retains nodes received through an ordinary method call. */
    class Executor {
      var nodes: Vec<Value>;
      /** Creates empty node storage. */
      init() throws { this.nodes = Vec<Value>(std.mem.global_allocator, 0); }
      /** Transfers a node into the executor. */
      public method add_node(node: Value) void throws { this.nodes.push(node); }
    }
    /** Destroys the executor before reclaiming the arena's storage. */
    function main() i32 throws {
      var arena = std.mem.ArenaAllocator(64);
      if (true) {
        var executor = Executor();
        executor.add_node(std.mem.create_interface<Value>(arena, Item(42)));
      }
      arena.reset();
      return drops;
    }
  )"),
            1);
}

/** A method cannot hide an arena owner's dependency from its caller. */
TEST(OwnedInterfaces, executor_prevents_arena_reset) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(

    /** Retains nodes received through an ordinary method call. */
    class Executor {
      var nodes: Vec<Value>;
      /** Creates empty node storage. */
      init() throws { this.nodes = Vec<Value>(std.mem.global_allocator, 0); }
      /** Transfers a node into the executor. */
      public method add_node(node: Value) void throws { this.nodes.push(node); }
    }
    /** Attempts to reclaim storage while an executor still owns a node. */
    function main() void throws {
      var arena = std.mem.ArenaAllocator(64);
      var executor = Executor();
      if (true) {
        executor.add_node(std.mem.create_interface<Value>(arena, Item(42)));
      }
      arena.reset();
    }
  )"),
      "retains its storage");
}

/** Reference-returning lambdas cannot hide a borrow of sibling storage. */
TEST(OwnedInterfaces, sibling_reference_through_lambda_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::compileStringWithStdlib(R"(
    /** Attempts to retain a sibling field through a lambda call. */
    class SelfBorrow {
      var value: i32;
      var view: const ref i32;
      /** Passes the sibling field through a reference-returning lambda. */
      init() {
        this.value = 42;
        var borrow = (value: const ref i32) => const ref i32 { return value; };
        this.view = borrow(this.value);
      }
    }
    /** Instantiates the invalid object. */
    function main() void { var value = SelfBorrow(); }
  )"),
                                "sibling reference");
}

/** One interface-typed factory works with both heap and arena storage. */
TEST(OwnedInterfaces, runtime_allocator_selection) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(fixture + R"(
    /** Returns an owner that borrows whichever allocator the caller supplies. */
    function make(allocator: const ref std.mem.IAllocator) Value throws {
      return std.mem.create_interface<Value>(allocator, Item(21));
    }
    /** Selects different allocators without changing the factory's type. */
    function run() i32 throws {
      var arena = std.mem.ArenaAllocator(128);
      var heap_node = make(std.mem.global_allocator);
      var arena_node = make(arena);
      return heap_node.read() + arena_node.read();
    }
    /** Checks that both concrete values were destroyed. */
    function main() i32 throws { var result = run(); return result + drops * 100; }
  )"),
            242);
}

/** A borrowed heap allocator must live until its returned owner is destroyed.
 */
TEST(OwnedInterfaces, local_heap_allocator_escape_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      sun::driver::compileStringWithStdlib(fixture + R"(
    /** Attempts to return an owner borrowing a local allocator. */
    function make() Value throws {
      var allocator = std.mem.HeapAllocator();
      return std.mem.create_interface<Value>(allocator, Item(42));
    }
    /** Forces the invalid factory to be checked. */
    function main() void throws { var node = make(); }
  )"),
      "local storage");
}

/** Constructor references remain borrows when forwarded through create_unique.
 */
TEST(OwnedInterfaces, unique_constructor_preserves_reference_arguments) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    /** Borrows an external integer. */
    class Borrowed {
      var value: const ref i32;
      /** Retains the caller's integer reference. */
      init(value: const ref i32) { this.value = value; }
      /** Reads the retained integer. */
      public const method read() i32 { return this.value; }
    }
    /** Keeps the referent alive through owner destruction. */
    function main() i32 throws {
      var value: i32 = 42;
      var owner = std.mem.create_unique<Borrowed>(std.mem.global_allocator, value);
      return owner.get().read();
    }
  )"),
            42);
}

/** Constructor borrows cannot be erased by a variadic allocation helper. */
TEST(OwnedInterfaces, unique_constructor_local_reference_escape_rejected) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::compileStringWithStdlib(R"(
    /** Borrows an external integer. */
    class Borrowed {
      var value: const ref i32;
      /** Retains the caller's integer reference. */
      init(value: const ref i32) { this.value = value; }
    }
    /** Attempts to return an owner borrowing a local integer. */
    function make() std.mem.Unique<Borrowed, const ref std.mem.IAllocator> throws {
      var value: i32 = 42;
      return std.mem.create_unique<Borrowed>(std.mem.global_allocator, value);
    }
    /** Forces the invalid factory to be checked. */
    function main() void throws { var owner = make(); }
  )"),
                                "local storage");
}
