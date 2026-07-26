#!/usr/bin/env python3

import os
import sys
import re
from pathlib import Path

def find_root():
    path = Path(os.getcwd()).resolve()
    while path.parent != path:
        if (path / "libbcachefs").exists():
            return path
        path = path.parent
    print("error: cannot find bcachefs-tools root (no libbcachefs/ found)", file=sys.stderr)
    sys.exit(1)

def extract_doc_blocks(directory):
    blocks = []
    # Match DOC(key) { ... } or DOC_LATEX(key) { ... }
    # Rust code uses a more manual parser but regex is easier in Python for this.
    pattern = re.compile(r"DOC(?:_LATEX)?\((?P<key>[^)]+)\)\s*\{(?P<content>.*?)\n\}", re.DOTALL)
    
    for root, _, files in os.walk(directory):
        for file in files:
            if file.endswith((".c", ".h")):
                path = Path(root) / file
                with open(path, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                    for m in re.finditer(r"(?P<type>DOC(?:_LATEX)?)\((?P<key>[^)]+)\)\s*\{", content):
                        start_pos = m.end()
                        depth = 1
                        i = start_pos
                        while i < len(content) and depth > 0:
                            if content[i] == '{': depth += 1
                            elif content[i] == '}': depth -= 1
                            i += 1
                        
                        if depth == 0:
                            block_content = content[start_pos:i-1].strip()
                            # Strip leading/trailing newlines and common indentation
                            lines = block_content.splitlines()
                            if lines:
                                # Find common leading whitespace
                                indent = None
                                for line in lines:
                                    if not line.strip(): continue
                                    match = re.match(r"^(\s*)", line)
                                    if match:
                                        cur_indent = len(match.group(1))
                                        if indent is None or cur_indent < indent:
                                            indent = cur_indent
                                if indent:
                                    lines = [line[indent:] if line.strip() else line for line in lines]
                                block_content = "\n".join(lines).strip()

                            blocks.append({
                                'key': m.group('key'),
                                'content': block_content,
                                'raw_latex': m.group('type') == "DOC_LATEX",
                                'file': path,
                                'line': content.count('\n', 0, m.start()) + 1
                            })
    return blocks

def parse_xmacro(source, macro_name):
    # Simplified X-macro parser
    define_pattern = re.compile(r"#define\s+" + re.escape(macro_name) + r"\(\)\s*(.*?)(?<!\\)\n", re.DOTALL)
    match = define_pattern.search(source)
    if not match:
        return []
    
    macro_text = match.group(1).replace('\\\n', ' ')
    entries = []
    
    # Match x(arg1, arg2, ...)
    pos = 0
    while pos < len(macro_text):
        m = re.search(r"x\s*\(", macro_text[pos:])
        if not m: break
        
        start = pos + m.end()
        depth = 1
        i = start
        while i < len(macro_text) and depth > 0:
            if macro_text[i] == '(': depth += 1
            elif macro_text[i] == ')': depth -= 1
            i += 1
        
        if depth == 0:
            args_str = macro_text[start:i-1]
            # Split args respecting parens and strings
            args = []
            cur_arg = []
            depth_inner = 0
            in_string = False
            j = 0
            while j < len(args_str):
                ch = args_str[j]
                if ch == '"': in_string = not in_string
                elif not in_string:
                    if ch == '(': depth_inner += 1
                    elif ch == ')': depth_inner -= 1
                    elif ch == ',' and depth_inner == 0:
                        args.append("".join(cur_arg).strip())
                        cur_arg = []
                        j += 1
                        continue
                cur_arg.append(ch)
                j += 1
            args.append("".join(cur_arg).strip())
            entries.append(args)
            pos = i
        else:
            break
    return entries

def escape_latex(s):
    replacements = {
        '_': r'\_',
        '#': r'\#',
        '%': r'\%',
        '&': r'\&',
        '$': r'\$',
        '{': r'\{',
        '}': r'\}',
        '~': r'\textasciitilde{}',
        '^': r'\textasciicircum{}',
    }
    return "".join(replacements.get(c, c) for ch in s for c in ch) # Flattening if needed but s is string

def escape_latex_str(s):
    out = ""
    for ch in s:
        if ch == '_': out += r"\_"
        elif ch == '#': out += r"\#"
        elif ch == '%': out += r"\%"
        elif ch == '&': out += r"\&"
        elif ch == '$': out += r"\$"
        elif ch == '{': out += r"\{"
        elif ch == '}': out += r"\}"
        elif ch == '~': out += r"\textasciitilde{}"
        elif ch == '^': out += r"\textasciicircum{}"
        else: out += ch
    return out

def convert_inline(text):
    # `code`, **bold**, *italic*
    text = re.sub(r"`([^`]+)`", lambda m: r"\texttt{" + escape_latex_str(m.group(1)) + "}", text)
    text = re.sub(r"\*\*([^*]+)\*\*", lambda m: r"\textbf{" + escape_latex_str(m.group(1)) + "}", text)
    text = re.sub(r"\*([^*]+)\*", lambda m: r"\textit{" + escape_latex_str(m.group(1)) + "}", text)
    return text

def markup_to_latex(content):
    out = []
    list_state = "None"
    
    for line in content.splitlines():
        if not line.strip():
            if list_state == "Itemize":
                out.append(r"\end{itemize}")
            elif list_state == "Description":
                out.append(r"\end{description}")
            list_state = "None"
            out.append("")
            continue
        
        if line.startswith("- "):
            item = line[2:]
            if list_state == "Description":
                out.append(r"\end{description}")
            if list_state != "Itemize":
                out.append(r"\begin{itemize}")
                list_state = "Itemize"
            out.append(r"\item " + convert_inline(item))
        elif line.startswith("[") and "] " in line:
            close = line.find("] ")
            term = line[1:close]
            desc = line[close + 2:]
            if list_state == "Itemize":
                out.append(r"\end{itemize}")
            if list_state != "Description":
                out.append(r"\begin{description}")
                list_state = "Description"
            out.append(r"\item[" + convert_inline(term) + "] " + convert_inline(desc))
        elif line.startswith("  ") and list_state == "Description":
            out.append(convert_inline(line.strip()))
        else:
            if list_state != "None":
                if list_state == "Itemize": out.append(r"\end{itemize}")
                elif list_state == "Description": out.append(r"\end{description}")
                list_state = "None"
            out.append(convert_inline(line))
            
    if list_state == "Itemize": out.append(r"\end{itemize}")
    elif list_state == "Description": out.append(r"\end{description}")
    
    return "\n".join(out) + "\n"

def join_c_strings(s):
    # Extract content of C strings: "foo" "bar" -> foo bar
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', s)
    result = "".join(parts)
    # Unescape
    return result.replace(r'\n', '\n').replace(r'\t', '\t').replace(r'\\', '\\').replace(r"\'", "'").replace(r'\"', '"')

def parse_opt_flags(flags_str):
    scope = []
    hidden = False
    nodoc = False
    for flag in flags_str.split('|'):
        f = flag.strip()
        if f == "OPT_FS": scope.append("fs")
        elif f == "OPT_FORMAT": scope.append("format")
        elif f == "OPT_MOUNT": scope.append("mount")
        elif f == "OPT_RUNTIME": scope.append("runtime")
        elif f == "OPT_DEVICE": scope.append("device")
        elif f == "OPT_INODE": scope.append("inode")
        elif f == "OPT_HIDDEN": hidden = True
        elif f == "OPT_NODOC": nodoc = True
    return scope, hidden, nodoc

def parse_opt_type_str(s):
    s = s.strip()
    if s.startswith("BCH_OPT_BOOL"): return "bool"
    if s.startswith("BCH_OPT_UINT"): return "uint"
    if s.startswith("BCH_OPT_STR"): return "str"
    if s.startswith("BCH_OPT_BITFIELD"): return "bitfield"
    if s.startswith("BCH_OPT_FN"): return "fn"
    return s

def generate_opts_table(entries):
    out = []
    out.append(r"\begin{longtable}{|l|l|l|p{8cm}|}")
    out.append(r"\hline")
    out.append(r"\textbf{Option} & \textbf{Type} & \textbf{Scope} & \textbf{Description} \\ \hline")
    out.append(r"\endfirsthead")
    out.append(r"\hline \textbf{Option} & \textbf{Type} & \textbf{Scope} & \textbf{Description} \\ \hline")
    out.append(r"\endhead")
    out.append(r"\hline \multicolumn{4}{|r|}{Continued on next page} \\ \hline")
    out.append(r"\endfoot")
    out.append(r"\hline")
    out.append(r"\endlastfoot")

    # Entry: x(name, id, flags, type, hint, help)
    for e in entries:
        if len(e) < 6: continue
        name = e[0]
        flags = e[2]
        opt_type = e[3]
        help_text = join_c_strings(e[5])
        
        scope, hidden, nodoc = parse_opt_flags(flags)
        if hidden or nodoc: continue
        
        t = parse_opt_type_str(opt_type)
        
        out.append(f"{escape_latex_str(name)} & {escape_latex_str(t)} & {', '.join(scope)} & {convert_inline(help_text)} \\\\ \\hline")
        
    out.append(r"\end{longtable}")
    return "\n".join(out) + "\n"

ENUM_LISTS = [
    {
        'key': "error-actions",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_ERROR_ACTIONS",
        'doc_field': 2,
    },
    {
        'key': "csum-opts",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_CSUM_OPTS",
        'doc_field': 3, # Guessed from looking at bcachefs_format.h later or main.rs
    },
    {
        'key': "compression-opts",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_COMPRESSION_OPTS",
        'doc_field': 2,
    },
    {
        'key': "str-hash-opts",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_STR_HASH_OPTS",
        'doc_field': 2,
    },
    {
        'key': "btree-ids",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_BTREE_IDS",
        'doc_field': 4,
        'flags_field': 2,
    },
    {
        'key': "time-stats",
        'header': "libbcachefs/bcachefs.h",
        'macro_name': "BCH_TIME_STATS",
        'doc_field': 1,
    },
    {
        'key': "sb-fields",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_SB_FIELDS",
        'doc_field': 2,
    },
    {
        'key': "jset-entry-types",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_JSET_ENTRY_TYPES",
        'doc_field': 2,
    },
    {
        'key': "counters",
        'header': "libbcachefs/sb/counters_format.h",
        'macro_name': "BCH_PERSISTENT_COUNTERS",
        'doc_field': 3,
    },
    {
        'key': "bkey-types",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_BKEY_TYPES",
        'doc_field': 3,
    },
    {
        'key': "metadata-versions",
        'header': "libbcachefs/bcachefs_format.h",
        'macro_name': "BCH_METADATA_VERSIONS",
        'doc_field': 2,
        'date_field': 3,
        'version_field': 1,
    },
    {
        'key': "recovery-passes",
        'header': "libbcachefs/init/passes_format.h",
        'macro_name': "BCH_RECOVERY_PASSES",
        'doc_field': 4,
        'pass_flags_field': 2,
    },
]

def btree_flags_annotations(flags):
    annotations = []
    if "BTREE_IS_snapshots" in flags: annotations.append("snapshot-aware")
    elif "BTREE_IS_snapshot_field" in flags: annotations.append("snapshot-field")
    if "BTREE_IS_extents" in flags: annotations.append("extent-based")
    if "BTREE_IS_write_buffer" in flags: annotations.append("write-buffered")
    return annotations

def generate_enum_list(el, entries):
    out = []
    out.append(r"\begin{description}")
    for e in entries:
        name = e[0]
        doc = join_c_strings(e[el['doc_field']]) if el.get('doc_field') is not None and el['doc_field'] < len(e) else ""
        if not doc: continue
        
        term = escape_latex_str(name)
        if el.get('version_field') is not None:
            v = e[el['version_field']]
            term += f" (version {v})"
        if el.get('date_field') is not None:
            d = join_c_strings(e[el['date_field']])
            term += f" [{d}]"
            
        annotations = []
        if el.get('flags_field') is not None:
            annotations.extend(btree_flags_annotations(e[el['flags_field']]))
        if el.get('pass_flags_field') is not None:
            f = e[el['pass_flags_field']]
            if "PASS_RESTRUCTRE" in f: annotations.append("restructures") # Typo in Rust code too? PASS_RESTRUCTURE
            if "PASS_ONLINE" in f: annotations.append("online")
            
        if annotations:
            term += " \\textit{(" + ", ".join(annotations) + ")}"
            
        out.append(f"\\item[{term}] {convert_inline(doc)}")
    out.append(r"\end{description}")
    return "\n".join(out) + "\n"

def main():
    root = find_root()
    generated_dir = root / "doc/generated"
    generated_dir.mkdir(parents=True, exist_ok=True)
    
    available_keys = set()
    errors = 0
    
    # --- DOC() blocks from C sources ---
    doc_blocks = extract_doc_blocks(root / "libbcachefs")
    doc_blocks.extend(extract_doc_blocks(root / "c_src"))
    
    for block in doc_blocks:
        latex = block['content'] + "\n" if block['raw_latex'] else markup_to_latex(block['content'])
        with open(generated_dir / f"{block['key']}.tex", "w", encoding="utf-8") as f:
            f.write(latex)
        if block['key'] in available_keys:
            print(f"error: duplicate DOC({block['key']}) in {block['file']}:{block['line']}", file=sys.stderr)
            errors += 1
        available_keys.add(block['key'])
        
    # --- BCH_OPTS() table ---
    with open(root / "libbcachefs/opts.h", "r", encoding="utf-8") as f:
        opts_source = f.read()
    opts_entries = parse_xmacro(opts_source, "BCH_OPTS")
    table = generate_opts_table(opts_entries)
    with open(generated_dir / "opts-table.tex", "w", encoding="utf-8") as f:
        f.write(table)
    available_keys.add("opts-table")
    
    # --- Simple enum lists ---
    for el in ENUM_LISTS:
        with open(root / el['header'], "r", encoding="utf-8") as f:
            source = f.read()
        entries = parse_xmacro(source, el['macro_name'])
        if not entries:
            print(f"warning: {el['macro_name']} in {el['header']} produced no entries", file=sys.stderr)
            continue
        latex = generate_enum_list(el, entries)
        with open(generated_dir / f"{el['key']}.tex", "w", encoding="utf-8") as f:
            f.write(latex)
        available_keys.add(el['key'])
        
    # --- Validate references ---
    with open(root / "doc/bcachefs-principles-of-operation.tex", "r", encoding="utf-8") as f:
        tex = f.read()
    
    refs = re.findall(r"\\bchdoc\{([^}]+)\}", tex)
    # Also scan DOC_LATEX blocks for nested \bchdoc references
    for block in doc_blocks:
        if block['raw_latex']:
            refs.extend(re.findall(r"\\bchdoc\{([^}]+)\}", block['content']))
            
    ref_set = set(refs)
    for r in refs:
        if r not in available_keys and not (generated_dir / f"{r}.tex").exists():
            print(f"error: \\bchdoc{{{r}}} in PoO has no matching DOC({r}) in source", file=sys.stderr)
            errors += 1
            
    for key in available_keys:
        if key not in ref_set:
            print(f"warning: DOC({key}) in source is not referenced by PoO", file=sys.stderr)
            
    if errors > 0:
        print(f"\n{errors} error(s)", file=sys.stderr)
        sys.exit(1)
        
    print(f"bch-docgen: {len(available_keys)} fragment(s) in {generated_dir}")

if __name__ == "__main__":
    main()
