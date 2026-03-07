# hep

version control. 92 commands. competing with bro (1 command).

build: `gcc -Isrc -Isrc/core src/*.c src/core/*.c -lz -o hep`
requires: `zlib` (`apt install zlib1g-dev`)

run `hep bios` for full command listing.

---

## wave 1 — core

| command | does | git equiv |
|---|---|---|
| `hep init` | start repo | `git init` |
| `hep print <file\|.>` | stage files | `git add` |
| `hep wave -m "msg"` | commit | `git commit` |
| `hep spy` | full log | `git log` |
| `hep compete` | diff staged vs HEAD | `git diff --cached` |
| `hep light` | status | `git status` |
| `hep expand [name]` | list/create branch | `git branch` |
| `hep travel <branch>` | switch branch | `git checkout` |
| `hep chiplets <branch>` | merge | `git merge` |
| `hep stl <url> [dir]` | clone | `git clone` |
| `hep send [remote]` | push | `git push` |
| `hep dock [remote]` | pull | `git pull` |
| `hep interface <commit> <file>` | show file at commit | `git show` |
| `hep search <pattern>` | search | `git grep` |
| `hep hall` | stash | `git stash` |
| `hep retrieve` | apply stash | `git stash pop` |
| `hep group [name]` | list/create tag | `git tag` |
| `hep microscope <hash>` | inspect object | `git cat-file` |
| `hep earth <file>` | untrack file | `git rm` |
| `hep house <key> [val]` | config | `git config` |
| `hep kill <commit>` | reset hard | `git reset --hard` |

## wave 2 — extended

| command | does | git equiv |
|---|---|---|
| `hep mean <commit>` | cherry-pick | `git cherry-pick` |
| `hep short` | one-line log | `git log --oneline` |
| `hep close <branch>` | delete branch | `git branch -d` |
| `hep secret [out.tgz]` | export archive | `git archive` |
| `hep change <old> <new>` | rename branch | `git branch -m` |
| `hep accuse <file>` | blame | `git blame` |
| `hep discord -m "msg"` | amend last commit | `git commit --amend` |
| `hep window <a> <b>` | diff two commits | `git diff <a> <b>` |
| `hep what <string>` | find introducing commit | `git log -S` |
| `hep bd <f1> [f2...]` | bulk untrack | `git rm` (multi) |
| `hep power` | verify all objects | `git fsck` |
| `hep hotel` | repo stats dashboard | — |
| `hep wpm` | word/line/char count | — |
| `hep gnome` | list untracked only | — |
| `hep intelisbetterthanamd` | system info banner | — |
| `hep nvl` | show empty tracked files | — |
| `hep ptl` | print .hep path | — |
| `hep aaa` | stage everything | — |
| `hep linux` | tree view of working dir | — |
| `hep r [n]` | last N commits summary | — |

## wave 3 — unbypassable essentials

| command | does | git equiv |
|---|---|---|
| `hep arm <branch>` | rebase onto branch | `git rebase` |
| `hep ia [remote]` | fetch without merging | `git fetch` |
| `hep intel <file>` | restore one file to HEAD | `git restore` |
| `hep amd start/good/bad/run` | bisect to find bad commit | `git bisect` |
| `hep nvidia` | full reflog — every HEAD position ever | `git reflog` |
| `hep arc` | list all stashes | `git stash list` |
| `hep radeon [-f]` | delete untracked files | `git clean` |

## wave 4 — hardware

| command | does | git equiv |
|---|---|---|
| `hep rtx list/squash/drop` | interactive rebase | `git rebase -i` |
| `hep gtx` | commits grouped by author | `git shortlog` |
| `hep rx <dir> <branch>` | worktree in new dir | `git worktree` |
| `hep iris <repo> [dir]` | add submodule | `git submodule add` |
| `hep xe set/list/clear` | sparse checkout patterns | `git sparse-checkout` |
| `hep uhd` | branch overview with tips | `git show-branch` |
| `hep hd [n]` | export N commits as patches | `git format-patch` |
| `hep fhd <file.patch>` | apply a patch file | `git apply` |
| `hep apu [name] [cmd]` | define command aliases | `git config alias.*` |
| `hep xpu add/show/list` | attach notes to commits | `git notes` |
| `hep npu [commit]` | verify commit integrity | `git verify-commit` |
| `hep cpu <branch>` | new branch from stash | `git stash branch` |
| `hep gpu` | ASCII commit graph | `git log --graph` |
| `hep rpu record/replay/list` | reuse conflict resolutions | `git rerere` |
| `hep a` | find dangling commits | `git fsck --lost-found` |
| `hep b` | prune unreachable objects | `git prune` |

## wave 5 — rig

| command | does | git equiv |
|---|---|---|
| `hep bios` | firmware — help menu & version | `git --help` |
| `hep case` | visual inspection — staged/unstaged | `git status` |
| `hep psu --short <branch>` | toggle rail — switch branch | `git checkout <branch>` |
| `hep psu --reboot <cmt>` | hard reset — pull the plug | `git reset --hard` |
| `hep psu --dust` | fan cleaning — prune loose objects | `git gc` |
| `hep psu --repaste` | thermal overhaul — deep compression | `git gc --aggressive` |
| `hep ups` | backup power — alias for nvidia, same reflog | `git reflog` |
| `hep nas <name> <url>` | external storage — link remote | `git remote add` |
| `hep link` | I/O check — list all connections | `git remote -v` |
| `hep raid` | mirroring — push to all remotes | `git push --all` |
| `hep room <dir> <branch>` | expansion — spare room worktree | `git worktree add` |

## wave 6 — the real gaps

| command | does | git equiv |
|---|---|---|
| `hep compete -l` | line-level diff, red/green +/- lines | `git diff` (proper) |
| `hep print -line <file>` | interactive hunk staging | `git add -p` |
| `hep hall -coat <file>` | stash only specific files | `git stash push <file>` |
| `hep spy -title <file>` | track file history through renames | `git log --follow` |
| `hep accuse -part <f> <s> <e>` | blame specific line range | `git blame -L` |
| `hep rp <old> <new>` | rename file, history preserved | `git mv` |
| `hep unsent` | show commits not pushed yet | `git cherry` |

## wave 7 — better than git

| command | does | git equiv |
|---|---|---|
| `hep undo` | step back one commit, no flags needed | `git reset --hard HEAD~1` |
| `hep redo` | step forward again after undo | (no clean equivalent) |
| `hep mansion limit <size>` | set large file threshold (e.g. 50MB) | `git lfs track` |
| `hep mansion dock [file]` | pull specific large file version | `git lfs pull` |
| `hep mansion light` | show mansion vs normal file status | `git lfs ls-files` |
| `hep mansion send` | push large file bytes to remote | `git lfs push` |

## wave 8 — networking / collaboration

| command | does | git equiv |
|---|---|---|
| `hep ethernet` | auto-generate changelog from wave messages grouped by month | — |
| `hep fiber <date>` | show all commits since a date (e.g. `2025-01-01` or `"7 days ago"`) | `git log --since` |
| `hep switch <br1> <br2>` | show exactly what diverged between two branches | `git log br1..br2` |
| `hep packet [hash]` | show one commit as a full detailed report with file sizes | `git show` |
| `hep ping <author>` | show all commits by a specific author | `git log --author` |
| `hep bandwidth` | show which files change most frequently, bar chart | — |
| `hep latency` | avg/min/max time between commits, activity by hour | — |
| `hep bridge [file.md]` | export repo summary as shareable markdown report | — |

---

## quick start

```sh
hep init
hep house name "your name"
hep house email "you@example.com"
hep print .
hep wave -m "first commit"
hep short
```

## what makes hep different from git

- `hep undo` — no flags, no fear, just undo
- `hep unsent` — see what you haven't pushed without memorizing `git log origin/main..HEAD`
- `hep compete -l` — real +/- line diff built in, no flags to remember
- `hep mansion` — native large file handling, no extensions needed
- `hep accuse` — same as `git blame` but the name is better
- `hep nvidia` / `hep ups` — two ways to reach the reflog, nvidia is the main command, ups is the alias
- `hep raid` — push to all remotes at once

---

AKSH NIKITA LEAD GREG GREGORIAN IN THE EARTH AND THE GRAPE SOLD IN LEBSOCK AND KOEHLER AND COOKED IN THE SILICON TEST AND JUICE CRASHED OUT IN EARTH IS GREG SWARDSTROM IN THE LEAD GREG I HOLD THE GREG IN EARTH AND I AM OM SHAH MONKEYTYPE IN EARTH AND IT SOUNDS LIKE HALL NORMAL AND DHAREN AND I DHAREN IN HOUSE

KIM KOMS CLOSE EIOJ RUYAGNAG HOTEL PYINGAHYNG ADYNSM WPM GNOME WINDOW LINUX WHAT INTELISBETTERTHANAMD NVL PTL AAA BD POWER R

ARM IA INTEL AMD NVIDIA ARC RADEON RTX GTX RX IRIS XE UHD HD FHD APU XPU NPU CPU GPU RPU A B
