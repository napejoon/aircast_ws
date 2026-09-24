# Memory, carried into the repo

Claude Code keeps per-project memory under `~/.claude/projects/<path-slug>/memory/`,
keyed by the workspace path. It does not travel with git and it does not survive a
move to another directory or another machine. This is a copy, so it does.

To restore on a new machine (adjust the slug for where the repo lives —
`D:\aircast_ws` becomes `D--aircast-ws`):

    mkdir -p ~/.claude/projects/D--aircast-ws/memory
    cp .claude/memory/*.md ~/.claude/projects/D--aircast-ws/memory/
    rm ~/.claude/projects/D--aircast-ws/memory/README.md

`MEMORY.md` is the index that gets loaded each session; the other files are one
fact each and are read on demand.
