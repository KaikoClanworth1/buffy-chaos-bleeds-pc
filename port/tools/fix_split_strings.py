"""Rejoin C string literals that a shell heredoc split at an intended "\\n".

    python tools/fix_split_strings.py <file.c> [...]

A line with an unterminated string literal followed by a line that starts
with a double quote is joined back with a literal backslash-n escape.
"""
import sys


def unterminated(line):
    in_str = False
    esc = False
    for ch in line:
        if esc:
            esc = False
            continue
        if ch == '\\':
            esc = True
            continue
        if ch == '"':
            in_str = not in_str
    return in_str


for path in sys.argv[1:]:
    data = open(path, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in data else '\n'
    lines = data.split(nl)
    out, i, fixed = [], 0, 0
    while i < len(lines):
        cur = lines[i]
        # A heredoc split leaves the literal's closing quote at column 0.
        while unterminated(cur) and i + 1 < len(lines) and lines[i + 1].startswith('"'):
            i += 1
            cur = cur + '\\n' + lines[i]
            fixed += 1
        out.append(cur)
        i += 1
    open(path, 'w', encoding='utf-8', newline='').write(nl.join(out))
    print(path, 'fixed', fixed)
