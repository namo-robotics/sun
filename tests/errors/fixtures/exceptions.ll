; Exercises Sun-owned exception storage independently of source-language lowering.

@root_name = private constant [13 x i8] c"fixture.Root\00"
@a_name = private constant [10 x i8] c"fixture.A\00"
@b_name = private constant [10 x i8] c"fixture.B\00"
@c_name = private constant [10 x i8] c"fixture.C\00"
@root_type = private constant { ptr, ptr, i32, i32 } { ptr @root_name, ptr null, i32 4, i32 4 }
@a_type = private constant { ptr, ptr, i32, i32 } { ptr @a_name, ptr @root_type, i32 4, i32 4 }
@b_type = private constant { ptr, ptr, i32, i32 } { ptr @b_name, ptr @root_type, i32 4, i32 4 }
@c_type = private constant { ptr, ptr, i32, i32 } { ptr @c_name, ptr @root_type, i32 4, i32 4 }

declare void @sun_eh_raise(ptr) noreturn
declare void @sun_eh_take(ptr, ptr) nounwind
declare ptr @sun_eh_push(ptr) nounwind
declare void @sun_eh_pop(ptr) nounwind
declare void @sun_eh_terminate() noreturn nounwind
declare i32 @sun_eh_personality(i32, i32, i64, ptr, ptr)

; Constructs a bounded exception value and transfers it to the runtime.
define internal void @fixture_throw(ptr %type, i32 %code) noinline uwtable {
entry:
  %value = alloca [144 x i8], align 16
  store ptr %type, ptr %value
  %payload = getelementptr i8, ptr %value, i64 16
  store i32 %code, ptr %payload
  call void @sun_eh_raise(ptr %value)
  unreachable
}

; Handles C without disturbing either outer propagation record.
define internal i32 @fixture_handle_c() noinline uwtable personality ptr @sun_eh_personality {
entry:
  %caught = alloca [144 x i8], align 16
  invoke void @fixture_throw(ptr @c_type, i32 30) to label %unexpected unwind label %handler
unexpected:
  unreachable
handler:
  %lp = landingpad {ptr, i32} catch ptr @c_type
  %exception = extractvalue {ptr, i32} %lp, 0
  call void @sun_eh_take(ptr %exception, ptr %caught)
  %payload = getelementptr i8, ptr %caught, i64 16
  %code = load i32, ptr %payload
  ret i32 %code
}

; Keeps B's unwind object stable while a nested cleanup handles C.
define internal void @fixture_cleanup_b(ptr %sum) noinline uwtable personality ptr @sun_eh_personality {
entry:
  %nested = alloca [176 x i8], align 16
  invoke void @fixture_throw(ptr @b_type, i32 20) to label %unexpected unwind label %cleanup
unexpected:
  unreachable
cleanup:
  %original = landingpad {ptr, i32} cleanup
  %previous = call ptr @sun_eh_push(ptr %nested)
  %code = invoke i32 @fixture_handle_c() to label %complete unwind label %terminate
complete:
  store i32 %code, ptr %sum
  call void @sun_eh_pop(ptr %previous)
  resume {ptr, i32} %original
terminate:
  %escaped = landingpad {ptr, i32} catch ptr null
  call void @sun_eh_terminate()
  unreachable
}

; Handles B after its own cleanup has run and includes C's result.
define internal i32 @fixture_handle_b() noinline uwtable personality ptr @sun_eh_personality {
entry:
  %caught = alloca [144 x i8], align 16
  %sum = alloca i32
  store i32 0, ptr %sum
  invoke void @fixture_cleanup_b(ptr %sum) to label %unexpected unwind label %handler
unexpected:
  unreachable
handler:
  %lp = landingpad {ptr, i32} catch ptr @b_type
  %exception = extractvalue {ptr, i32} %lp, 0
  call void @sun_eh_take(ptr %exception, ptr %caught)
  %payload = getelementptr i8, ptr %caught, i64 16
  %code = load i32, ptr %payload
  %nested = load i32, ptr %sum
  %total = add i32 %code, %nested
  ret i32 %total
}

; Keeps A's unwind object stable throughout two nested cleanup levels.
define internal void @fixture_cleanup_a(ptr %sum) noinline uwtable personality ptr @sun_eh_personality {
entry:
  %nested = alloca [176 x i8], align 16
  invoke void @fixture_throw(ptr @a_type, i32 10) to label %unexpected unwind label %cleanup
unexpected:
  unreachable
cleanup:
  %original = landingpad {ptr, i32} cleanup
  %previous = call ptr @sun_eh_push(ptr %nested)
  %code = invoke i32 @fixture_handle_b() to label %complete unwind label %terminate
complete:
  store i32 %code, ptr %sum
  call void @sun_eh_pop(ptr %previous)
  resume {ptr, i32} %original
terminate:
  %escaped = landingpad {ptr, i32} catch ptr null
  call void @sun_eh_terminate()
  unreachable
}

; Returns all three preserved payloads, catching A through its parent family.
define i32 @sun_fixture_nested() uwtable personality ptr @sun_eh_personality {
entry:
  %caught = alloca [144 x i8], align 16
  %sum = alloca i32
  store i32 0, ptr %sum
  invoke void @fixture_cleanup_a(ptr %sum) to label %unexpected unwind label %handler
unexpected:
  unreachable
handler:
  %lp = landingpad {ptr, i32} catch ptr @root_type
  %exception = extractvalue {ptr, i32} %lp, 0
  call void @sun_eh_take(ptr %exception, ptr %caught)
  %payload = getelementptr i8, ptr %caught, i64 16
  %code = load i32, ptr %payload
  %nested = load i32, ptr %sum
  %total = add i32 %code, %nested
  ret i32 %total
}

; Rethrows a parent-caught A after independently handling C in the same scope.
define internal void @fixture_rethrow() noinline uwtable personality ptr @sun_eh_personality {
entry:
  %caught = alloca [144 x i8], align 16
  invoke void @fixture_throw(ptr @a_type, i32 10) to label %unexpected unwind label %handler
unexpected:
  unreachable
handler:
  %lp = landingpad {ptr, i32} catch ptr @root_type
  %exception = extractvalue {ptr, i32} %lp, 0
  call void @sun_eh_take(ptr %exception, ptr %caught)
  %ignored = call i32 @fixture_handle_c()
  call void @sun_eh_raise(ptr %caught)
  unreachable
}

; Proves that rethrowing preserves A's concrete identity after handling C.
define i32 @sun_fixture_rethrow() uwtable personality ptr @sun_eh_personality {
entry:
  %caught = alloca [144 x i8], align 16
  invoke void @fixture_rethrow() to label %unexpected unwind label %handler
unexpected:
  unreachable
handler:
  %lp = landingpad {ptr, i32} catch ptr @a_type
  %exception = extractvalue {ptr, i32} %lp, 0
  call void @sun_eh_take(ptr %exception, ptr %caught)
  %payload = getelementptr i8, ptr %caught, i64 16
  %code = load i32, ptr %payload
  ret i32 %code
}

; Terminates before an escaping cleanup failure can outlive its stack record.
define void @sun_fixture_escape() uwtable personality ptr @sun_eh_personality {
entry:
  %nested = alloca [176 x i8], align 16
  invoke void @fixture_throw(ptr @a_type, i32 10) to label %unexpected unwind label %cleanup
cleanup:
  %original = landingpad {ptr, i32} cleanup catch ptr @root_type
  %previous = call ptr @sun_eh_push(ptr %nested)
  invoke void @fixture_throw(ptr @b_type, i32 20) to label %unexpected unwind label %terminate
unexpected:
  unreachable
terminate:
  %lp = landingpad {ptr, i32} catch ptr null
  call void @sun_eh_terminate()
  unreachable
}

; Leaves recursion frames intact during the handler search and unwinds them later.
define internal void @fixture_deep(i32 %depth) noinline uwtable {
entry:
  %bottom = icmp eq i32 %depth, 0
  br i1 %bottom, label %fail, label %recurse
fail:
  call void @fixture_throw(ptr @a_type, i32 10)
  unreachable
recurse:
  %next = sub i32 %depth, 1
  notail call void @fixture_deep(i32 %next)
  ret void
}

; Does not intercept A merely because this frame has a handler for B.
define internal void @fixture_nonmatching(i32 %depth) noinline uwtable personality ptr @sun_eh_personality {
entry:
  invoke void @fixture_deep(i32 %depth) to label %unexpected unwind label %wrong_handler
unexpected:
  unreachable
wrong_handler:
  %lp = landingpad {ptr, i32} catch ptr @b_type
  call void @sun_eh_terminate()
  unreachable
}

; Catches A after traversing deep frames and skipping an unrelated handler.
define i32 @sun_fixture_deep(i32 %depth) uwtable personality ptr @sun_eh_personality {
entry:
  %caught = alloca [144 x i8], align 16
  invoke void @fixture_nonmatching(i32 %depth) to label %unexpected unwind label %handler
unexpected:
  unreachable
handler:
  %lp = landingpad {ptr, i32} catch ptr @root_type
  %exception = extractvalue {ptr, i32} %lp, 0
  call void @sun_eh_take(ptr %exception, ptr %caught)
  %payload = getelementptr i8, ptr %caught, i64 16
  %code = load i32, ptr %payload
  ret i32 %code
}
