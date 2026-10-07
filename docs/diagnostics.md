# Runtime flight recorder

Use a diagnostic profile on the normal client:

```sh
saga --debug
saga --debug debug
saga --debug trace
saga --debug wire
saga --debug forensic
saga --debug off
```

The client configures its own connection to `sagad`, including an already running
compatible daemon. Enable recording before persona activation. Other clients are
unaffected. The daemon also accepts these flags as defaults for new connections.
Background detached maintenance is not automatically recorded as a user session.
Restart an older daemon before using this client; persisted databases remain
schema 13 and existing sessions remain readable.

Profiles are cumulative:

| Profile | Recorded information |
| --- | --- |
| off | No diagnostic recorder; normal errors still work |
| debug | Build, persona and provider summaries, lifecycle, runtime decisions, output budgets, tools, memory write outcomes, cancellation, errors |
| trace | Context/message/schema accounting, selection/provenance, memory retrieval rankings and rejection reasons, cognition and operation spans, search/instance decisions, parsed provider events |
| wire | HTTP request/response bodies and headers, curl timing measurements, SSE bytes with receive boundaries |
| forensic | Logical prompt snapshots, SOUL, full provider-exposed reasoning/public output/tool assemblies, persistence parameters, memory candidates, context checkpoint snapshots and filesystem diffs |

Reasoning is recorded only if the provider exposes it. This changes neither the
normal reasoning visibility policy nor the conversation history. Diagnostic
runtime events never become model messages. Provider capabilities are adapter
reports, not evidence that every advertised feature has been exercised.

## Files and permissions

By default recordings live in `$XDG_STATE_HOME/saga/logs`, falling back to
`~/.local/state/saga/logs`. Each connection initially opens a bootstrap `saga-0`
recording for setup/discovery. Activating a persona rebinds to its existing SQLite
session number:

```text
logs/noa-42-1791310031/
  noa-42-1791310031.log
  manifest.json
  payloads/<event-seq>-<field-index>.json
  crash/flight-recorder.jsonl
  crash/fatal-signal.log
```

The basename is `<sanitized-persona>-<session-number>-<unix-seconds>.log`.
A deterministic directory suffix resolves collisions without overwriting files.
Directory permissions are 0700; logs, manifests and payloads are 0600. Never
commit recordings, persona content, databases or generated artifacts.

The principal format is JSONL, version 1: one event per line. Every normal event
has `seq`, UTC `ts`, steady-clock `mono_ns`, `severity`, diagnostic `level`,
`component`, `event` and available correlation IDs. Sequence numbers are global
within that persona/session recording, including emissions from multiple threads.
The bootstrap and each rebound session start a new sequence.

For live monitoring, follow the **stable connection path** printed by the client:

```sh
tail -F --max-unchanged-stats=1 /path/printed/by/saga/follow
```

Each connection owns a `follow` symlink in its initial bootstrap bundle. It is
atomically updated when a persona is created or activated, the session changes,
the log rotates, or the current persona is erased. Separate connections have
separate aliases. The link copies no private events into a shared log; complete
recordings remain in their respective persona/session bundles. Manifests and
session headers expose `follow_path` alongside the archive location.

Following the initial `saga-0-….log` archive directly stops at
`debug.session_completed` with `reason=session_rebound`: the recorder has moved
to a new bundle. This is a file change, not the end of agent activity. `tail -F`
cannot switch to another pathname by itself. Use `follow` for ongoing activity
and the manifest's `log_files` for complete historical inspection. Very brief
intermediate sessions or rapid rotations can pass between tail's checks; those
events remain in the archives subject to configured retention. The alias is
removed when its connection disconnects. Erasing a persona also removes stale
crashed-recorder aliases still pointing to its deleted bundles.

Large fields become references to JSON sidecars; prompt and HTTP request bodies
always have sidecars. References include path, byte count and SHA-256 of the
stored **redacted** JSON representation. Decode a string-valued sidecar as JSON
to obtain its original text. Hashes are content fingerprints, not authentication.
The manifest records build metadata, context, files, sidecars, event/error/drop
counts and times. Footer metrics describe activity before the footer itself;
the manifest contains the final totals.

## Correlation and runtime decisions

SQLite's existing append-only runtime journal feeds the diagnostic sink once.
Transient generation/activity events use the same observer. Spans, diagnostic
context and provider transport supplement that journal rather than replacing it.
The active recorder is scoped to a daemon connection thread; a bounded writer
thread serializes events. Core generation semantics remain provider independent.

IDs include existing persona, session, user-message, turn, generation, task, tool
call and execution IDs. HTTP requests and diagnostic spans receive local
monotonic counters. Search attempts retain their query IDs. A retry and a
continuation remain separate generation segments of the same turn:

```json
{"component":"generation","event":"generation.classified","classification":"output_limit","normalized_finish_reason":"length"}
{"component":"runtime","event":"runtime.decision","decision":"continue_generation","reason":"output_limit","policy":"preserve_valid_incomplete_generation"}
```

Actual records also include timestamps, sequence and correlation fields. A
reasoning-only 8192-token limit is never labelled an empty response. Decisions
explain retries, output-limit continuation, true empty recovery, tool execution
and turn completion/failure/cancellation. Identity conflicts and suppressed
user/tool duplicates are visible through invariant and idempotency events.
Workspace transitions include the previous/next paths and explicitly preserve
the logical user turn.

## Context and memory

`context.accounting` records message/schema byte and token estimates, the input
budget, capacity/utilization and effective output budget. Physical component
counts sum to the reported total. `serialization_tokens` separately reports the
estimate over the complete serialized arrays; punctuation and per-unit overhead
make these estimates slightly different. They are estimates from Saga's existing
conservative tokenizer, not exact backend tokenization.

`context.semantic_components` breaks down system/SOUL and nested wake/live state
(memory, self-model, goals, research, project and attention). These inclusive
subcomponent estimates **must not be added** to physical message totals.
`context.message_selection` retains SQLite message IDs and exclusion reasons.
`context.diff` compares redacted message fingerprints and schema hashes between
requests. Duplicate identical blocks can share a fingerprint; message selection
provides the distinct source IDs. Compaction/reset records identify checkpoints,
source messages removed from prompt selection, retained references, objective,
checks and observations. Reset accounting covers dialogue since the previous
checkpoint and handoff estimates; it excludes system, schemas and live state.
Generation accounting provides complete serialized request estimates. Latest
requests and recent dialogue can survive as excerpts rather than full messages.
SQLite source records are preserved.

Memory searches report their actual ranking factors, matching words, selected
and rank-limited candidates, and project/supersession rejection reasons. Write
records include database path, table, SQL operation, operation ID, affected rows
and available target row IDs. Forensic records include candidates and SQL bound
parameters. Fact deduplication distinguishes inserting, merging evidence and
superseding; duplicate praxis rejection is explicit.

Transaction writes are buffered until COMMIT. ROLLBACK produces rolled-back write
outcomes rather than false commit records. `INSERT_OR_UPDATE_OR_IGNORE` honestly
labels statements whose SQLite execution path cannot be inferred from the verb
alone. Detailed SQL and fact deduplication explain their actual target/decision;
`last_insert_row_id` is the SQLite register and is not an UPDATE target identity.

## Wire recording and redaction

Redaction is central, before queueing or sidecar storage. It handles sensitive
JSON keys, serialized nested JSON, common assignments/URL query credentials,
HTTP authentication/cookies, configured credentials, PEM private keys and SSH
key material. Semantic password triples and positional key/value arrays are
also protected. Binary/base64 file contents are withheld by default.

SSE receive events record original offsets, sequence and sizes; the assembled
transport is recorded at request completion or interruption, up to 16 MiB.
Original `data`, `event`, `id` and `retry` fields survive unless redaction requires
rewriting a frame. Stream-aware redaction assembles semantic channels to detect
credentials split across distinct SSE deltas. An affected channel's wire deltas
are replaced with `<redacted:stream-channel>`; its complete redacted content is
available in the generation snapshot. Raw offsets refer to the original transport,
so they cannot index a rewritten redacted payload byte-for-byte. With explicit
secret permission, transport text is preserved without these rewrites.

Incremental text/tool/stdout fragments record metadata instead of independently
persisting fragments that could assemble into a credential. Complete outputs are
recorded with central redaction. Wire capture uses transport chunk boundaries,
not a promise that each chunk contains exactly one SSE frame. HTTP timing fields
are libcurl's measured timeline, not invented DNS/TLS lifecycle events. The compact
compatibility header uses curl's [official public ABI offsets](https://github.com/curl/curl/blob/master/include/curl/curl.h).

Redaction cannot recognize every unlabelled secret or personal fact. All enabled
profiles can still contain private information, especially trace/forensic.
Inspect recordings before sharing them. To deliberately disable redaction:

```sh
saga --debug forensic --debug-allow-secrets
```

`--debug-no-redact` is an alias. The client displays an explicit warning.

## Filtering, buffering and retention

```sh
saga --debug forensic --debug-dir /private/diagnostics \
  --debug-components runtime,generation,context,provider,http,sse,memory,tool \
  --debug-exclude tui --debug-rotate-size 100M --debug-keep 3
```

Filters match exact components and their dotted children. Errors/fatal events and
recorder headers/footers bypass filters. Default rotation is 64 MiB. `--debug-keep`
limits rotated log parts in the current bundle; zero (default) retains all parts.
Sidecars and crash checkpoints remain available for forensic references; retention
does not silently erase earlier session bundles. Delete complete bundles explicitly
when they are no longer needed. Compression is not introduced.

The queue is bounded to 8 MiB, with one oversized critical event permitted to make
progress. Lifecycle, errors, raw wire boundary records, tool outcomes and memory
writes wait for space; verbose parsed events, UI and sampler events can be dropped.
`logger.events_dropped` reports saturation. Deltas in the existing runtime journal
are coalesced; hidden raw reasoning never floods the terminal.

Significant terminal events flush the writer and update the manifest and a bounded
1000-event crash checkpoint. Regular events are written without per-token fsync.
I/O failure disables recording with `logger.error` on stderr and does not terminate
the agent. Linux process samples include RSS/virtual memory, thread/FD counts and
cumulative CPU time, at a five-second interval during active inference/service.
The IPC options permit changing that interval.

## Crash guarantees and limitations

Explicit diagnostic crashes and `std::terminate` drain the queue and dump the ring.
Fatal signal handlers use only lock-free descriptor reads, `write` and `_exit`;
they append an emergency marker beside the most recent durable ring checkpoint.
They cannot safely serialize arbitrary C++ state in a signal handler. The marker
is an emergency record outside the normal ordered JSONL stream. SIGKILL, power
loss or a catastrophic failure can lose buffered events since the last checkpoint.
SQLite's durable journal supplies complementary recovery provenance.

The recorder observes Saga-mediated filesystem edits and shell output; it is not
an operating-system-wide file watcher. Scheduler/operation spans expose connection
and execution timing, but no new scheduler or lock tracing framework is added.
TUI activity transitions are recorded in the daemon; terminal drawing timings are
not sent over IPC. GPU metrics, external llama-server stderr/slot internals and
hidden provider reasoning are not invented or scraped. Available model/stream
metadata is retained without extra monitoring requests.

## Verification

`saga_debug` covers filenames, SHA-256 vectors, JSONL ordering/timestamps,
concurrent producers, filters, redaction (including split SSE credentials),
rotation, live `tail -F` across persona/session changes and rotation, independent
connection aliases, drop policy, private permissions, disk failure, sidecars, crash markers,
context diffs, 8192-token continuation, dispatch/tool idempotency and transactional
memory outcomes, file diffs, stdout/stderr, cancellation and a basic writer
benchmark. `saga_debug_wire` exercises the production HTTP/SSE adapter,
request correlation, artificially small output budgets, all CLI profiles,
optional API keys, tools, retrieval, provider errors and daemon lifecycle against
an isolated local mock. Existing research/search/reader/runtime regression suites
remain in the full CTest run. The research regression also checks recorded
fallback/claim/reader provenance without duplicate search events. Public services
are not queried for repetitive tests.
