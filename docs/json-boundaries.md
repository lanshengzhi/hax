# JSON boundaries

This inventory records the production Jansson users left after the trace, transcript, and history
migration in issue #17. New code should use the project-owned adapter in `src/json.h` unless it is
crossing one of the compatibility boundaries below.

## Project-owned paths

The following paths use `hax::json` values or typed adapters: configuration and catalog metadata,
session control and item records, credential-store records, transport error/retry inspection,
provider response adapters, trace payload formatting, transcript rendering, history replay, and
tool-schema construction. The legacy schema object remains available only for provider callers.

## Remaining Jansson users

### Provider and wire compatibility

- `src/providers/anthropic.cpp`, `src/providers/anthropic.h`, `src/providers/http_provider.cpp`, and
  `src/providers/http_provider.h`: legacy model-list callback objects are materialized at provider
  callback boundaries.
- `src/providers/anthropic_body.cpp`, `src/providers/chat_body.cpp`, and
  `src/providers/responses_body.cpp` (and their headers): provider request builders retain Jansson
  objects for cache markers, protocol-owned fields, and the wire interface.
- `src/providers/wire.cpp` and `src/providers/wire.h`: the legacy wire body interface applies
  configured extra fields and emits request JSON.
- `src/providers/codex.cpp` and `src/providers/codex.h`: Codex request customization and legacy
  catalog callback objects remain Jansson-owned.
- `src/providers/config_provider.cpp` and `src/providers/config_provider.h`: configured-provider
  extra bodies and recursive merges cross into the legacy wire boundary.
- `src/providers/codex_auth.cpp`, `src/providers/codex_login.cpp`, and
  `src/providers/openai_compat_json.cpp`: authentication documents and the compatible-provider
  reasoning-detail merge still need Jansson's mutable object operations.
- `src/providers/llamacpp.cpp`, `src/providers/llamacpp.h`, `src/providers/openrouter.cpp`, and
  `src/providers/openrouter.h`: legacy catalog and model callback signatures expose Jansson.

### Tool and display compatibility

- `src/agent_dispatch.cpp`: live tool-header argument extraction still reads Jansson objects.
- `src/tools/bash.cpp`, `src/tools/edit.cpp`, `src/tools/path_preprocess.cpp`, `src/tools/read.cpp`,
  `src/tools/task_wait.cpp`, and `src/tools/write.cpp`: tool execution and argument preprocessing
  retain Jansson validation and mutation behavior.

### Schema compatibility seam

- `src/tool_schema.cpp` and `src/tool_schema.h`: `tool_schema_value()` is the project-owned schema
  representation used by transcript rendering. `tool_schema_build()` and its public `struct json_t`
  return type convert that value to a new Jansson object for the provider request builders listed
  above; this is the remaining intentional legacy API.

Tests under `tests/` that inspect those legacy protocol or tool objects also include Jansson, but
are not production users. This list should be updated when one of the boundaries moves.
