#!/usr/bin/env python3
"""Read-error gate: no B-tree read's error is taken for success.

Every call of a B-tree read (each function of btree.h named bt_seek_*,
bt_cursor_open / next / prev / resume / seek*, bt_get*, bt_first* /
bt_last*, bt_lookup or bt_sample_keys that returns int: the others,
bt_get_ext and bt_get_meta, read in-memory superblock words and cannot
fail), and of every function in xstore.c, vexec.c and
engine.c that returns a value and calls one of those directly, and of
the named read helpers built on them (READ_HELPERS), must either
propagate a failure or carry a marker on its line:

    /* RDERR_OK: <why an error here is safe> */

A call propagates when:
  - it is returned as is (return CALL(...), or return xs_rd_count(CALL)),
  - it is (part of) the condition of an if / while / switch whose body
    propagates, or
  - its result (the variable it is assigned to, or an &...err out
    parameter) is tested within WINDOW lines by a condition whose body
    propagates; for a call inside a loop, the lines after the loop are
    searched too (a walk that breaks on the error and tests it after).
A body propagates when it returns something other than a success
literal (0, 1, SX_OK, SX_DONE, XTC_OK; NULL is a failure), jumps
(goto), assigns an
error (a negative literal, an XTC_E_* or SX_ERROR-class code, or the
tested variable) to another lvalue, sets a failure flag (cancelled,
fail, error, broken, umap_bad, taint), or calls a reporter
(xs_rd_err_tid, xstore_read_err_catalog, xs_iscan_fail,
xs_idx_mark_broken).  A (void) cast or a bare call statement always
needs the marker.

This is a pattern check, not a data-flow analysis: it errs toward
asking for a marker, and a marker's reason is reviewed like code.
Code under #if defined(SQLXTC_VTAB) is not compiled and not checked.
Exit 0 = clean, 1 = a site without propagation or marker.

Usage: test_rderr_sites.py [DIR]   (DIR holds the three sources;
default: the directory above this script).
"""
import os
import re
import sys

FILES = ("xstore.c", "vexec.c", "engine.c")
WINDOW = 12
MARKER = "RDERR_OK:"

FAMILY = re.compile(r"bt_seek_\w+|bt_cursor_(?:open|next|prev|resume|seek\w*)"
                    r"|bt_get\w*|bt_first\w*|bt_last\w*|bt_lookup"
                    r"|bt_sample_keys")

# Read helpers built on the direct readers (whose result is a read's
# outcome), besides the direct readers the script finds itself.
READ_HELPERS = {
    "xs_cat_high", "xs_cat_lookup", "xs_cat_value", "xs_hw_raise",
    "xs_idx_preimage", "xs_idxcat_load", "xs_iscan_cand",
    "xs_iscan_committed", "xs_iscan_next1", "xs_key_live_locked",
    "xs_scan_fill", "xs_scan_load_buffered", "xs_scan_point",
    "xs_wrec_payload", "xs_ww_probe", "xs_ww_conflict_locked",
    "xs_ssi_validate_locked", "xs_uniq_check_stmt",
    "xs_uniq_validate_locked", "xstore_max_rowid_txn",
    "xstore_row_exists_txn", "xstore_scan_next", "xstore_iscan_next",
    "xstore_table_id", "xstore_table_schema", "vx_table_schema",
    "xstore_max_live_rowid", "xstore_drop_index", "xstore_drop_table",
    "xstore_rename_table", "xstore_create_index_ex",
}

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "defined"}
SUCCESS = {"0", "1", "SX_OK", "SX_DONE", "XTC_OK", "SX_ROW"}
REPORTERS = re.compile(r"\b(xs_rd_err_tid|xstore_read_err_catalog|"
                       r"xs_iscan_fail|xs_idx_mark_broken)\s*\(")
FLAG = re.compile(r"\b\w*(cancelled|fail|error|broken|umap_bad|taint)\w*"
                  r"\s*(=[^=]|,\s*1)")
ERRVAL = re.compile(r"^\s*(-\s*\d+|XTC_E_\w+|SX_(ERROR|NOMEM|BUSY|"
                    r"CONSTRAINT|IOERR)|SQLITE_IOERR)\b")


def strip(src):
    """Blank comments and string/char literals, keeping every newline."""
    out = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src[i] in "\"'":
            q, j = src[i], i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == "\\" else 1
            out.append(q + re.sub(r"[^\n]", " ", src[i + 1:j]) + q)
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def blank_vtab(text):
    """Blank the #if defined(SQLXTC_VTAB) ... #endif regions."""
    lines = text.split("\n")
    depth = 0
    for i, ln in enumerate(lines):
        s = ln.strip()
        if depth == 0 and re.match(r"#\s*if\s+defined\s*\(\s*SQLXTC_VTAB", s):
            depth = 1
            lines[i] = ""
            continue
        if depth > 0:
            if re.match(r"#\s*if", s):
                depth += 1
            elif re.match(r"#\s*endif", s):
                depth -= 1
            lines[i] = ""
    return "\n".join(lines)


def functions(text):
    """(name, return type line, body start, body end) of each top-level
    function, as offsets into text (BSD KNF: name at column 0, braces at
    column 0)."""
    out = []
    for m in re.finditer(r"^([A-Za-z_]\w*)\s*\(", text, re.M):
        if m.group(1) in KEYWORDS:
            continue
        b = re.compile(r"^[{;]|;\s*$", re.M).search(text, m.end())
        if b is None or text[b.start()] != "{":
            continue
        e = re.compile(r"^}", re.M).search(text, b.start())
        if e is None:
            continue
        prev = text.rfind("\n", 0, m.start() - 1)
        ret = text[prev + 1:m.start()].strip()
        out.append((m.group(1), ret, b.start(), e.end()))
    return out


def primitives(root):
    """The B-tree reads that can fail: btree.h's int-returning functions
    of the read families."""
    hdr = strip(open(os.path.join(root, "btree.h"), encoding="ascii").read())
    names = {m.group(1) for m in re.finditer(
        r"^\s*int\s+(bt_\w+)\s*\(", hdr, re.M)}
    return sorted(n for n in names if FAMILY.fullmatch(n))


def loop_end(text, start, lo):
    """If text[start] is inside the body of a for / while loop, the
    offset just past that loop's body; else None."""
    d = 0
    for j in range(start - 1, lo, -1):
        c = text[j]
        if c == "}":
            d += 1
        elif c == "{":
            if d == 0:
                head = text[lo:j].rstrip()
                if head.endswith(")"):
                    k = len(head) - 1
                    pd = 0
                    while k >= 0:
                        if head[k] == ")":
                            pd += 1
                        elif head[k] == "(":
                            pd -= 1
                            if pd == 0:
                                break
                        k -= 1
                    if re.search(r"\b(for|while)\s*$", head[:k]):
                        _, e = body_after(text, j)
                        return e
                if re.search(r"\bdo\s*$", head):
                    _, e = body_after(text, j)
                    return e
            else:
                d -= 1
    return None


def match_paren(text, i):
    """Index just past the parenthesis group that opens at text[i]."""
    d = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            d += 1
        elif text[j] == ")":
            d -= 1
            if d == 0:
                return j + 1
    return len(text)


def body_after(text, i):
    """The statement or block that starts at text[i:] (after a
    condition), and the offset just past it."""
    j = i
    while j < len(text) and text[j] in " \t\n":
        j += 1
    if j < len(text) and text[j] == "{":
        d = 0
        for k in range(j, len(text)):
            if text[k] == "{":
                d += 1
            elif text[k] == "}":
                d -= 1
                if d == 0:
                    return text[j:k + 1], k + 1
    d = 0
    for k in range(j, len(text)):
        if text[k] in "({":
            d += 1
        elif text[k] in ")}":
            d -= 1
        elif text[k] == ";" and d <= 0:
            return text[j:k + 1], k + 1
    return text[j:], len(text)


def propagates(body, var):
    """Does this guarded statement / block report a failure?"""
    if re.search(r"\bgoto\b", body) or REPORTERS.search(body) or \
            FLAG.search(body):
        return True
    for m in re.finditer(r"\breturn\b([^;]*);", body):
        e = m.group(1).strip()
        if e and e not in SUCCESS:
            return True
    for m in re.finditer(r"([\w\]\)>.*-]+)\s*=(?!=)([^;]*);", body):
        lhs, rhs = m.group(1).strip(), m.group(2)
        if var is not None and lhs.lstrip("*") == var:
            continue               # an error code into the tested variable
        if ERRVAL.match(rhs):
            return True
        if var is not None and re.search(r"(?<![\w.>])" + re.escape(var) +
                                         r"\b", rhs):
            return True
    return False


def enclosing_cond(text, start, lo):
    """If text[start] lies inside the condition of an if / while / for /
    switch, return (keyword, offset of its '(')."""
    d = 0
    for j in range(start - 1, lo, -1):
        c = text[j]
        if c == ")":
            d += 1
        elif c == "(":
            if d == 0:
                kw = re.search(r"\b(if|while|for|switch)\s*$", text[lo:j])
                if kw:
                    return kw.group(1), j
            else:
                d -= 1
        elif c in ";{}" and d == 0:
            return None
    return None


def stmt_start(text, start, lo):
    """Offset of the start of the statement holding text[start]."""
    d = 0
    for j in range(start - 1, lo, -1):
        c = text[j]
        if c == ")":
            d += 1
        elif c == "(":
            d -= 1
        elif c in ";{}" and d <= 0:
            return j + 1
    return lo


def check_site(text, call_s, call_e, fn_lo, fn_hi):
    """Classify one call: 'ok' or the reason it needs a marker."""
    s = call_s
    w = re.search(r"xs_rd_count\s*\(\s*$", text[fn_lo:s])
    if w:                                   # xs_rd_count(CALL): transparent
        s = fn_lo + w.start()
        call_e = match_paren(text, text.index("(", s))
    before = text[stmt_start(text, s, fn_lo):s]
    if re.search(r"\(\s*void\s*\)\s*$", before):
        return "(void) cast"
    if re.search(r"\breturn\s*$", before):
        return "ok"
    var = None
    am = re.search(r"([\w\]\)>.*-]+?)\s*=(?![=])[^=;]*$", before)
    if am and not re.search(r"[!<>=]$", before[:am.end(1) + 1].rstrip("= ")):
        var = am.group(1).strip().lstrip("*(")
    args = text[s:call_e]
    outs = [a.strip()[1:] for a in re.findall(r"&\s*[\w>.\[\]-]*err\w*", args)]
    vars_ = [v for v in [var] + outs if v]
    cond = enclosing_cond(text, s, fn_lo)
    after = call_e
    if cond is not None:
        kw, op = cond
        cend = match_paren(text, op)
        body, bend = body_after(text, cend)
        if kw in ("if", "switch") and propagates(body, var):
            return "ok"
        if kw in ("while", "for"):
            after = bend                  # the loop's end: look past it
        else:
            after = cend
    elif re.match(r"\s*;", text[call_e:]) and not vars_ and \
            re.fullmatch(r"\s*", before):
        return "result ignored"
    if not vars_:
        return "result not tested"
    regions = [after]
    le = loop_end(text, s, fn_lo)
    if le is not None:
        regions.append(le)
    for r0 in regions:
        if tested(text, r0, fn_hi, vars_):
            return "ok"
    return "no propagating test of " + ", ".join(vars_)


def tested(text, after, fn_hi, vars_):
    """Is one of vars_ tested by a propagating condition, or returned,
    within WINDOW lines of text[after]?"""
    lim = after
    for _ in range(WINDOW):
        nl = text.find("\n", lim + 1)
        lim = nl if 0 <= nl < fn_hi else fn_hi
    region = text[after:lim]
    for v in vars_:
        pat = re.compile(r"\b(if|while|switch)\s*\(")
        for m in pat.finditer(region):
            op = after + m.end() - 1
            cend = match_paren(text, op)
            if not re.search(r"(?<![\w.>])" + re.escape(v) + r"(?![\w])",
                             text[op:cend]):
                continue
            body, _ = body_after(text, cend)
            if m.group(1) == "switch" or propagates(body, v):
                return True
        rm = re.search(r"\breturn\b[^;]*(?<![\w.>])" + re.escape(v) +
                       r"(?![\w])", region)
        if rm:
            return True
    return False


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(here)
    texts, raws, fns = {}, {}, {}
    for f in FILES:
        raw = open(os.path.join(root, f), encoding="ascii").read()
        raws[f] = raw.split("\n")
        texts[f] = blank_vtab(strip(raw))
        fns[f] = functions(texts[f])
    prim = primitives(root)
    pcall = re.compile(r"(?<![\w.>])(" + "|".join(prim) + r")\s*\(")
    readers = set(READ_HELPERS) | set(prim)
    for f in FILES:
        for name, ret, lo, hi in fns[f]:
            if re.search(r"\bvoid\s*$", ret):
                continue
            if pcall.search(texts[f], lo, hi):
                readers.add(name)
    call = re.compile(r"(?<![\w.>])(" +
                      "|".join(sorted(readers, key=len, reverse=True)) +
                      r")\s*\(")
    nsites = nmarked = 0
    bad = []
    for f in FILES:
        text = texts[f]
        for name, ret, lo, hi in fns[f]:
            for m in call.finditer(text, lo, hi):
                ln = text.count("\n", 0, m.start()) + 1
                nsites += 1
                if MARKER in raws[f][ln - 1]:
                    nmarked += 1
                    continue
                why = check_site(text, m.start(), match_paren(
                    text, m.end() - 1), lo, hi)
                if why != "ok":
                    bad.append("%s:%d: %s() in %s: %s" %
                               (f, ln, m.group(1), name, why))
    if bad:
        print("  [rderr] FAIL: read results not propagated (propagate the "
              "error, or mark the line /* RDERR_OK: reason */):")
        for b in bad:
            print("        " + b)
        return 1
    print("  [rderr] OK: %d read sites, %d marked RDERR_OK, %d readers"
          % (nsites, nmarked, len(readers)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
