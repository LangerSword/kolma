# REA artifact evidence for the Kolma RBRC report

Retrieved 2026-11-24. REA 6.4.0 (`rea-agents`), skill
`reverse-engineer-anything` v37, Hermes 0.23.1, protocol 2025-11-25.
Sources below are **REA-observed artifact facts**, not source-code claims.
Native decompilation was not available in this session; see the provider gap.

## REA-validated target identities

| Target | REA format / kind | Architecture | SHA-256 observed by REA |
| --- | --- | --- | --- |
| `/usr/bin/zstd` (zstd 1.5.7) | ELF executable | x86_64 | `6b799c822798547e33f3d928d04492ca4814d4b45088786b73d700f0cc0d905a` |
| `/usr/bin/xz` (XZ Utils 5.8.4) | ELF executable | x86_64 | `24c477492f40fd8b1b6eca2d6cc020e47d4c9c11ee1bfb9ba57fb8e2f6a11bda` |
| `/usr/bin/bzip2` (1.0.8) | ELF executable | x86_64 | `f7a4fc551da5bca1a77026c9530273224daeedfca5492665bed1c383feb42096` |
| `/home/lakshaya/projects/kolma/build/kolma` (v1.3.0) | ELF executable | x86_64 | `eb77e4e8a89dfd1ec23a3f3901f71882ccbee8c2aedb5bb767e5dfef25372d73` |
| `/home/lakshaya/.hermes/cache/scratch/rea-kolma/out.zip` | ZIP archive | – | `4b463d8b4c69f44e34b8ec2e466e1ccab39397221aafda7d72ac68662d26236b` |

These identities are useful for a presentation because they pin the exact
shipped artifacts behind every competitor number. They do **not** by themselves
establish the internal algorithm used by a binary.

## Successful artifact inspection: Kolma-produced ZIP

REA `inspect_artifact` on the Kolma-generated ZIP returned:

- Evidence: `ev_30021100ba7b71f75b30c1f789150e64d58b16ae1a3976826f2db3d2897068d0`
- Inventory sub-evidence:
  `ev_292cc3e21ac00ef5992ff2fcc97fe968a93405faad2f7bd12903abc27ba5aaf0`
- Provider: `rea-artifact-graph` v1.
- Root: ZIP, 548 bytes, `hash_status: verified`.
- Graph: 5 nodes, 5 occurrences, 4 containment edges.
- Members observed:
  - `proj/` virtual directory.
  - `proj/a.txt`: 23 bytes declared, 25 bytes compressed, SHA-256
    `73907a9efef6cbec0da43178ba2cab7cd7939986684dedc97f4707f3a409ace5`,
    verified.
  - `proj/sub/` virtual directory.
  - `proj/sub/b.txt`: 15 bytes declared, 17 bytes compressed, SHA-256
    `4e7267fd5c54130fe83e69920d63e38117957e56f9172965b8db27c75a3c1a96`,
    verified.
- `integrity_contradictions: []`, `hypotheses: []`, `unexplored_branches: []`.
- Coverage: `complete-within-substeps`.

The tiny entries grew slightly in the ZIP container, which is expected and
matches Kolma's honest raw-fallback rule. For a report, this is concrete
shipped-artifact evidence that Kolma's universal-format output is a well-formed
ZIP with independently verifiable member digests.

Stated REA limitation: ZIP inventory lists central-directory entries only;
bytes after the central directory, including an APK Signing Block or ZIP
comment, are not occurrences.

## Provider gap encountered

`open_binary` completed for all ELF targets, but native analysis tools were
blocked:

- `GHIDRA_INSTALL_DIR` is unset; no Ghidra 12.1.x installation was found.
- Hopper launcher `/opt/hopper/bin/Hopper` is missing.
- IDA config (`REA_IDA_MCP_CONFIG`) is unset.
- `inspect_binary_layout` additionally requires `REA_PWNTOOLS_PYTHON` pointing
  to a Python with pwntools 4.15.0, pyelftools 0.33 and Unicorn 2.1.2.
  No such standalone Python was found; REA's bundled bridge is not a caller
  supply of those modules.
- `binary_overview` returned `provider_unavailable` with candidate rejections
  for Ghidra, Hopper and IDA.

Therefore **no decompiled functions, pseudocode, symbol identities or
instruction-level claims are made in this report**. Competitor mechanism claims
in `docs/research/rbrc-prior-art.md` come from primary specs, papers, pinned
repositories and measured binaries, not from REA disassembly.

## How to use this in the presentation

1. **Artifact identity slide.** Show the pinned SHA-256 table beside the
   competitor benchmark table. It proves the numbers refer to specific shipped
   binaries, not vague tool names.
2. **Interop evidence slide.** Show the REA ZIP graph: root + two verified
   members, zero integrity contradictions. This supports the claim that Kolma
   writes standard archives an independent tool can inspect.
3. **Method-boundary slide.** State plainly: RBRC's novelty is an original
   combination of old ingredients; artifacts were identity-checked with REA,
   but mechanism attribution came from literature and measurement because
   native decompilation providers were unavailable.
4. **Limitations slide.** Include the provider gap verbatim. It is stronger
   evidence discipline than pretending decompilation happened.

## Follow-up to unlock deeper REA evidence

With the user's go, configure one native provider explicitly in Hermes MCP
settings and restart the session, then re-run REA on the pinned artifacts:

```yaml
mcp_servers:
  rea:
    command: npx
    args: ["-y", "rea-agents@6.4.0", "mcp"]
    timeout: 1000
    env:
      GHIDRA_INSTALL_DIR: /absolute/path/to/ghidra_12.1.x_PUBLIC
      JAVA_HOME: /absolute/path/to/jdk-21
      REA_GHIDRA_STARTUP_TIMEOUT_MS: "900000"
      REA_PWNTOOLS_PYTHON: /absolute/path/to/python-with-pwntools-4.15.0
```

Then the next report can add function-level Evidence IDs, inferred procedure
names kept distinct from real symbols, and corroborated branch behaviour.
