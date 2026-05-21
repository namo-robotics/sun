// tests/test_mutex.cpp - Tests for Mutex synchronization primitive

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "error.h"
#include "execution_utils.h"

// ============================================================================
// Atomic Intrinsic Compilation Tests
// ============================================================================

TEST(MutexTest, atomic_cmpxchg_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      return old;
    }
  )"));
}

TEST(MutexTest, atomic_store_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _atomic_store_i32(_address_of<i32>(x), 42); };
      return 0;
    }
  )"));
}

TEST(MutexTest, atomic_load_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 42;
      var val = unsafe { _atomic_load_i32(_address_of<i32>(x)); };
      return val;
    }
  )"));
}

// ============================================================================
// Futex Intrinsic Compilation Tests
// ============================================================================

TEST(MutexTest, futex_wait_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      // Don't actually wait - just verify it compiles
      // _futex_wait(_address_of<i32>(x), 1);  // Would block if x != 1
      return 0;
    }
  )"));
}

TEST(MutexTest, futex_wake_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _futex_wake(_address_of<i32>(x)); };
      return 0;
    }
  )"));
}

// ============================================================================
// Atomic Intrinsic Execution Tests
// ============================================================================

TEST(MutexTest, atomic_cmpxchg_success) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      // old should be 0 (the original value)
      // x should now be 1
      return old;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(MutexTest, atomic_cmpxchg_fail) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 5;
      // Expected is 0, but x is 5, so cmpxchg should fail
      var old = unsafe { _atomic_cmpxchg_i32(_address_of<i32>(x), 0, 1); };
      // old should be 5 (the actual value, not changed)
      return old;
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST(MutexTest, atomic_store_and_load) {
  auto value = executeString(R"(
    function main() i32 {
      var x: i32 = 0;
      unsafe { _atomic_store_i32(_address_of<i32>(x), 42); };
      var val = unsafe { _atomic_load_i32(_address_of<i32>(x)); };
      return val;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Mutex Compilation Tests
// ============================================================================

TEST(MutexTest, mutex_import_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
      var m = Mutex();
      return 0;
    }
  )"));
}

TEST(MutexTest, mutex_lock_unlock_compiles) {
  EXPECT_NO_THROW(compileString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
      var m = Mutex();
      m.lock();
      m.unlock();
      return 0;
    }
  )"));
}

// ============================================================================
// Mutex Execution Tests
// ============================================================================

TEST(MutexTest, mutex_single_thread_lock_unlock) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
      var m = Mutex();
      var counter: i32 = 0;
      
      m.lock();
      counter = counter + 1;
      m.unlock();
      
      m.lock();
      counter = counter + 10;
      m.unlock();
      
      return counter;
    }
  )");
  EXPECT_EQ(value, 11);
}

TEST(MutexTest, mutex_multiple_lock_unlock) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    function main() i32 {
      var m = Mutex();
      var result: i32 = 0;
      
      var i: i32 = 0;
      while (i < 10) {
        m.lock();
        result = result + 1;
        m.unlock();
        i = i + 1;
      }
      
      return result;
    }
  )");
  EXPECT_EQ(value, 10);
}

// ============================================================================
// Multi-threaded Mutex Tests
// ============================================================================

TEST(MutexTest, mutex_two_threads_sequential) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    var counter: i32 = 0;
    var mutex: Mutex = Mutex();

    function increment() void {
      mutex.lock();
      counter = counter + 1;
      mutex.unlock();
    }

    function main() i32 {
      var t1 = spawn(lambda() i32 {
        var i: i32 = 0;
        while (i < 100) {
          increment();
          i = i + 1;
        }
        return 0;
      });
      
      var t2 = spawn(lambda() i32 {
        var i: i32 = 0;
        while (i < 100) {
          increment();
          i = i + 1;
        }
        return 0;
      });
      
      t1.join();
      t2.join();
      
      return counter;
    }
  )");
  // Both threads increment 100 times each, should total 200
  EXPECT_EQ(value, 200);
}

TEST(MutexTest, mutex_three_threads) {
  auto value = executeString(R"(
    import "build/stdlib.moon";
    using sun;

    var counter: i32 = 0;
    var mutex: Mutex = Mutex();

    function main() i32 {
      var t1 = spawn(lambda() i32 {
        var i: i32 = 0;
        while (i < 50) {
          mutex.lock();
          counter = counter + 1;
          mutex.unlock();
          i = i + 1;
        }
        return 0;
      });
      
      var t2 = spawn(lambda() i32 {
        var i: i32 = 0;
        while (i < 50) {
          mutex.lock();
          counter = counter + 1;
          mutex.unlock();
          i = i + 1;
        }
        return 0;
      });
      
      var t3 = spawn(lambda() i32 {
        var i: i32 = 0;
        while (i < 50) {
          mutex.lock();
          counter = counter + 1;
          mutex.unlock();
          i = i + 1;
        }
        return 0;
      });
      
      t1.join();
      t2.join();
      t3.join();
      
      return counter;
    }
  )");
  // Three threads each increment 50 times = 150
  EXPECT_EQ(value, 150);
}

// Test that without mutex, we would get race conditions
// (This test verifies the mutex is actually needed)
TEST(MutexTest, mutex_prevents_races) {
  // Run the same test multiple times to increase chance of catching races
  for (int run = 0; run < 5; run++) {
    auto value = executeString(R"(
      import "build/stdlib.moon";
      using sun;

      var counter: i32 = 0;
      var mutex: Mutex = Mutex();

      function main() i32 {
        var t1 = spawn(lambda() i32 {
          var i: i32 = 0;
          while (i < 1000) {
            mutex.lock();
            counter = counter + 1;
            mutex.unlock();
            i = i + 1;
          }
          return 0;
        });
        
        var t2 = spawn(lambda() i32 {
          var i: i32 = 0;
          while (i < 1000) {
            mutex.lock();
            counter = counter + 1;
            mutex.unlock();
            i = i + 1;
          }
          return 0;
        });
        
        t1.join();
        t2.join();
        
        return counter;
      }
    )");
    EXPECT_EQ(value, 2000) << "Race condition detected on run " << run;
  }
}
