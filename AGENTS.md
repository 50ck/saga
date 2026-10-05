# Privacy

- Never use or expose the user's personal email address in Git commits, tags,
  patches, logs or pushes. Before every commit or tag, verify both author and
  committer identities and use the user's GitHub-provided `noreply` address unless
  the user explicitly supplies another address for that operation.
- Publish source code, build definitions, documentation and synthetic test
  fixtures only. Do not commit persona SOUL files, configuration, credentials,
  databases, database dumps, conversations, logs or generated artifacts.
- `src/schema.sql` is the application schema, not a database dump; keep it tracked.

# Changes

- Use English for code, application text, documentation and commit messages.
- Make a commit after each logical modification, once its relevant checks pass.
- Use a clear commit message describing the resulting behavior. Never claim a
  commit or push succeeded without checking the command's result.
- If Git metadata is read-only or network access is unavailable, finish the
  authorized local work and report the restriction without bypassing it.
