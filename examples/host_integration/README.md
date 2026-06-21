# Example: host integration

A complete, self-contained host that drives **every layer** of the compiler in
one program — the role an SBC data plane plays:

1. **compile** an embedded topology-hiding ruleset to a native `.so`
   (`hmr::pipeline::Compiler`, the Facade over lex → parse → optimize → LLVM IR
   → `-O3` → emit → link);
2. **observe** load events (`hmr::module::ModuleObserver`, GoF Observer);
3. **load** + ABI-check the module (`hmr::module::ModuleManager`, `dlopen`);
4. build a per-thread **context** (`hmr::runtime::makeContext`) and bind
   `$LOCAL_IP`;
5. **apply** the module to a SIP request that leaks internal topology, printing
   before/after + the verdict;
6. **hot-swap** the module for a second ruleset and re-apply against a fresh
   packet (RCU-style reload — no restart).

See [`main.cpp`](./main.cpp). The ruleset uses only fully-lowered HMR features
(see [../../docs/compatibility-matrix.md](../../docs/compatibility-matrix.md)).

## Build & run

### As part of the main build
The example is built automatically by the top-level build when LLVM is available
(`-DHMR_BUILD_EXAMPLES=ON`, the default):

```sh
cmake -S . -B build -DLLVM_DIR="$(llvm-config-18 --cmakedir)"
cmake --build build --target hmr_host_integration
./build/examples/host_integration/hmr_host_integration
```

### Standalone
The example's `CMakeLists.txt` pulls the compiler in from the repository root,
so it also configures on its own:

```sh
cmake -S examples/host_integration -B build-example \
  -DLLVM_DIR="$(llvm-config-18 --cmakedir)"
cmake --build build-example
./build-example/hmr_host_integration
```

## Expected output (abridged)

```
[compile] TopologyHiding: 4 rule(s), 1 regex(es), ~15000 bytes
[observer] loaded: TopologyHiding

=== V1: topology hiding ===
--- before ---
INVITE sip:bob@example.com SIP/2.0
From: "Alice" <sip:alice@internal.local>;tag=99
...
Server: SecretPBX/9.9
User-Agent: InternalUA
X-Internal-Route: hop-1
...
--- verdict = 0 (OK) ---
--- after ---
INVITE sip:bob@example.com SIP/2.0
From: "Alice" <sip:alice@203.0.113.5>;tag=99      <- host masked
To: <sip:bob@example.com>
Contact: <sip:alice@10.0.0.1>                      <- Server/User-Agent/X-Internal-Route stripped
...

[observer] reloaded: TopologyHiding
=== V2: after hot reload ===
... (also adds `X-Anonymized: true`)
```

The exact byte size and ordering vary by host/LLVM version; the masked `From`
host, the stripped leak headers, and the post-reload `X-Anonymized` marker are
the observable effects.
