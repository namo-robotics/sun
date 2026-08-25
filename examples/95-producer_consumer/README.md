# Producer & Consumer

Two threads and one queue. The producer pushes ten squares onto a shared
`Queue<i64>`; the consumer takes them off and adds them up. The consumer
receives all ten and their sum is 385, whichever order the threads happen to
run in.

`Shared<T>` puts the queue in a heap box next to a reference count and a
mutex. `clone()` adds an owner, `lock()` returns a guard that holds the mutex
until it goes out of scope, and the last owner to be dropped frees the queue.
Sun never copies a class implicitly, so a thread cannot reach the queue by
accident — every extra owner is a visible `clone()`.

The capture list is what puts an owner inside a thread. `[producer_queue]` has
no `ref`, so the closure owns that clone: it moves in, the thread drops it when
the lambda ends, and the name cannot be used in `main` afterwards. Borrowing
with `[ref producer_queue]` is allowed for a single thread, but two threads
cannot borrow the same handle — the second is rejected with *cannot borrow
'producer_queue' as mutable because it is already borrowed*. Giving each
thread its own clone is what makes two of them legal.

Where the lock is taken decides how much the threads get in each other's way.
`take_one` holds it only for the `pop()`, and has released it by the time it
returns, so the consumer can wait for an empty queue without blocking the
producer. Sleeping while still holding the guard would serialise the two
threads completely.

The consumer stops after ten items because it knows how many to expect. A
queue that had to signal "no more items are coming" would need a flag of its
own alongside the items, read under the same lock — otherwise the producer
could add one more between the consumer's "is it empty?" and "is it done?"
questions.

```bash
./build.sh
./main
# items left: 0
# total: 385
```
