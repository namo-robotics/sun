# C++ test cleanup record

## Review

Reviewed the C++ test inventory by suite, repeated test bodies, execution-helper
usage, declaration-only programs, and dense multi-scenario cases. Candidate
removals were checked against the source and assertions; text similarity alone
was not used as proof. This is a conservative cleanup, not a claim that every
remaining test is irreducible.

The review retains:

- Different numeric widths, signedness, boundary values, and runtime intrinsic
  cases; these can exercise different lowering paths.
- Borrow, lifetime, destruction, error-propagation, and ownership cases through
  their existing compilation or execution stages. Analysis alone does not run
  the borrow checker.
- Direct/imported, generic/non-generic, and JIT/native variants in module,
  serialization, ABI, and FFI tests. The expensive byte-literal regression keeps
  all its original compilation paths, with readable checks and failure traces.
- Parser comment cases whose otherwise similar programs contain different
  comment syntax, plus frontend, tooling, and pure type-rule assertions that
  inspect distinct results.
- File, process, cache, and command-line integration fixtures, including their
  process-isolation and sharding rules.

## Removed duplicates

All cases below used the same execution helper and equivalent source and
assertions as the survivor; none used a separate fixture or compilation mode.

| Removed test | Surviving test | Coverage retained |
| --- | --- | --- |
| `Operators_Bitwise.and_i32_decimal` | `Operators_Bitwise.and_i32` | Identical decimal operands, execution helper, and result. |
| `Operators_Bitwise.or_i32_decimal` | `Operators_Bitwise.or_i32` | Identical decimal operands, execution helper, and result. |
| `Operators_Bitwise.xor_i32_decimal` | `Operators_Bitwise.xor_i32` | Identical decimal operands, execution helper, and result. |
| `EndToEnd_Programs.mutual_recursion` | `Ffi.declare_forward_decl_keeps_sun_symbol` | Same forward declaration, mutually recursive functions, JIT execution, and result. |
| `EndToEnd_Programs.ref_basic` | `MemorySafety_Refs.basic_reference_read` | Same reference read and returned value through executeString. |
| `EndToEnd_Programs.ref_modify_original` | `MemorySafety_Refs.reference_modify_original` | Same write to the original variable and read through its reference. |
| `EndToEnd_Programs.ref_modify_through_ref` | `MemorySafety_Refs.reference_modify_through_ref` | Same write through a reference and read of the original variable. |
| `EndToEnd_Programs.ref_arithmetic` | `MemorySafety_Refs.reference_in_arithmetic` | Same reference arithmetic expression and execution result. |
| `EndToEnd_Programs.ref_to_global` | `MemorySafety_Refs.reference_to_global` | Same global reference mutation and execution result. |
| `MemorySafety_BorrowChecker.function_returns_value_not_ref` | `MemorySafety_Refs.basic_reference_read` | Same reference returned by value from main; the comment is the only source difference. |
| `MemorySafety_BorrowChecker.refs_to_different_vars` | `MemorySafety_Refs.refs_to_different_vars` | Same two independent references, arithmetic assignment, and result. |
| `MemorySafety_Refs.i32_ref_arg` | `MemorySafety_Refs.pass_i32_by_ref` | Identical parameter mutation program and result in the same file. |
| `Builtins_StaticPtr.length_of_a_method_result` | `Errors.without_stdlib_message_stays_literal_only` | Same class implementation and method-result length without standard-library imports. |
| `Lambdas_LifetimeSyntax.class_lifetime_and_this_application_accepted` | `Lambdas_RefLambdaTypes.object_subscribes_its_own_method_via_ref_param` | Same Bus lifetime, bound method subscription, two calls, and accumulated result. |

## Focused cases and cheaper stages

These declaration-only tests retain their names and declaration shapes, but use
analysis instead of compiling and running an unrelated `main` returning zero:

- `Classes.class_with_method`
- `Classes.simple_class_definition`
- `Classes_Generic.generic_class_definition`
- `Classes_Generic.generic_class_with_constructor`
- `Classes_Generic.generic_class_with_method`
- `Interfaces.interface_with_default_implementation`
- `Interfaces.interface_with_field`
- `Interfaces.simple_interface_definition`
- `Interfaces_Generic.generic_interface_with_method`
- `Interfaces_Generic.simple_generic_interface_definition`

The three checks formerly grouped under
`Classes_FieldInitializers.invalid_types_and_missing_annotations_are_rejected`
are now `incompatible_default_type_is_rejected`,
`missing_type_annotation_is_rejected`, and `interface_field_default_is_rejected`
in the same suite. Each retains its invalid declaration and runs analysis.

`Classes.duplicate_method_signatures_are_rejected_before_emission` retains the
ordinary-class case. The generic and interface cases are independently named
`duplicate_generic_method_signatures_are_rejected` and
`duplicate_interface_method_signatures_are_rejected` in the same suite. The
generic case still instantiates and calls the class. Analysis now directly
proves the rejection occurs before code generation.

Shared execution helpers retain their signatures and behavior, with bodies
compiled once in `tests/support/execution_utils.cpp`. Tests inspecting driver
internals include the driver header explicitly. Analysis-helper tests cover
valid declarations, parser and semantic diagnostics, session isolation, and
optional standard-library loading.

## Validation and measurements

Validation used the existing Debug/Ninja configuration on macOS arm64, five
parallel jobs, and individual CTest discovery (`SUN_TEST_SHARDS=1`). Compiler
cache use was disabled for build comparisons; existing precompiled headers and
compiler/dependency artifacts were retained for test-object rebuilds.

- The baseline discovered 3,415 CTest entries. Its four failures involved socket
  binding or debugger launch under sandbox restrictions; all four passed when
  rerun outside the sandbox.
- The updated full run passed with zero failures across 3,408 enabled entries
  (3,410 discovered entries, including two previously disabled benchmarks).
  Fifteen platform-dependent cases reported skips. Socket, debugger,
  standard-library, and TLS checks passed outside the sandbox.
- All 35 focused cases passed, covering the analysis helper, converted
  declarations, split diagnostics, surviving duplicates, and byte-literal
  integration regression. A direct GoogleTest filtered rerun also passed.
- Every retained CTest entry has the same command and properties as before.
  No sharding default or isolation exclusion changed.

For an unchanged translation unit, `tests/operators/test_logical.cpp`, three
compiles with each helper-header version gave median times of **4.88 seconds
before** and **0.99 seconds after**, about **80% less compilation time**. Both
variants used the existing compile command and precompiled header, without
ccache; the original header was supplied through a temporary include directory.
This measures one representative source file, not the entire test target.

Whole-target rebuild and whole-suite runtime comparisons were inconclusive.
The initial test-object rebuild took 147.84 seconds, but subsequent attempts had
long unexplained wall-clock stalls, including an attempt with idle sleep
inhibited. The baseline suite likewise contained simultaneous long stalls in
otherwise short tests. The updated full suite completed in 676.62 seconds, but
that cannot fairly be compared with the stalled baseline. No whole-build or
whole-suite speedup is claimed from those measurements.

Local inventories, discovery snapshots, and measurement logs are under
`tmp/test-cleanup/`. The cleanup reduces repeated compilation work and removes
confirmed duplicate executions without setting a test-count reduction quota.

## Legacy generic-parameter metadata

Removed the newly added `LegacyGenericParameters` and
`ModernGenericParametersOverrideLegacy` tests because old generic-parameter
metadata is no longer supported. `GenericInterfaceConstraintRoundtrip` retains
coverage of current parameters and constraints for all four declaration kinds.
