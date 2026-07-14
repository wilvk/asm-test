using System;
using System.Collections.Generic;
using System.Runtime;
// Forces compacting gen2 GCs that MOVE surviving objects, so a profiler's MovedReferences2 fires.
class GcMover {
    static object[] survivors = new object[40000];
    static void Main() {
        for (int round = 0; round < 6; round++) {
            var garbage = new List<byte[]>();
            for (int i = 0; i < 150000; i++) {
                var b = new byte[64];
                if ((i & 3) == 0) survivors[i % survivors.Length] = b; // fragmented survivors
                else garbage.Add(b);
            }
            garbage.Clear();
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
        }
        Console.WriteLine("HELLO_GC_MOVER done gen2=" + GC.CollectionCount(2));
    }
}
