# Saga

**The model thinks. The runtime remembers. The agent persists.**

Saga is a C++23 runtime for persistent personal agents. Each persona has its own
identity seed, SQLite database, autobiographical memory, knowledge, procedures,
goals, evidence and unfinished work. Replacing the model preserves that state.
Persona selection lives outside cognition.

**Memory gives it a past. Goals give it a future. Attention gives it a present.**

`saga` is the interactive ncurses client; `sagad` owns sessions, cognition, tools
and persistence. They communicate over a private Unix socket. The only model
backend is an OpenAI-compatible HTTP API, including servers such as llama.cpp.

## Build and run

Linux, a C++23 compiler, CMake, SQLite with FTS5/JSON support, libcurl, wide
ncurses and MD4C are required. Web research requires libcurl 7.85+
with asynchronous DNS support. Prefer the distribution's development packages
(for Markdown, `libmd4c-dev` on Debian/Ubuntu or `md4c` on Arch Linux).
Stable C ABI declarations support SQLite, curl, ncurses and MD4C systems with only
the shared libraries. Lexbor 3.0.0 is vendored and linked statically; no libxml2
or web browser dependency is required.
The nlohmann JSON header is vendored with its MIT license.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/saga
```

After the persona menu, ncurses opens a full-screen chat with framed messages,
streamed replies, tool commands and elapsed times, and a fixed model/context/task
status bar above the input. Terminal resizing reflows the transcript immediately,
including Unicode display widths. Use the mouse wheel or Page Up/Down to scroll
the chat history; the status bar and input stay fixed. Use arrows/Home/End to
edit, Ctrl-U to clear, and Escape or `/exit` to leave. Approvals appear in chat;
type `y` or `n` and Enter. `--no-tui` selects line chat explicitly, and redirected
input also uses line chat. There is no Python, Rust, Node or browser runtime requirement.

Press F2 to select and copy using the terminal's normal mouse drag and clipboard
shortcut. This disables mouse reporting and freezes display updates while model
events continue to be received. F2 or Escape returns to chat and applies queued
updates; the draft input is preserved. Page Up/Down can move through the chat
history in selection mode. Terminals such as xterm also support Shift-drag without
changing modes. Mouse wheel scrolling is restored when leaving selection mode.

Approval borders are yellow, Saga borders dark gray, and tool activity light gray.
Approval arguments use JSON syntax colors: bold cyan keys, amber strings, cyan
numbers, purple booleans/null and blue punctuation. A `command` string uses the
same shell highlighting as tool activity, including its JSON-escaped quotes,
newlines and Unicode. The displayed JSON and submitted arguments remain unchanged;
colors follow the text when the terminal resizes.
Shell commands have a muted blue prompt and lexical syntax highlighting. `/help`
uses parsed Markdown with bold blue section headings, bold cyan commands, light
gray explanations and highlighted examples. The terminal size comes from the
attached TTY; resize forces a complete repaint to remove stale text.
Assistant
Markdown is rendered as it streams: headings, bold/italic/strikethrough, cyan inline
code on gray, links, image descriptions, fences, quotes, nested lists, task lists
and aligned tables. Code contents remain literal; shell and common programming
tokens are highlighted. Fence info can specify a title and a highlighted line
(`javascript {6} title="Example.js"`). Images display their alt text and caption;
links display their styled label without fetching resources. Single underscores
use standard Markdown italics; triple tildes additionally support inline strikes.
While streaming, unfinished inline styles, code and links receive provisional
formatting, then the completed response is parsed from its original Markdown.
Ambiguous constructs can change appearance as more text arrives; stored messages
are never modified. Unchanged bubbles are cached; resizing rebuilds layout.
Strikes use a combining stroke through the text, and italic appearance depends
on terminal support. Public `report_progress` commentary streams separately from completion claims; commands stream bounded stdout/stderr while running. Empty model replies get one recovery attempt and then a visible error, never an empty bubble. `/steer PROMPT` queues instructions for the next model call in the same session, after the current command and approval finish. `/stop`, Ctrl+C or Esc interrupt active work and cancel queued steering while preserving completed changes.

`/help`, `/status`, `/permissions`, `/web` and inspection commands
remain available while the agent waits for the model, streams a reply, runs a tool
or waits for approval. This includes `/memory`, `/know`, `/praxis`, `/artifacts`,
`/journal`, `/goals`, `/tasks`, `/self`, `/project`, `/state`, and `/name` or `/soul`
without editing arguments. Permission changes are saved immediately and apply to
subsequent host actions and structured edits. An already pending approval still requires `y` or `n`;
changing the policy does not approve it or change a running command's isolation.
Session changes, identity edits, knowledge edits and `/compact` wait until the
current turn finishes. Local command errors leave inference and approvals intact.

The client starts `sagad` automatically. Run `./build/sagad` in the foreground
to supervise it yourself. `cmake --install build --prefix PREFIX` installs both
executables together. Run Saga from a repository or a separate work directory;
the project must be outside Saga storage and must not contain it as a descendant.

## Setup

Every interactive start asks you to select a persona. No persona is selected
automatically. First-run creation offers **Use defaults**, which creates
`Assistant` with a neutral SOUL. Custom creation asks only for a name and an
optional SOUL.md file. Names support spaces and Unicode. All settings can be
changed later.

Model setup asks for an endpoint and an optional API key. It discovers models,
offers a selector when needed, detects context from serving metadata or `/props`,
without sending test completions or tool calls. Unknown context requires
a manual value. Context below 65,536 tokens rejects setup.
Both bare endpoints and endpoints ending in `/v1` work. `SAGA_API_KEY` overrides
the configured key; an empty key sends no Authorization header. TLS verification
is enabled. Small contexts and insecure TLS require explicit advanced overrides.

Setup displays model discovery and effective context detection. Streaming and
function calling are used during actual work; setup does not claim to have
verified them. Normal conversations retain the model's reasoning behavior.
HTTP inference honors `timeout_seconds`
(default 600), without a separate 60-second low-speed cutoff. For a slow local
model, increase this advanced setting in `config.toml`; backend setup preserves it.

## Commands

| Command | Effect |
| --- | --- |
| `/help`, `/help COMMAND` | Descriptions grouped by section, or detailed examples |
| `/status` | Identity, SOUL path, context progress, task, memory and permissions |
| `/permissions`, `/permissions 1`, `2`, `3`, `4` | Choose guarded host or unrestricted host, with or without approval |
| `/compact` | Save typed cognition and a handoff, then rebuild working context |
| `/new` | Close/consolidate this session and start another |
| `/persona` | Close the session and return to the selector |
| `/model` | Close the session and configure the global backend |
| `/name`, `/name NEW NAME` | Show or deliberately change your persona's display name |
| `/soul`, `/soul FILE` | Show the seed or deliberately replace it with your file |
| `/memory QUERY` | Deep autobiographical recall |
| `/know QUERY` | Semantic knowledge, including temporal history |
| `/praxis QUERY` | Procedures and relevant failed strategies |
| `/artifacts QUERY` | Created or modified files |
| `/journal`, `/goals`, `/tasks` | Inspect persistent records |
| `/self`, `/project`, `/state` | Inspect learned state and continuity |
| `/fact JSON`, `/correct JSON` | Record explicit knowledge or a correction |
| `/exit` | Close the session and exit |

```text
/fact {"subject":"workstation","predicate":"operating_system","object":"Void Linux"}
/correct {"subject":"workstation","predicate":"operating_system","object":"FreeBSD"}
```

Facts can also use `"scope":"project"` to keep knowledge local to the current
project. Corrections preserve the old fact, close its validity interval, and record the
replacement with provenance. Automatic SOUL writes are forbidden; learned
tendencies belong to the self-model. Seed creation and deliberate edits are
preserved in the persona's event history.

Sandboxed shell commands run automatically. Structured file edits and workspace
selection follow the same approval choice as host shell operations. Permissions
are persisted separately for each persona:

| Option | Alias | Host operations and structured edits |
| --- | --- | --- |
| 1 (default) | `/permissions ask` | Ask before guarded host operations, edits and workspace changes |
| 2 | `/permissions always` | Automatically approve guarded host operations, edits and workspace changes |
| 3 | `/permissions host-ask` | Ask before unrestricted host operations, edits and workspace changes |
| 4 | `/permissions host-always` | Unrestricted host, edits and workspace changes without approval (**DANGEROUS**) |

Use `/permissions 3` or select **3** in the menu to approve sandbox actions
automatically and ask before real host commands. Option **4** runs those host
commands automatically. Existing `ask`/`always` settings keep their guarded behavior;
they are never silently upgraded to unrestricted access. Existing settings now
also determine whether structured file edits and workspace changes need approval. Persistent macros
containing host actions follow the same policy; sandbox macros are automatic.
Non-interactive batch clients decline pending
approval requests. `saga --batch UUID` is an
explicit selection for scripts; `--json` prints protocol events.


### Live coding workflow

The persona can publish concise public commentary with `report_progress` before
an action group or after a discovery. Saga renders these messages as they stream;
private reasoning never becomes commentary. Preparing a prompt, generating,
preparing an operation, running, awaiting approval and verifying are distinct
activity states with an elapsed clock. Empty or reasoning-only replies get one
recovery attempt, followed by a visible error if there is still no response/action.

`file_write` proposes a complete creation/replacement diff. `file_edit` takes
`path`, `expected_hash`, `description` and exact `old_text`/`new_text` replacements;
each old fragment must occur once and replacements must not overlap. Text files
up to 512 KiB are supported. In modes 1/3, a yellow approval popup shows the complete
proposed diff before any write, initially collapsed to ten rendered rows. Click
**See more/See less**, or use Tab and Enter. Use the mouse wheel or Page Up/Down to
scroll an expanded proposal. Approve/Reject also accept the existing `y`/`n` input.
Inspection commands temporarily reveal the transcript beneath a pending review;
Tab returns to its popup without approving it. Saga rechecks the original hash
and path after approval. A changed file is left
untouched and requires a fresh proposal. Artifact versions are recorded only after
the write. In modes 2/4, edits are automatic and their transcript diffs also start
collapsed to ten rows, with clickable expansion. Additions are green; deletions
are red. Collapsed diffs retain small previews in the client; expanding loads the
complete diff from this persona's event history, including while the agent works.
F2 coalesces token/progress updates and bounds live output previews while keeping
final outcomes. Resizing recomputes wrapping and click targets while keeping the footer.

Shell commands stream stdout/stderr in roughly 100 ms batches, with 256 KiB capture
limits per stream and a visible truncation notice. Shell approval covers the
command; it cannot predict arbitrary file changes. Saga shows observed text diffs
after the command finishes, within a 2 MiB snapshot budget. Use structured file
tools for source edits and reviewed diffs.

Use `/project PATH` or the agent's `project_open(path, create)` tool to select an
explicitly requested workspace. Selecting a project never exposes private Saga
storage. An untouched task can follow the new workspace. Once work has been
observed, Saga keeps its previous provenance and creates a new scoped task with
unresolved checks rather than carrying proof from another project.

`/steer PROMPT` queues instructions in the current session. Resolve any pending
approval first; the current approved command finishes, then queued instructions
are delivered in order before the next model call. Remaining unstarted calls
from the old plan are cancelled. `/stop`, Ctrl+C or Esc cancel inference, approval,
and the running command's process group. Completed edits stay, unfinished tasks
become blocked, and queued steering is cancelled rather than resumed. The session
stays open. These stop keys also work during F2 selection/copy mode. Inspection
commands and permission changes remain available during work.

## Persistence and cognition

```text
$XDG_CONFIG_HOME/saga/config.toml
$XDG_DATA_HOME/saga/registry.db
$XDG_DATA_HOME/saga/personas/<uuid>/SOUL.md
$XDG_DATA_HOME/saga/personas/<uuid>/agent.db
$XDG_DATA_HOME/saga/personas/<uuid>/cache/
$XDG_DATA_HOME/saga/personas/<uuid>/artifacts/
$XDG_STATE_HOME/saga/logs/
$XDG_RUNTIME_DIR/saga/sagad.sock
```

Unset config/data/state variables use standard home fallbacks. An unavailable
runtime directory uses `/tmp/saga-<uid>/saga/`. Directories use 0700; configuration,
databases, locks, logs and sockets use 0600. The registry contains only selection
metadata. Each persona has an exclusive context lock and an independent database.

Databases use WAL, foreign keys, full synchronization, atomic migrations and
explicit schema versions. Version 2 adds project-scoped facts; version 3 adds
persona permission settings, context checkpoints and durable steering messages. Existing databases migrate
without deleting identity or history. Triggers reject updates/deletions of life events.
Derived memory may change while its sources remain immutable. FTS5 BM25 retrieval
combines lexical, entity, project, goal, salience, recency, confidence and
accessibility signals, with configurable weights. Forgetting reduces accessibility
instead of deleting history. Separate tables preserve the different memory and
epistemic categories.

The executive retrieves memory for past references, recalls praxis for work,
reconsiders unexpected results, and gates completion on required checks and
high-impact assumptions. Important or historically overconfident work receives
a fresh-context review. Disagreement keeps the task in verification. Evidence is
deduplicated by source; model confidence remains separate from evidence confidence.
Recorded prediction outcomes update domain calibration in the self-model.

Session closure creates a first-person diary and episode through a validated
function call during detached maintenance, then extracts supported facts and
procedural candidates. Closure writes a factual provisional diary immediately
for later refinement. Crash recovery
preserves abandoned sessions and writes provisional diaries locally on wake.
Selection, session switching and attached maintenance never generate model
completions. Detached daemon maintenance performs pending reflection and memory
extraction later. Wake restores recent
life, project state, tasks, commitments, goals and open loops.
New sessions keep unfinished tasks available as continuity rather than activating
one automatically. The footer displays the current task; an earlier task appears
there only after it is selected again to resume work. A daemon scheduler
maintains inactive identities, retries reflection/extraction, calibrates confidence
and queues restrained reminders. Attached TUIs can receive idle notifications.

Praxis uses Beta success/failure accounting and experience thresholds: two
successes for learned, five uses with four successes for validated, and ten uses
with a strong record for habitual. Promotion requires actual task outcomes and
passed checks. Habitual procedures can become approved tool macros; their actions
still pass the action gate.

Default context reserves 8,192 tokens for generation and 2,048 for safety. Without
a model tokenizer, the builder uses a conservative byte upper estimate, leaving
capacity unused. For detected llama.cpp endpoints, Saga requests `timings_per_token`
and `return_progress` on the normal chat-completions stream. Context uses
`prompt_n + cache_n + predicted_n`, including reused cache tokens. Prompt progress
can report the full input count before generation. Exact streamed counts update
the UI at most every 150 ms; no `~` is shown once both counts are available.
While generating, a separate `+N` shows generated tokens individually so progress
remains visible between changes in the rounded K total (`+~N` for estimates).
The typing indicator appears only once the current stream has generated tokens;
connection and prompt processing show “is preparing context...” beforehand.
If a server omits those fields, the UI estimates input plus generated content,
reasoning and tool arguments and marks it with `~`. Final standard usage is
authoritative and can correct an estimate. Internal review/extraction calls keep
their own metrics and do not replace the displayed conversation context.
`/status` shows prompt tokens actually reused from cache when the server reports
them; those measurements are also recorded as `model.cache_observed` events.
The same Saga session remains active between messages. Each OpenAI-compatible
request sends its working conversation again; a server generation task is not a
new persona session. Identity and the wake snapshot stay stable until compaction
or an explicit identity/session change. Live attention and task checks are appended
as persisted internal data messages, keeping previous turns unchanged for prefix
cache reuse. Tool excerpts use a consistent limit, and streamed reasoning is
preserved in assistant history when supplied by the server. Server cache reuse
still depends on its model, chat template and available cache checkpoints.
Detected llama.cpp requests explicitly enable `cache_prompt` and
`chat_template_kwargs.preserve_thinking` without disabling normal reasoning.
The client checks the daemon's runtime revision at startup and reports when an
older running daemon must be restarted to load context or permission updates.
Each compaction displays
`xN` in the status bar and `/status`, omitted before the first compaction.

Before automatic or manual compaction, memory extraction persists supported facts
and procedural candidates in their own tables. If the backend is unavailable,
original events and existing typed cognition still persist for later consolidation.
A transactional checkpoint references all typed cognitive records and saves the
objective, latest request, decisions, evidence sources and unfinished checks.
The new context receives this handoff alongside the same SOUL and current typed
state. Repeated compaction without new messages does not create another checkpoint.
Compaction preserves
assistant/tool groups and retains complete observations in the event record;
excerpts carry provenance. Long tool turns archive older complete exchanges and
persist a working summary; `recall_observation` can retrieve their original
observations by event ID. Oversized user inputs or a single oversized latest
exchange are refused.

## Tool boundaries

Shell execution requires Linux Landlock ABI 3 or newer plus seccomp. In default
`execution="sandbox"`, children can access the project, writable private HOME/TMPDIR
and system programs. They cannot
read persona databases, registry or configuration, access other home directories,
connect to sockets, signal unrelated processes, or inherit backend credentials.
Unsupported kernels refuse shell execution. File tools reject paths outside the
project/artifact area, symlinks and hard links. The shell guard also rejects
hard-linked workspaces. These boundaries are enforced independently of prompting.

For desktop notifications, tmux, network access or host files, the model can request
`shell_exec` with `execution="host"`. The selected permission mode determines what
host means. Options **1/2** use a whitelisted desktop environment and retain
Landlock/seccomp guards on Saga storage and `/proc`. Because Landlock permissions
are inherited through directories, creating files directly in an ancestor of Saga
storage (often the home directory) can be denied even after approval. Changing
approval policy alone does not remove those guards.

Options **3/4** run a normal child process as the daemon's OS user, inheriting its
environment plus the client's desktop settings. Saga adds no Landlock, seccomp,
`no_new_privs` or sandbox resource limits. These commands can write directly in
the home directory and access every file or process that user can access, including
Saga configuration, credentials and other persona storage. OS-enforced persona
isolation therefore applies only in guarded modes. The model is still instructed
not to inspect other identities or modify SOUL automatically.

All shell tools capture output, close inherited descriptors, use noninteractive
stdin, enforce the requested timeout (up to 120 seconds) and clean up their process
group. Host mode is not an interactive terminal emulator and cannot remove
restrictions inherited from the OS or an outer container. `sudo` follows the host's
normal policy and can require an interactive terminal/password. Unrestricted host
does not automatically grant root privileges. Backend credentials are stripped
from guarded command environments; unrestricted execution inherits the host
environment as requested.

File writes track artifacts directly. Shell actions observe created/modified
project files and removals. Tracking excludes `.git`, `build`, `node_modules` and
`.cache`, with a 10,000-file / 256-change ceiling reported in the tool result.
Files over 8 MiB receive marked metadata fingerprints. Binary reads use base64;
process output records UTF-8 normalization, truncation and original byte counts.
Environment perception includes the clock, machine, git state and project-scoped
processes, without logging git author or committer emails.

## Protocol and validation

The private Unix socket uses newline-delimited JSON with `type`, `request_id`
and `payload`. Peer credentials must match the daemon user. Clients first authenticate
using the daemon's 0600 `control.token`; host tools cannot read that token. Restart
an older daemon after upgrading to this protocol. Control requests
include `personas.list`, `personas.create`, `backend.models`, `backend.setup`,
`persona.activate`, `chat`, `command` and `session.close`. Persona registry
operations are unavailable while that connection hosts cognition. Cognitive
tools receive only the active `PersonaContext`.

Streaming events include assistant deltas, context usage, cognitive modes,
tool lifecycle events, approval requests and notifications. Approval must match
both the request ID and approval UUID. `result` or `error` completes a request.
`saga --request` reads one control request from stdin and prints its events.
Mutable workflows use an interactive client or an approval-aware protocol client.

```json
{"type":"personas.create","request_id":"example","payload":{"name":"Researcher"}}
```

Release tests cover persistence, FTS, temporal facts, isolation, proof gates,
praxis, context limits, process sandboxing and anonymous IPC. Native runtime tests
exercise fragmented streamed tools, approvals, diaries, model replacement, persona
switching, offline consolidation, fresh review and crash recovery. A connected Unix
socket test exercises the daemon's request handler, session switching and identity
isolation. The HTTP/daemon/CLI suite uses a native local mock OpenAI server.
Environments denying local TCP sockets report that suite as **skipped**.

The optional development check `python3 tests/pty_chat.py` exercises ncurses in a
real pseudo-terminal, including styles, approval Enter, local commands and resize.
Python is not required by Saga or its CTest suite.

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSAGA_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Run the full suite on a Linux host permitting local sockets to validate the
listener and HTTP transport. Sanitizer builds require the compiler's ASan and
UBSan runtime libraries. Desktop notification delivery also requires a working
user DBus session. Routing and evidence interpretation use heuristics plus the
reasoning model; proof gates enforce provenance and required checks rather than
mathematically proving arbitrary goals.

## Repository and personal data

This repository contains code and synthetic test fixtures. Personas, SOUL files,
API keys, databases, conversations, logs and generated artifacts belong to local
XDG storage and must never be committed. `.gitignore` also excludes these files
if they are created inside the checkout. `src/schema.sql` contains the initial
schema and empty runtime defaults; it contains no personal records and is required
to build Saga.

For the initial publication to `50ck/saga`, run `sh scripts/publish.sh` from a
normal terminal with Git, SSH, curl, Python 3 and GitHub access. Python is used only
by this development helper. It obtains the account's public numeric ID from
GitHub, configures its GitHub `noreply` identity, stages an explicit source list,
verifies both commit identities, creates the initial commit and pushes over SSH.
It preserves GitHub's initial LICENSE commit, refuses other existing source
history or a conflicting remote, and never force pushes. Future changes should be
committed after the relevant checks pass, with
author and committer identities verified before every commit.

## License

See [LICENSE](LICENSE) for the project's license. The vendored nlohmann JSON
header retains its own [MIT license](third_party/nlohmann/LICENSE).

## Public web research

Saga provides native `web_search`, `web_fetch` and `web_read` tools, independent of shell and
host permissions. Public read-only access is enabled automatically for each
persona. `/web` shows the engine and current budgets; `/web off` cancels an active
web request and prevents subsequent native network operations. `/web on` enables
it again. These controls, `/status`, `/help`, steering and cancellation work while
research is active. Disabling native access does not change host shell permissions;
the cognitive prompt forbids bypassing that decision through shell commands.

DuckDuckGo is the first engine. It uses the public non-JavaScript HTML interface
and falls back once to Lite for a server error or missing endpoint. These are
website interfaces, not a guaranteed search API: markup can change, queries can
be throttled, and bot challenges can interrupt access. Saga reports challenges,
rate limits, unrecognized markup and empty results distinctly and does not solve
CAPTCHA or silently switch engines. No API key or browser is required.

Search accepts quoted phrases, `site:`, `-site:`, `intitle:`, `inurl:`, `filetype:`,
and term emphasis/exclusion. The adapter supplies engine-specific strategy guidance
and enforces explicit domain constraints against result hostnames. DuckDuckGo
can return related matches or imperfect operator results, so the agent must read
sources and verify its constraints. Bangs and first-result redirects are unsupported.
See [DuckDuckGo search syntax](https://duckduckgo.com/duckduckgo-help-pages/results/syntax).

The cognitive cycle is: recall relevant knowledge, identify a concrete gap,
search, read primary documentation, compare sources, record findings, implement,
and verify through observed execution. Explicit research requests and recorded
critical external assumptions create persistent research requirements. The runtime
requires an attempt and an accounted outcome before implementation. This is a
bounded safeguard, not a guarantee that the model will recognize every knowledge
gap or correctly interpret every source. Stable trivial questions do not require
searching merely because they are absent from memory.

`research_question` records a gap; `research_resolve` requires exact passages from
fetched document snapshots for supported or contradicted findings. Failed research
is disclosed before reversible work continues. Critical unverified or contradicted
research blocks verified completion; high-risk implementation stays gated. A search
snippet cannot substantiate a researched conclusion, and documentation cannot pass
an implementation/execution proof obligation. Explicit research checks use
`task_add_check` with `kind: "research"` and a quoted document passage. Learned
procedures remain candidates until experience validates them.

Each persona owns its immutable source snapshots, research questions, source
URLs, retrieval timestamps, content hashes and excerpts. Compaction preserves
references and research conclusions; `web_read(source_id)` retrieves stored
passages even with network access disabled. `refresh: true` creates a new snapshot.
Repeated evidence from the same source URL does not increase confidence as though
it came from independent sources. Search and page content remain untrusted data,
never instructions to change permissions or execute commands.

V1 reads HTML and plain text, including headings, lists, code and tables. PDF,
authenticated sites and JavaScript-dependent content are unsupported. Requests
use verified TLS, public HTTP/HTTPS destinations, address checks at connection
time, validated redirects, no ambient proxies, and no model credentials. Each
request has a 30-second deadline, five-redirect limit and 2 MiB decompressed body
limit. Canonical content is bounded to 256 KiB. `web_fetch(url, query,
max_output_tokens)` selects source sections locally, defaulting to 2,048 and
allowing up to 4,096 estimated tokens. `web_read` also accepts a stored source ID
and a focus query. Raw HTML, HTTP responses and API payloads never enter the
model context. Older snapshots retain compatibility with byte-based excerpts.

Global configuration additions in Saga's `config.toml`:

```toml
search_engine = "duckduckgo"
web_search_limit = 4
web_read_limit = 8
web_output_tokens = 4096
web_allow_private_network = false
```

These are per-turn network budgets, in addition to the existing tool-round budget.
Cached source excerpts do not consume page-fetch budget. A search defaults to five
results, with a maximum of ten. Changing the selected engine requires restarting
the daemon; additional engines implement `SearchEngine` and register in
`make_search_engine`. HTTP transport, document extraction, cognitive tools and
persistence are shared. No dynamic plugin loader is required.

The deterministic web tests use synthetic HTML and a mocked HTTP transport,
including a research-to-compile-and-execute workflow. CTest never requires live
DuckDuckGo. Optional live diagnostics (built with `BUILD_TESTING=ON`):

```sh
./build/saga_web_smoke 'site:duckduckgo.com "advanced syntax"'
./build/saga_web_smoke --read https://duckduckgo.com/duckduckgo-help-pages/results/syntax
```

These commands contact the public Internet and return a nonzero status on a
challenge, access failure or parsing failure. They do not load any persona data.

## Deterministic document acquisition

`WebAcquisitionEngine` is compiled into Saga's normal runtime. All sources produce
`CanonicalDocument` before the agent sees content:

```text
HTTP → representation inspection → instance detection → API/raw or Lexbor HTML
→ typed blocks → sanitization → normalization → deduplication
→ local BM25F section selection → compact Markdown with provenance
```

The public interface is in `include/saga/web/acquisition.hpp`; implementation is
in `src/web/`. It reuses Saga's exception-based errors, curl transport, JSON and
persona-owned SQLite connection. Platform handlers share composed helpers in
`sources.cpp` rather than a separate class hierarchy or plugin registry.
GitHub/GitHub Enterprise, GitLab, Forgejo/Gitea, Discourse and
MediaWiki handlers prefer public APIs. Forge handlers support repository, file,
directory, issue, review, commit and release routes. Review diffs are fetched
when the focus query requests a diff or patch. API failures fall back to an
already downloaded textual page. Self-hosted instances use weighted metadata,
asset and structural signals, evidence-derived mount paths and bounded JSON
probes. Ordinary articles incur no platform probes.

Lexbor parses HTML once. A block graph preserves headings, paragraphs, lists,
definitions, quotations, code, tables and captions. Prose, documentation,
discussion, reference and listing profiles use centralized readability, density,
semantic, continuity and template weights. Strict, balanced and recall passes
are evaluated against coverage, extracted size, link density and boilerplate.
Scripts and hidden controls are removed; navigation, sidebars and footers receive
penalties. Warning asides and related source sections remain available. Markdown
is parsed directly with the existing MD4C dependency. Unicode NFC is supplied
by Lexbor; iconv handles declared charsets. Code and diffs preserve whitespace.

Technical tokenization retains flags, qualified identifiers, filenames and
manual-page names. BM25F weights titles/headings above prose, expands useful
section context, and selects complete semantic units within a conservative
token estimate. Large focused code blocks use complete source-line windows;
large focused tables select source rows. Extraction and reduction never use
an LLM, embeddings, subprocesses or JavaScript execution.

Persona-local SQLite tables cache HTTP bodies/validators, instance detections,
canonical documents, content fingerprints and site-template observations.
Conditional GET honors ETag and Last-Modified. HTTP and document caches are
bounded to 128 entries; source snapshots remain immutable life evidence.
Opaque search-pagination state is also cached locally behind short cursor IDs,
instead of being passed through the model context.
Template repetition increases a penalty rather than erasing historical content.
Structured extraction diagnostics and fallback reasons are retained separately
from the compact agent view.

Default limits: 2 MiB decompressed responses, five redirects, ten-second connect
timeout, thirty-second per-request deadline, 256 KiB canonical documents,
64 KiB code blocks, 100 table rows/posts, twelve table columns, 8,000 semantic
blocks, 100,000 DOM nodes and depth 128. `WebLimits` centralizes internal limits.
The advanced `web_allow_private_network` setting permits LAN acquisition; its
default is false. TLS verification stays enabled, ambient proxies and model
credentials are excluded, and resolved socket addresses and every redirect are
checked. Diagnostic URLs omit query strings.

The TUI uses `•` activity markers, displays the submitted search query and shows
`Visited title (URL)` for completed reads. Context accounting now shares one
conservative estimator between its input guard and live display. Nested tool
observations receive bounded context projections; full observations stay in
append-only events. This fixes premature budget errors caused by counting each
byte as one token while displaying roughly one token per three bytes.

Tests include a synthetic regression corpus in `tests/web/corpus`, platform/API
mocks, sanitizer/encoding/tokenizer/retrieval checks, cache and SSRF boundaries,
context-budget regressions and deterministic mutation smoke tests. A native local
HTTP test checks redirects, conditional requests, response limits and negotiation;
it skips when the environment forbids local sockets. Run:

```sh
ctest --test-dir build --output-on-failure
./build/saga_acquisition_tests --benchmark
cmake -S . -B build-asan -DSAGA_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Sanitizer builds require installed compiler ASan/UBSan runtime libraries and also
instrument the vendored HTML parser. Fixtures are synthetic; no persona data or
real conversations are included.

Known limits: no authenticated acquisition, PDFs, browser challenges or JavaScript
rendering. Dynamic shells fail explicitly. Forge routes assume one URL segment
for a branch/ref unless the segment is percent-encoded; ambiguous branch paths
fall back to HTML. API collection pages are bounded; exhaustive pagination is
not claimed. Generic discussion pages retain source paragraphs unless reliable
author/reply structure is supplied by a platform API. Extraction weights are
heuristics calibrated against the synthetic corpus, not guarantees for every
site layout. Conservative token estimates may reduce context utilization.
