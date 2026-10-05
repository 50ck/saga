#!/bin/sh
# Initial source-only publication. Run on a host with writable Git metadata.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

if git rev-parse --verify HEAD >/dev/null 2>&1; then
    echo 'This helper is for the initial publication only; repository history already exists.' >&2
    exit 1
fi

# Public profile metadata only: never request or inspect a personal email address.
saga_git_email=$(curl --fail --silent --show-error --location \
    --connect-timeout 10 --max-time 30 https://api.github.com/users/50ck |
    python3 -c '
import json, sys
profile = json.load(sys.stdin)
if profile.get("login") != "50ck" or type(profile.get("id")) is not int or profile["id"] <= 0:
    sys.exit("GitHub account metadata did not match 50ck")
print(str(profile["id"]) + "+50ck@users.noreply.github.com")
')

git init -b main
if [ "$(git symbolic-ref --short HEAD)" != main ]; then
    echo 'Expected an unborn main branch; refusing to change an existing branch.' >&2
    exit 1
fi
if saga_remote=$(git remote get-url origin 2>/dev/null); then
    case "$saga_remote" in
        git@github.com:50ck/saga.git|https://github.com/50ck/saga|https://github.com/50ck/saga.git) ;;
        *) echo 'Origin points elsewhere; refusing to replace it.' >&2; exit 1 ;;
    esac
    # Use the SSH key for publication even if origin was initially HTTPS.
    git remote set-url origin git@github.com:50ck/saga.git
else
    git remote add origin git@github.com:50ck/saga.git
fi
git config user.name 50ck
git config user.email "$saga_git_email"
export GIT_AUTHOR_NAME=50ck GIT_COMMITTER_NAME=50ck
export GIT_AUTHOR_EMAIL="$saga_git_email" GIT_COMMITTER_EMAIL="$saga_git_email"

# Preserve GitHub's initial license commit without replacing local source files.
# A remote containing more files needs an explicit merge, not an automatic reset.
saga_main_ref=$(git ls-remote --heads origin main)
if [ -n "$saga_main_ref" ]; then
    git fetch origin main
    if [ "$(git ls-tree -r --name-only origin/main)" != LICENSE ]; then
        echo 'Remote main contains more than an initial LICENSE; reconcile its history first.' >&2
        exit 1
    fi
    if [ -e LICENSE ]; then
        echo 'A local LICENSE already exists; reconcile it with the remote license first.' >&2
        exit 1
    fi
    git reset --mixed origin/main
    git restore --source=origin/main -- LICENSE
fi

git add -- .gitattributes .gitignore AGENTS.md README.md CMakeLists.txt cmake include src tests third_party scripts
# Check the complete index, including files inherited from GitHub.
git ls-files -z | python3 -c '
import pathlib, sys
roots = {".gitattributes", ".gitignore", "AGENTS.md", "README.md", "CMakeLists.txt", "LICENSE"}
extensions = {"cmake": {".in", ".cmake"}, "include": {".hpp", ".h"},
              "src": {".cpp"}, "tests": {".cpp", ".hpp", ".py", ".md"},
              "third_party": {".h", ".hpp"}, "scripts": {".sh"}}
for raw in sys.stdin.buffer.read().split(b"\0"):
    if not raw:
        continue
    path = raw.decode("utf-8")
    p = pathlib.PurePosixPath(path)
    if path in roots or path in {"src/schema.sql", "third_party/nlohmann/LICENSE"}:
        continue
    if p.parts[0] not in extensions or p.suffix not in extensions[p.parts[0]]:
        sys.exit("Refusing to publish a staged file outside the source allowlist")
print("Verified source-only staging; no persona files, configuration or databases.")
'
git diff --cached --check

# Verify BOTH effective identities immediately before the commit. Capture
# unexpected identities without printing them, even when local Git config differs.
python3 - "$saga_git_email" <<'PY'
import re, subprocess, sys
expected = re.escape("50ck <" + sys.argv[1] + ">")
for identity in ("GIT_AUTHOR_IDENT", "GIT_COMMITTER_IDENT"):
    value = subprocess.check_output(["git", "var", identity], text=True).strip()
    if not re.fullmatch(expected + r" \d+ [+-]\d{4}", value):
        sys.exit(identity + " differs from the verified GitHub noreply identity")
    print(identity + ": 50ck <" + sys.argv[1] + "> (verified)")
PY
git commit -m "Implement Saga persistent agent runtime and ncurses client"
git push -u origin main
