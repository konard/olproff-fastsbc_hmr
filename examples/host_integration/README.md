# Example: host integration

A complete, self-contained host that drives **every layer** of the compiler in
one program — the role an SBC data plane plays:

1. **compile** an embedded topology-hiding ruleset to a native `.so`
   (`hmr::pipeline::Compiler`, the Facade over lex → parse → optimize → LLVM IR
   → `-O3` → emit → link);
2. **observe** load events (`hmr::module::ModuleObserver`, GoF Observer);
3. **load** + ABI-check the module (`hmr::module::ModuleManager`, `dlopen`);
4. build a per-thread **context** (`hmr::runtime::make_context`) and bind
   `$LOCAL_IP`;
5. **apply** the module to a SIP request that leaks internal topology, printing
   before/after + the verdict;
6. **hot-swap** the module for a second ruleset and re-apply against a fresh
   packet (RCU-style reload — no restart).

See [`main.cpp`](./main.cpp). The ruleset uses only fully-lowered HMR features
(see [../../docs/compatibility-matrix.md](../../docs/compatibility-matrix.md)).

## Build & run

The example is built automatically by the top-level Meson build when LLVM is
available (`-Dbuild_examples=true`, the default; it is gated on `enable_llvm`
because it drives the full compile pipeline). After the one-time ANTLR setup
(`eval "$(scripts/setup_antlr.sh)"`, see the [user guide](../../docs/user-guide.md)):

```sh
meson setup builddir \
  -Dantlr_jar="$ANTLR_JAR" -Dantlr_inc="$ANTLR_INC" -Dantlr_libdir="$ANTLR_LIBDIR"
meson compile -C builddir example_host_integration
./builddir/examples/example_host_integration
```

(The ANTLR runtime is linked statically, so no `LD_LIBRARY_PATH` is needed.)

It must export its dynamic symbols (`export_dynamic: true` in
[`examples/meson.build`](../meson.build)) so the `dlopen`'d module resolves its
`hmr_rt_*` imports against this process.

## Expected output (abridged)

```
[compile] TopologyHiding: 4 rule(s), 0 regex(es), 15512 bytes
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

[compile] TopologyHiding: 3 rule(s), 0 regex(es), 15568 bytes
[observer] reloaded: TopologyHiding
=== V2: after hot reload ===
... (also adds `X-Anonymized: true`)
```

`0 regex(es)` is the point of review item #2: the `comparison-type
case-sensitive` host rewrite lowers to an **exact byte compare**, not a
`std::regex` — no regex slot is emitted. The exact byte size and ordering vary
by host/LLVM version; the masked `From` host, the stripped leak headers, and the
post-reload `X-Anonymized` marker are the observable effects.
