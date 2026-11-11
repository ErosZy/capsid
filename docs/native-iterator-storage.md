# Array and string iterator storage

The native VM keeps an array or string iterator's cursor and retained input in
the allocation that owns the iterator object. The JavaScript object layout and
the layouts of other classes stay the same. The object's allocation includes
enough aligned space for the cursor; its existing finalizer releases the input,
and the ordinary object free releases the complete allocation.

An empty iterator initially has no own-property allocation. Adding its first
own property allocates the existing shape's capacity before the shape or value
is changed. Later property growth, deletion, compaction, accessors, symbols and
prototype changes follow the existing object paths. The iterator remains
extensible, and freezing the JavaScript object does not freeze its internal
cursor. Array mutation, typed-array detachment and string code points retain
their normal iterator behavior.

Memory accounting includes the inline cursor in object bytes and counts only
allocations that exist. Garbage collection retains the same input edge and can
collect cycles involving an iterator, its input and application properties.
No iterator object, user getter, return hook, EOF or cancellation transition is
replaced by a shortcut.

`quickjs_iterator_storage` checks observable iterator behavior, 2,048 cyclic
ownership graphs, exact allocator and memory counters, and all 38 allocation
failure points of iterator creation followed by own-property growth. Its CTest
budget is 20 seconds. The original job-context properties, original Hono guest
and 500 millisecond application deadline, native fetch matrices, and retired
branch contracts remain separate gates. Private timing observations are not a
formal performance or density qualification.
