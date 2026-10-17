"""
The web interface without what only its readers need: comments (HTML, CSS,
JavaScript) and the indentation at the start of lines -- the firmware serves
it gzipped from flash, and every byte counts there. www/index.html stays as
it is; tools/gen_ui_asset.py strips what it embeds, then checks that it still
starts (tools/check_ui.js) and parses (node --check).

JavaScript is walked token by token: strings, template literals (with their
${...} nesting) and regular expression literals are copied untouched, so a
"//" in 'http://' or in /\\/\\// is no comment. A regular expression is told
from a division by what comes before the slash, as JavaScript does.
"""

import re

# after these a "/" starts a regular expression, after anything else it divides
_REGEX_AFTER_WORDS = {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw", "case",
                      "do", "else", "yield", "await"}


def strip_js(src: str) -> str:
    out = []
    i, n = 0, len(src)
    last = ""            # the last significant character written (not whitespace, not comment)
    last_word = ""       # the last identifier written, for "return /x/"
    braces = []          # for template literals: "t" (a ${ } inside a template) or "b" (a plain brace)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":                      # a line comment: up to the line's end
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and nxt == "*":                      # a block comment
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
            continue
        if c in "'\"":                                    # a string
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                elif src[j] == "\n":
                    raise ValueError(f"unterminated string at {i}")
                j += 1
            out.append(src[i:j + 1])
            last, last_word, i = c, "", j + 1
            continue
        if c == "`" or (c == "}" and braces and braces[-1] == "t"):   # a template literal (or its rest after ${ })
            if c == "}":
                braces.pop()
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "`":
                    break
                if src[j] == "$" and j + 1 < n and src[j + 1] == "{":
                    braces.append("t")
                    j += 1
                    break
                j += 1
            out.append(src[i:j + 1])
            last, last_word, i = src[j], "", j + 1
            continue
        if c == "/":
            regex = last == "" or last in "(,=:[!&|?{};+-*%<>~^" or last_word in _REGEX_AFTER_WORDS
            if regex:                                     # a regular expression literal
                j, in_class = i + 1, False
                while j < n:
                    if src[j] == "\\":
                        j += 2
                        continue
                    if src[j] == "[":
                        in_class = True
                    elif src[j] == "]":
                        in_class = False
                    elif src[j] == "/" and not in_class:
                        break
                    elif src[j] == "\n":
                        raise ValueError(f"unterminated regular expression at {i}")
                    j += 1
                j += 1
                while j < n and src[j].isalpha():          # flags
                    j += 1
                out.append(src[i:j])
                last, last_word, i = "/", "", j
                continue
        if c == "{":
            braces.append("b")
        elif c == "}" and braces:
            braces.pop()
        if c.isalnum() or c in "_$":
            j = i
            while j < n and (src[j].isalnum() or src[j] in "_$"):
                j += 1
            word = src[i:j]
            out.append(word)
            last, last_word, i = word[-1], word, j
            continue
        out.append(c)
        if not c.isspace():
            last, last_word = c, ""
        i += 1
    return _tidy_lines("".join(out))


def strip_css(src: str) -> str:
    return _tidy_lines(re.sub(r"/\*.*?\*/", "", src, flags=re.S))


def _tidy_lines(text: str) -> str:
    """Indentation and trailing blanks off, empty lines out. Line breaks stay:
    JavaScript inserts semicolons at some of them."""
    lines = (line.strip() for line in text.split("\n"))
    return "\n".join(line for line in lines if line)


def strip_html(html: str) -> str:
    """The page with its scripts' and styles' comments and indentation gone,
    and its own comments."""
    parts = re.split(r"(<script[^>]*>.*?</script>|<style[^>]*>.*?</style>)", html, flags=re.S)
    out = []
    for part in parts:
        m = re.match(r"(<(script|style)[^>]*>)(.*?)(</\2>)", part, flags=re.S)
        if m:
            body = strip_js(m.group(3)) if m.group(2) == "script" else strip_css(m.group(3))
            out.append(m.group(1) + "\n" + body + "\n" + m.group(4))
        else:
            part = re.sub(r"<!--.*?-->", "", part, flags=re.S)
            out.append("\n".join(line.strip() for line in part.split("\n") if line.strip()))
    return "\n".join(p for p in out if p)
