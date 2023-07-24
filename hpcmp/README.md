# High Performance Computing Memory Profiler
A tool for profiling memory usage in HPC applications in the context of plan-based scheduling.  

## Running the tool 

`$ valgrind --tool=hpcmp [--hpcmp-out-file <out-file>] <hpc-application>` will run `hpc-application` and export it's memory usage information to `out-file`, as JSON.  
If the `--hpcmp-out-file` switch is omitted, it will instead print the memory usage information to stdout, in a way that is a bit more dense than JSON. This is only for debugging.

The HPCMP tool is a proof-of concept. The same data could be extracted by leveraging the Linux kernel's perf/BPF instrumentation. However, Valgrind offers a much more flexible and stable play-ground for experimentation.

The tool is *S L O W*. It will take around 20x more CPU-time than native execution. Notice, I said *CPU-time*, actual time is worse: since the Valgrind's synthetic CPU is single-core, the execution time will scale linearly with the number of threads are spawned, no matter how many cores your machine has. For any non-stack read/store instruction, the tool looks for a memory block in a thread-local cache, in order to update the access count. If it's not found, then it looks it up in the thread-global block tracker. If it's not there either, it assumes it is a static memory acess (e.g. `.data`, `.bss` section) or a bogus access. It is then ignored, since we only care about memory allocated with `malloc()`. This implies, any static memory access will go through all the possible lookups. So the more static memory your program uses, the slower it runs.  


## Output format (JSON)
The base value is an array containing `MpEvent`s:
```json
[
    MpEvent,
    MpEvent,
    ...
]
```

There are two sub-types of `MpEvent`, but the common part is as follows:

```json
{
    "thid": u64,    // TID (thread ID) of the event.
    "icnt": u64,    // Instruction count since the last event within same thread.
    "id": u64,      // Unique ID of this event.
    ...
}

```

The first sub-type of `MpEvent` is `SyncMpEv`. It is identifiable by the presence
of member `sync`, of type `SyncEv` thus:


```json
{
    "thid": u64,    // TID (thread ID) of the event.
    "icnt": u64,    // Instruction count since the last event within same thread.
    "id": u64,      // Unique ID of this event.
    "sync": SyncEv  // Synchronization point between two threads
}

```

The second sub-type of `MpEvent` is `LifeMpEv`. It is identifiable by the presence
of member `life`, of type `LifeEv` thus:

```json
{
    "thid": u64,    // TID (thread ID) of the event.
    "icnt": u64,    // Instruction count since the last event within same thread.
    "id": u64,      // Unique ID of this event.
    "life": LifeEv  // Memory/sync object created/destroyed
}

```

`SyncEv` is also sub-typed, and the common part is:

```json
{
    "usage": [ BlockUsage, ... ], // Memory usage counters since last SyncEv 
                                  // within this thread.
    ...
}
```
The sub-types for `SyncEv` are `Fork`, `Join`, `Exit`, `Acquire`, `Release`.  
`Fork`:
```json
{
    "usage": [ BlockUsage, ... ], 
    "fork" : u64                  // TID of the child thread 
}
```
`Join`:
```json
{
    "usage": [ BlockUsage, ... ], 
    "join" : u64                  // TID of the child thread 
}
```
`Exit`:
```json
{
    "usage": [ BlockUsage, ... ], 
    "join" : null
}
```
`Acq`:
```json
{
    "usage": [ BlockUsage, ... ], 
    "acq" : u64                 // Address of the sync objects the acquire
                                // operation was performed on.
}
```
`Rel`:
```json
{
    "usage": [ BlockUsage, ... ], 
    "acq" : u64                 // Address of the sync objects the release
                                // operation was performed on.
}
```

`LifeEv` is also subyped, and the subtypes are `Alloc`, `Free`, `NewSync`, `DelSync`.   
`Alloc`:
```json
{
    "alloc": {
        "addr": u64,   // Address of the new memory allocation.
        "size": u64    // Size of the new memory allocation [Bytes].
    }
}
```
`Free`
```json
{
    "free": BlockUsage // Details of the memory allocation being freed
}
```
`NewSync`:
```json
{
    "newsync": {
        "prim": str, // Type of the newly created synchronization object. May be
                     // anything, only for debugging.     
        "addr": u64  // Address of the newly created synchronization object.
    }
}
```
`DelSync`:
```json
{
    "delsync": {
        "prim": str, // Type of the destroyed synchronization object. May be
                     // anything, only for debugging.     
        "addr": u64  // Address of the destroyed synchronization object.
    }
}
```

Finally, `BlockUsage` looks as follows:
```json
{
  "addr": u64, // Start address of the allocation.
  "size": u64, // Allocation size [Bytes]. Can be inferred, but helps debugging.
  "r": u64,    // Reads performed by current thread on this allocation since 
               // last SyncEv [Bytes].
  "w": u64     // Stores performed by current thread on this allocation since 
               // last SyncEv [Bytes].
}
```
