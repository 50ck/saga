# Generation and turn lifecycle

A session contains operator turns. A turn owns model generation segments and tool
executions; an HTTP request is a segment, never a new turn. `ModelBackend::generate`
produces typed `ProviderEvent` values. The OpenAI adapter alone interprets SSE,
`choices`, `reasoning_content`, `finish_reason`, usage and llama.cpp timings.
Other protocols can implement the same interface without changing the executive.
There are no additional dependencies or services.

`GenerationState` incrementally owns reasoning, commentary, public text, tool
fragments, usage, first-delta/public timing and a single terminal classification.
Tool fragments are assembled by index with stable IDs, fragmented names and
arguments. JSON is validated at the terminal boundary. Truncated tool arguments
are checkpointed but never executed. A full response message following deltas is
reconciliation metadata, not a second text source.

## Termination and recovery

- `stop` with public content or tools: a completed segment; the executive still
  decides whether tools, verification or a final answer must follow.
- `length`: `output_limit`, including a segment containing only reasoning.
- Reasoning without public output at `stop`: `reasoning_only`, not empty.
- No substantive output at `stop`: `empty`, with one explicit recovery attempt.
- Content filtering: a provider error, not an empty response.
- Missing terminal state, malformed data, HTTP errors, connection failures and
  cancellation have separate classifications.

Valid incomplete segments are persisted as assistant protocol state. Their
reasoning and partial text enter the next conversation request; incomplete tool
calls are supplied as internal continuation data and cannot execute until reissued
as valid calls. Saga appends a runtime attention snapshot, not a new operator
message. Continuations retain the turn ID, get a new generation ID and increase
the requested output allowance within configured and available-context limits.
This portable mechanism does not depend on provider-side persistent sessions or
an unfinished assistant-prefill API. Templates may differ in how they preserve
reasoning; detected llama.cpp requests retain `preserve_thinking`.

Transient transport errors get a bounded retry and short interruptible backoff.
The failed segment's buffers and metrics remain in the journal. The same canonical
conversation, including already committed tool results, is reused for the retry;
partial tool calls never execute. A retry does not rerun the tool scheduler.
Saga does not guarantee idempotency if a model deliberately issues a *new* tool
call with equivalent arguments; existing action gates still apply.

## Persistence and events

Migration 8 adds `turns`, `generations` and tool-run turn/generation/call provenance
without modifying old conversations. Generations retain compact checkpoints,
terminal classifications, input estimates and requested/effective output limits.
The existing append-only `events` table is the event journal. Runtime events carry
session, turn, generation, sequence and timestamp; tool events also carry call IDs.
UI updates are coalesced at 100 ms independently of persistence. Durable
text/reasoning/tool fragments and generation state are committed together at
one second or 4 KiB and at channel/terminal boundaries. Cancellation flushes
pending buffers; a sudden process crash can lose the final uncommitted interval.
Provider normalization diagnostics report per-kind counts at each checkpoint;
wire logging retains the original HTTP/SSE bytes and chunk metadata. Reasoning is stored privately and never emitted to the normal UI.
Provider buffers and tool fragments have explicit byte/count bounds.

The journal is separate from the `messages` table. Spinners, usage, status and
execution progress do not become model messages. Public `report_progress` calls
use the existing assistant-tool/result protocol and enter history once; public
text before tools remains assistant commentary. Native commentary events are
supported by the normalized interface and retained as assistant text when needed.
They do not end a turn.

Tool results are written to conversation history before the dependent generation.
Crash recovery marks active turns/segments interrupted and in-flight tools as
`tool.execution.unknown_after_crash`. Completed tool runs remain completed.
Recovery never automatically replays tools or resumes a crashed HTTP request.

## Output and reasoning budgets

The previous 8,192 limit came from `generation_reserve`, which set request
`max_tokens`. It remains the compatible initial default, not a universal ceiling.
Global `config.toml` supports:

| Setting | Default | Meaning |
| --- | --- | --- |
| `default_max_output_tokens` | `0` | Inherit `generation_reserve`; otherwise initial segment allowance |
| `hard_max_output_tokens` | `32768` | Maximum requested per segment |
| `max_continuations` | `3` | Valid incomplete segments allowed to continue |
| `max_generation_retries` | `1` | Retries per transiently failed segment |
| `reasoning_budget` | `"auto"` | `auto`, `fixed`, or `disabled` |
| `reasoning_soft_budget` | `0` | Fixed mode: explicit token estimate; auto mode: zero means 75% of output |
| `reasoning_hard_budget` | `0` | Fixed mode: explicit token estimate; auto mode: zero means 90% of output |
| `reasoning_control` | `false` | Opt-in llama.cpp capability probe/control |
| `stream_assistant_text` | `true` | UI token streaming; provider streaming stays enabled |

Each allowance is clamped to `context_window - estimated_input - safety_margin`,
the hard limit and exposed model `max_output_tokens` metadata. Continuations can
request twice the previous initial allowance, up to these limits. The journal
records all limits and their source, usage, finish reason and output composition.
Reasoning estimates use UTF-8 bytes/3 when dedicated reasoning usage is absent;
they are not an exact tokenizer. Auto thresholds only warn by default and never
terminate a working connection. Fixed mode uses only explicitly supplied budgets.

The documented llama.cpp `/v1/chat/completions/control` protocol is used only when
opted in and a bounded no-op probe returns the expected `success` boolean. The
request arms `reasoning_control`; a hard threshold can issue `reasoning_end` using
the streamed completion ID. Old or non-llama endpoints simply continue normally.
Control requests have a two-second limit; no arbitrary reasoning tags are parsed.
The protocol follows the [llama.cpp server documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md).

## Activity, liveness and cancellation

Provider activity is tracked immediately, independently of `[DONE]`. A stream
with a finish reason and clean HTTP end is valid even without `[DONE]`; a stream
without a finish reason is interrupted. Streaming `timeout_seconds` measures
absence of response bytes, not total generation duration. HTTP connection timeout
remains ten seconds. Cancellation is serviced from curl progress callbacks and
between stream/tool events on the existing single cognition thread.

The TUI redraws on its existing 50 ms event loop. Reasoning shows an in-place
`is thinking...`; public text shows typing; tool argument generation and execution
have separate activity phases. Commentary and bounded tool stdout/stderr remain
visible. Hidden reasoning never enters the UI queue. Public deltas are coalesced
and bounded while terminal selection pauses rendering. Required proof checks still
gate final completion claims.

Tests cover normalized streams, the exact 8,192-token regression, small-budget
continuation, fragmented and multiple calls, semantic progress, transient retry
without repeating a completed write, cancellation and crash recovery. Native HTTP
integration tests continue to exercise the actual curl/SSE/daemon/client path.

## Foreground efficiency

Compaction is a local handoff over immutable sources, not another model request.
Automatic learning runs during detached maintenance and consumes at most 40
unprocessed events, targeting an 8,192-token input presentation per batch
while retaining at least one bounded source event to make progress. Internal
requests still obey the normal input-budget checks.
A successful batch advances its source-event watermark; completion is recorded
only after all eligible source events have been consumed. Original events remain
unchanged, and deferred extraction can retry without losing them.

Identity, SOUL and the initial wake snapshot are frozen for the session, including
after checkpoints. Current tasks, research references and handoffs enter at the
tail. This preserves the longest available prefix without promising server cache
hits across compaction or unrelated clients.

The provider receives a fixed 29-tool working set for chat, research and coding.
The complete registry remains available through `tool_schema(names)` and
`tool_invoke(name, arguments)` without changing the native schema set each round.
Invocation uses the same validation, approval, cancellation and durable execution
path. Wrapped results retain the actual action's source event, with a separate
invocation event; duplicate call IDs replay the committed result. Optional
learning, prediction and self-reflection are demand-driven, not required rituals.

Research should close focused knowledge gaps using immutable passage references,
then proceed to implementation and observed verification. Independent tool calls
can share a generation, reducing provider round trips. Saga still executes them
in order on its cognition thread; this change does not add parallel side effects.
The existing evidence/risk gates and bounded search/turn budgets remain enforced.

The design follows the stable-prompt and tool-loop principles documented in the
[Hermes agent loop](https://github.com/NousResearch/hermes-agent/blob/main/website/docs/developer-guide/agent-loop.md)
and OpenAI's [latency guidance](https://developers.openai.com/api/docs/guides/latency-optimization)
and [prompt caching guidance](https://developers.openai.com/api/docs/guides/prompt-caching).
Performance comparisons require the same model, template, endpoint, cache state,
permissions and actual correctness checks; a shorter elapsed time alone does not
establish equivalent output quality.
