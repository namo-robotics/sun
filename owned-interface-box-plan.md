# Store owned interface values in one allocator-backed box

Implementation handoff for [Sun PR #472](https://github.com/namo-robotics/sun/pull/472), based on branch `i470` at `07d99c6f`. Follow the repository's `AGENTS.md` when implementing.

## Design

Keep the safe public API `std.mem.create_interface<I>(allocator, value)`. The current implementation creates a `Unique` and has the compiler extract its data and allocator fields. Replace that path with one private `InterfaceBox<T>` allocation. The library owns the allocation layout and cleanup policy. The compiler constructs, borrows, dispatches, moves, and drops a four-pointer interface handle.

For `std.mem.create_interface<Value>(arena, Item(42))`, the handle has these fields in order:

| Field | Contents |
|---|---|
| `owner` | Address of the entire `InterfaceBox<Item>` in the arena |
| `object` | Address of the initialized `Item` field inside that box |
| `vtable` | Existing dispatch table for `Item` implementing `Value` |
| `deinit` | Library function that destroys and releases this box |

A method call passes `object` as its receiver. A drop calls `deinit(owner)` once. Moving transfers the four fields and clears the source's cleanup responsibility. An owning child-to-parent upcast replaces only the vtable. Borrowing extracts the existing two-pointer `{object, vtable}` view. Preserve direct concrete-to-interface borrowing, including conversion of an arena to `const ref IAllocator`.

## Implementation

1. In `stdlib/std/allocator.sun`, define a private `InterfaceBox<T>` with `allocator: const ref IAllocator` and `value: T`. Its constructor stores the reference and moves the value inline. In the factory, call `create<InterfaceBox<T>>(allocator, allocator, value)`. The first allocator selects the storage. The second initializes the box field. Reuse the construction-failure guard in `create`.
2. Implement a private, nonthrowing `drop_interface_box<T>` in Sun. It takes a `raw_ptr<InterfaceBox<T>>`, saves the allocator reference, destroys the box in place, then deallocates the original box pointer with the size and alignment of one box. Normal field destruction drops the concrete value.
3. Replace the one-argument intrinsic with unsafe typed adoption: `_create_interface<I>(owner, object, deinit)`. Its operands have types `raw_ptr<O>`, `raw_ptr<T>`, and `function(raw_ptr<O>) void`. Infer `O` and `T`, require an interface `I`, and validate that the concrete `T` supports dynamic dispatch through it. The factory passes the box pointer, the address of its initialized value field, and the specialized cleanup function. The unsafe caller guarantees stable valid pointers and sole cleanup responsibility. Code generation supplies the vtable and stores the operands. Remove the compiler's checks for `std.mem.Unique` and its allocator layout from this path.
4. Update owner layout and all consumers, including ABI lowering, calls and returns, borrowed-view extraction, upcasts, moves, and scope cleanup. Use shared accessors or named field indices. Audit code that currently treats the first two owner fields as a borrowed view. Existing implementation entry points include `include/types/interface_type.h` and `src/codegen/intrinsics/generic.cpp`.
5. Preserve dependencies on the allocator and references held by the concrete value at the typed factory call boundary. Moving an owner into a collection, returning it, or borrowing through it must retain the applicable lifetime restrictions even though the factory uses raw pointers for adoption.
6. Bump the `.moon` format version for the changed owner ABI. Update the interface, ownership, and intrinsic documentation, including the requirement to rebuild existing bundles. Remove direct adoption of an existing `Unique` from this PR. Keep ordinary `Unique` APIs and tests.

## Verification

- With a counting allocator and an observable item destructor, check one allocation, correct dispatch, one destruction, and one deallocation through the original allocator using the original address, size, and alignment.
- Exercise distinct owner and object addresses through borrowing, vector growth and removal, owning upcasts, and interface-valued returns. Verify exactly-once cleanup after moves, replacement, and discarded results.
- Retain rejection coverage for use after move, reset while an arena owner or view is live, escape from a local arena, and escape of a reference held inside the item. Verify that allocation failure destroys the consumed value once.
- Replace tests specific to one-argument `_create_interface<I>(unique)` with typed-adoption coverage. Include a non-box owner, a mismatched cleanup signature, a call outside an unsafe block, and a concrete type that cannot dispatch through the requested interface.
- From the workspace root, format changed Sun sources with `sun fmt`, build with `cmake --build build -j$(($(nproc)/2))`, and run `SUN_PATH=$PWD ctest --test-dir build -j$(($(nproc)/2))`.
