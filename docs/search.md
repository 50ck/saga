# Search backends

`WebResearch` asks `SearchOrchestrator` for normalized results. The orchestrator
uses a `SearchEngineRegistry`; each compiled adapter implements `SearchEngine::run`.
Adding a backend does not change the research verifier or source acquisition engine.
Responses use Saga's existing JSON value type, with `engine`, `query`,
`submitted_query`, `results` (title, URL, snippet, rank) and an optional opaque cursor.
Only these normalized results enter model context. Snippets are discovery data.
Source fetching and evidence evaluation are independent operations.

`SearchContext` carries cancellation/control servicing, runtime events, an injected
monotonic clock and an overall deadline. Search must be human initiated. There is
no background harvesting, recurring monitoring, CAPTCHA solving or browser spoofing.
The HTTP layer uses the honest `Saga/0.1 (public web research)` User-Agent, bounded
responses, public-address socket validation and redirect checks. Private instances
require an explicit operator configuration; directory-discovered instances always
remain subject to the public network policy.

## Routing and caching

`search_engines` sets the ordered backend list. An empty list preserves the legacy
`search_engine` as first choice and adds the other built-in engine as fallback.
DuckDuckGo remains first by default: API availability alone does not establish
better result relevance. Set `search_engines = ["fourget", "duckduckgo"]` to prefer
4get. Fallback is at query level, not fan-out to every backend.

Successful first-page results are cached in SQLite for ten minutes, keyed by
backend, query, limit and domain constraints. Errors are not result-cache entries.
Pagination tokens are omitted from cached responses because 4get tokens are
single-use. A cache hit never requires another API request. Backend degradation is
separate from result caching. There are at most two engine attempts and a 45-second
monotonic budget per query by default. Infrastructure events do not enter conversation
history; only normalized tool observations do.

## DuckDuckGo

The adapter consumes the HTML endpoint with Lexbor, extracts result headings and
snippets, filters domain constraints locally and normalizes result URLs. The lite
endpoint is a bounded fallback for server errors only. Every HTTP request, including
that fallback, passes through a shared production gate: starts are at least one
second apart, concurrent callers cannot bypass it, and waits service cancellation
and operator controls. Challenges and rate limits open a one-minute cooldown. Saga
switches backend instead of attempting to bypass protection.

## 4get

The official [API documentation](https://4get.ca/api.txt),
[instance directory](https://4get.ca/instances),
[probe source](https://git.lolcat.ca/lolcat/4get/src/branch/master/ami4get.php) and
[instance browser source](https://git.lolcat.ca/lolcat/4get/src/branch/master/instances.php)
are the protocol references. The server version inspected during implementation is
9. Compatibility is based on required schema, not exact version equality.

The directory parser reads origins from the Server table using the existing Lexbor
DOM parser. It ignores unrelated links, deduplicates canonical origins, rejects
credentials and non-root URLs, and excludes private destinations. The directory is
cached for six hours; a failed refresh can use retained seeds for up to 24 hours.
Configured manual origins are also supported. Discovery is bounded to 128 origins.

Health checks are lazy and serialized per pool: a search probes only candidates it
needs, never the entire directory. `/ami4get` must report `status: ok`, `service:
4get`, an integer version and explicit API/bot-protection capabilities. Automatic
selection requires `api_enabled: true` and `bot_protection: 0`. CAPTCHA, invite-only
and unknown protection modes are ineligible. Optional names and counters do not
control eligibility or ranking. Probe health expires after five minutes. Directory
refresh and probes are single-flight within the pool.

Eligibility excludes cooling-down and protected instances. Ranking favors recent
validated availability, observed search reliability and latency, with modest
stickiness for the successful instance. Manual instances receive configurable
priority. Failed candidates are tried once per query; bounded attempts rotate to the
next eligible instance. Failures open exponential cooldowns of one to eight minutes.
After cooldown, transient failures permit one lazy probe. Cancellation never counts
as an instance failure. SQLite stores directory and instance state; restored cooldowns
are bounded and converted to monotonic deadlines.

`/api/v1/web?s=...` performs the initial search using 4get's default scraper. HTTP
200 alone is insufficient: JSON must decode, `status` must equal `ok`, and `web`
must be an array. Null optional title/description fields are tolerated. Results are
normalized, deduplicated and domain-filtered; raw API payloads never reach the model.

The opaque `npt` token is bound to its instance, constraints and fifteen-minute
expiry. Continuation sends only `npt` to that instance. Failure requires restarting
the query; Saga never transfers the token to another instance.

## Configuration

The existing flat `config.toml` format is retained. Defaults:

| Key | Default |
| --- | --- |
| `search_engines` | `[]` (legacy first choice plus fallback) |
| `duckduckgo_min_request_interval_ms` | `1000` (minimum allowed) |
| `duckduckgo_challenge_backoff_ms` | `60000` |
| `search_cache_ttl_seconds` | `600` |
| `search_total_timeout_seconds` | `45` |
| `search_max_engine_attempts` | `2` |
| `fourget_directory_ttl_seconds` | `21600` |
| `fourget_probe_ttl_seconds` | `300` |
| `fourget_failure_backoff_ms` | `60000` |
| `fourget_max_instance_attempts` | `6` |
| `fourget_probe_timeout_seconds` | `3` |
| `fourget_request_timeout_seconds` | `10` |
| `fourget_manual_instances` | `[]` |
| `fourget_prefer_manual` | `true` |
| `fourget_allow_private_instances` | `false` |

The strongest configured DuckDuckGo spacing/backoff applies to the shared process
gate. Health is advisory and expires; a successful search is still required. Future
relevance/duplicate-rate metrics can extend ranking without changing research.

## Observability and verification

Runtime events include requested, queued, rate-limit wait, started, backend-selected,
instance-completed/failed, fallback, completed and failed. They carry query IDs,
backend, cache status, counts and durations. Instance details remain diagnostic;
the normal TUI displays the actual query and a concise backend-fallback message.
A new generation immediately displays a waiting state, then thinking on reasoning
deltas, without publishing private reasoning.

`tests/search_tests.cpp` uses synthetic HTTP responses and injected clocks for
spacing, concurrent gates, directory safety, capability checks, protected-instance
rejection, HTTP-200 errors, malformed JSON, circuit recovery, cancellation,
instance failover, single-flight operations, pagination affinity and result caching.
Public services are not queried by CTest.

## Research decomposition and verification

An explicit research request creates a pending `research_plans` record referencing
its user event and logical turn. It does not turn the operator's entire prompt into
a proposition. `research_plan` records intent, operator constraints, desired actions
and external goals, then creates granular claims. `research_goals` groups claims;
the existing `research_questions` table remains the backward-compatible claim store.
A concrete additional `research_question` creates its own external goal. Goal status
is derived from its claims: unknown, partially established, established,
not established or contradicted.

`research_plan` returns only its current plan, with explicit `plan_id`, `goal_id`
and `claim_id` fields. For explicit operator research, omitted goal `required`
inherits true; explicit false still denotes an optional goal, and at least one
required goal must remain. Proposed claims are unverified hypotheses, not facts.
The result no longer repeats every historical plan and the entire decomposition.

`goal_id` on web tools associates one search/read with all claims in that goal.
`question_id` selects a single returned `claim_id`; when both are supplied, the
claim must belong to the goal. IDs are never interpreted as ordinal numbers or
silently remapped across namespaces. `research_resolve.id` always names one claim.
Unknown or out-of-scope references fail before network/budget use and return a
structured `ResearchReferenceError` with available goal/claim references.
`research_status` retrieves the active reference catalog for recovery/resumption.

`question_id` on web tools associates queries and source reads with a claim. With
one pending claim, older calls can infer that association. With multiple claims,
association is explicit. Goal-level attempts are recorded for each selected claim,
without asserting that any of them has been verified. `research_attempts` stores outcomes separately from claim
status and source snapshots. A failed backend or search never evaluates a claim,
invalidates previously fetched evidence or marks every outstanding goal unverified.
Supported/contradicted conclusions require exact quotes from immutable fetched
documents; search snippets cannot serve as evidence. Evaluation remains the agent's
responsibility, recorded through `research_resolve` per claim. This validates
provenance and quoted passages, not mathematical truth or source authority.

Required claims need an associated search/read attempt before an explicit
unverified resolution. Disabled network access is reported separately. Reversible
low-risk work may proceed after disclosed uncertainty; required unverified claims
still prevent verified completion. Optional claims do not impose that gate. Normal
warnings contain short claim descriptions, never the entire operator request.
Intent, constraints and actions cannot be submitted verbatim as claims. Decomposition
is bounded to eight goals and 32 concise claims and is committed transactionally.
Legacy whole-request questions are retained as non-required audit records by the
schema migration.

The context builder and cognitive checkpoints carry goal/claim IDs and the
structured decomposition. Reference IDs survive generic context reduction.
Lookups consistently include session-level claims and the active task's claims;
unrelated tasks from the same session do not leak into reference validation or
resolution. Research created before task setup follows the new task just as it
follows later workspace changes. Existing personas, tool names, source snapshots and
research conclusions remain compatible. Infrastructure health, pacing and latency
stay in the runtime event journal, not model conversation history.

## Dispatch and workspace invariants

Tracing found no workspace callback that invokes `Runtime::chat` a second time.
The confirmed duplication was task setup: the runtime created a task before the
model, while its prompt still invited another `task_create`. Attention rebuilds
also carry operator content as data, which must not become a fresh user turn.

The client now supplies `user_message_id`; the daemon carries it through to a
unique `(session_id, user_message_id)` durable turn. Replays return the existing
turn status without dispatching it again; reusing an ID for different content is
rejected. Workspace changes do not alter that ID or create a turn. Missing IDs from
older clients inherit the request ID or receive a fresh ID for a new operation.

`tool_dispatch_keys` identifies `(turn_id, tool_call_id)`. A completed call returns
its durable result; conflicting arguments are rejected. An execution without a
durable result is uncertain and cannot automatically run again. Nested skill steps
are distinguished from their outer call. Repeated task setup in one turn reuses the
active task and merges proof obligations; reopening the active workspace is a no-op.
When existing workspace logic re-scopes a task after work began, the current
turn's research plans, goals and claims follow the new task ID. This prevents
completed side effects from repeating after request replay or crash.
There is no automatic replay of an interrupted turn.

`tests/research_tests.cpp` reproduces setup, workspace switching, three focused
Tetris claims, two DuckDuckGo successes, a challenge, 4get recovery, direct source
reads, granular evidence resolution, shell preflight and the next generation's
reasoning activity. It verifies one turn/task/user dispatch and retained evidence.
The separate failure regression proves a completely unavailable search can still
be followed by successful direct acquisition and claim verification.

The reference-contract regression seeds four legacy claims, creates three goals
with eleven new claims, and reproduces the observed `1/2/3` goal versus `5–15`
claim mismatch. It verifies default mandatory research, compact unambiguous output,
structured reference recovery, no network requests on invalid references,
goal-wide association, per-claim resolution, task binding, scope isolation and
cancellation without false attempt failures. All fixtures are synthetic.
