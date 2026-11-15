"""Match only processes belonging to one isolated Shimmy boot fixture."""

from pathlib import Path


def boot_processes(listing: str, binary: Path, root: Path) -> list[tuple[int, str]]:
    prefix = f"{binary.resolve()} {root.resolve()} "
    matches = []
    for line in listing.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2 or not fields[0].isdigit():
            continue
        if fields[1].startswith(prefix):
            matches.append((int(fields[0]), fields[1][len(prefix):]))
    return matches
