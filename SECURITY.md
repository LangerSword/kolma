# Security policy

## What kolma protects

**Encrypted archives (format v2)** use Encrypt-then-MAC:

- ChaCha20 (RFC 8439) per chunk, with a distinct nonce per chunk index.
- PBKDF2-HMAC-SHA256, 200 000 iterations, 16-byte random salt, deriving
  **two independent keys**: one for encryption, one for authentication.
- HMAC-SHA256 over `payload || header-without-the-tag`. Every metadata byte
  (sizes, chunk table, salt, nonce, KDF parameters) is authenticated along
  with the ciphertext.
- The tag is verified in constant time **before any output file is created**.
  A wrong password, a flipped bit anywhere, or a chunk reordering fails
  cheaply and writes nothing.

**All archives** carry a CRC-32 of the original data, verified on restore.

**The parser treats every header byte as hostile.** Chunk tables are bounded
by the file size before any allocation, chunk sizes are capped at 64 MiB,
KDF iterations at 10 000 000, headers at 64 MiB; inconsistent size tables are
rejected; recorded source names never become output paths unless they are a
single, control-free path component. Outputs are never overwritten without
`--force` (dangling symlinks count as existing). A libFuzzer harness runs
against the parser and every decoder under ASan/UBSan in CI, and crash inputs
are kept as regression seeds.

## What kolma does NOT protect

- **The crypto is a from-scratch implementation and has not been audited.**
  The primitives are checked against published RFC/FIPS vectors, which proves
  correctness of the algorithms, not absence of side channels or implementation
  flaws. For anything that matters, use age or GPG.
- **Unencrypted archives are not tamper-proof.** CRC-32 detects corruption,
  not deliberate modification.
- **Legacy v1 archives have no authentication tag.** They are readable for
  compatibility and are reported as `legacy v1, NOT authenticated`.
- **Metadata is not confidential.** Even in an encrypted archive, the original
  file name, sizes, type, algorithm and timestamps are stored in plaintext in
  the header (they are authenticated, not encrypted).
- **Passwords on the command line are visible in `ps`.** Use `KOLMA_PASSWORD`;
  the CLI prints a warning when `--password` is used.
- kolma is a compression tool, not a secure-delete tool: it does not scrub
  originals or temp files beyond normal deletion.

## Supported versions

| Version | Supported |
| --- | --- |
| 1.x | yes |
| < 1.0 | no |

## Reporting a vulnerability

Please report privately via GitHub's
[private vulnerability reporting](https://github.com/LangerSword/kolma/security/advisories/new)
on this repository, or email lakshayasharma695@gmail.com with the details and
a repro if you have one. You should hear back within a week. Please do not
open public issues for unfixed vulnerabilities.
