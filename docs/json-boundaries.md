# JSON boundaries

Glaze is the sole JSON implementation. `src/json.h` is the only project header that includes
Glaze directly; domain and provider interfaces use the project-owned `hax::json` types and typed
adapters.

## Project-owned paths

The adapter covers configuration and catalog metadata, session control and item records,
credential-store records, transport error/retry inspection, provider request and response
payloads, trace payload formatting, transcript rendering, history replay, tool arguments, and tool
schemas. Provider-specific adapters keep optional or opaque fields as raw Glaze fragments until a
call site needs a typed view.

Request builders construct `hax::json::value` trees and serialize them once at the wire boundary.
Extra provider fields are parsed into the same value type and merged recursively, so protocol
defaults and user passthrough fields never cross a second ownership or parser boundary. Existing
session JSONL records remain provider-independent and are read through the same Glaze adapter as
new records.

## Compatibility and contract coverage

The final-removal validation matrix covers:

- session creation, resume, fork, turn cuts, pruning, legacy JSONL fixtures, and lifecycle e2e;
- OpenAI Chat Completions, OpenAI Responses, Anthropic Messages, configured providers, OpenRouter,
  llama.cpp, Codex authentication/catalog/login, and wire dispatch;
- tool schema, argument validation, path rewriting, read/write/edit/bash/task behavior;
- catalog/config selection, pricing tiers, cache settings, trace redaction, transcript replay, and
  history reconstruction;
- normal, release, address/undefined sanitizer, thread sanitizer, supported-platform, and lint
  gates where the toolchain is available.

The compatibility fixtures under `tests/fixtures/session/` are intentionally unchanged. They prove
that session files written before the migration remain readable without retaining an alternate JSON
implementation.

## Migration evidence

Measurements use the same x86_64 Linux worker, GCC 16.2, C++23, `debugoptimized`, and an isolated
Meson build directory. Build times are wall-clock seconds from `date +%s%N`; the final compile time
is also confirmed by the clean build's Ninja log. Runtime values are the median of five fresh
process launches. The runtime fixtures are `test_json` (representative adapter parsing) and
`test_session_contract` (legacy/session compatibility).

| Measurement | Before final removal | After final removal | Change |
| --- | ---: | ---: | ---: |
| Meson setup | 5.871 s | 1.894 s | -67.7% |
| Clean compile | 260.608 s | 172.742 s | -33.7% |
| `hax` binary (unstripped) | 45,585,328 B | 51,762,736 B | +13.6% |
| `test_json` median | 0.001818 s | 0.001873 s | +3.0% |
| `test_session_contract` median | 0.001703 s | 0.001864 s | +9.5% |

The build figures are worker wall-clock comparisons, not parser-only benchmarks: the final compile
ran after the focused iteration had warmed the compiler cache, so the reduction is not attributed
solely to the JSON change. The binary-size increase is the unstripped `debugoptimized` artifact;
Glaze was already present before this ticket, and the final build adds no JSON runtime library.
The increase is the cost of the project-owned value/tree and raw-fragment mutation paths that
replace the former external DOM boundary. The final artifact's stripped size is 3,155,536 bytes;
normal, sanitizer, and lint gates provide the corresponding correctness and safety checks.

The isolated Linux worker gate record is: explicit-ticket-root normal `make tests` 107/107,
explicit-ticket-root address/undefined sanitizer `make tests` 107/107, explicit-ticket-root thread
sanitizer `make tests` 107/107, explicit-ticket-root release `make` successful, and `make lint`
successful. The worker cannot execute macOS, FreeBSD, or OpenBSD jobs; the same
platform-neutral Meson, dependency-install, and static-packaging paths contain no reference to a
second JSON implementation and remain covered by their platform CI.
