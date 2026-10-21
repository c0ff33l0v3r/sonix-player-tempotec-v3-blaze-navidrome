#!/usr/bin/env python3
"""Moves interface sizes onto ui_px() (src/gui/shell/uiscale.h), and proves the move.

  uipx.py wrap FILE...   wraps the pixel literals in LVGL size, position and
                         style calls: lv_obj_set_width(o, 40) -> ui_px(40).
  uipx.py check [BASE]   unwraps every ui_px(...) in the files changed since BASE
                         (default 095e282, where blaze-port stood) and prints
                         whatever still differs from BASE. Wrapping alone leaves
                         nothing; what it prints is the hand-made part to review.

wrap only touches integer literals that are a pixel argument of a known call,
at the top of that argument or inside plain parentheses or MIN/MAX. A literal
beside * / % << >> or a comparison is a factor, not a size, and is left alone,
as is everything inside any other nested call. #defines and local arithmetic
are not touched: those are done by hand and show up in check.
"""
import difflib
import re
import subprocess
import sys

INCLUDE = '#include "src/gui/shell/uiscale.h"'

GEOMETRY = {
    'lv_obj_set_size': (1, 2), 'lv_obj_set_width': (1,), 'lv_obj_set_height': (1,),
    'lv_obj_set_x': (1,), 'lv_obj_set_y': (1,), 'lv_obj_set_pos': (1, 2),
    'lv_obj_align': (2, 3), 'lv_obj_align_to': (3, 4), 'lv_obj_set_ext_click_area': (1,),
    'lv_obj_set_content_width': (1,), 'lv_obj_set_content_height': (1,),
    'lv_obj_scroll_to_x': (1,), 'lv_obj_scroll_to_y': (1,), 'lv_obj_scroll_to': (1, 2),
    'lv_obj_scroll_by': (1, 2), 'lv_obj_scroll_by_raw': (1, 2),
}
PX_STYLES = '''width height min_width max_width min_height max_height length x y radius
    pad_all pad_hor pad_ver pad_top pad_bottom pad_left pad_right pad_gap pad_row pad_column
    margin_all margin_hor margin_ver margin_top margin_bottom margin_left margin_right
    border_width outline_width outline_pad shadow_width shadow_spread shadow_offset_x
    shadow_offset_y line_width line_dash_width line_dash_gap arc_width translate_x translate_y
    transform_width transform_height text_letter_space text_line_space blur_radius'''.split()
CALLS = dict(GEOMETRY)
for p in PX_STYLES:
    CALLS['lv_obj_set_style_' + p] = (1,)
    CALLS['lv_style_set_' + p] = (1,)
CALLS['lv_obj_set_style_size'] = (1, 2)
# Sonix's own helpers that take a size in pixels.
CALLS.update({
    'keyboard_create': (1, 2), 'kb_make_key': (1,), 'make_flat_button': (1,),
    'make_roller': (2,), 'make_duration_roller': (2,),
})

TRANSPARENT = {'MIN', 'MAX', 'LV_MIN', 'LV_MAX'}
FACTOR_NEIGHBOURS = set('*/%<>=!&|^')
IDENT = re.compile(r'[A-Za-z_]\w*')
NUMBER = re.compile(r'\d+(?:\.\d*)?(?:[eE][+-]?\d+)?[uUlLfFxX\w.]*')


def skip_literal(src, i):
    """Index past the string/char literal or comment at src[i], or i if none."""
    if src.startswith('//', i):
        j = src.find('\n', i)
        return len(src) if j < 0 else j
    if src.startswith('/*', i):
        j = src.find('*/', i + 2)
        return len(src) if j < 0 else j + 2
    if src[i] in '"\'':
        q, j = src[i], i + 1
        while j < len(src) and src[j] != q:
            j += 2 if src[j] == '\\' else 1
        return j + 1
    return i


def call_args(src, open_paren):
    """[(start, end)] of each top-level argument of the call opened at src[open_paren]."""
    args, depth, start, i = [], 0, open_paren + 1, open_paren + 1
    while i < len(src):
        j = skip_literal(src, i)
        if j != i:
            i = j
            continue
        c = src[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                args.append((start, i))
                return args, i
            depth -= 1
        elif c == ',' and depth == 0:
            args.append((start, i))
            start = i + 1
        i += 1
    raise ValueError('unbalanced call at %d' % open_paren)


def neighbour(src, i, step):
    while 0 <= i < len(src) and src[i] in ' \t\n':
        i += step
    return src[i] if 0 <= i < len(src) else ''


def px_literals(src, start, end):
    """Spans of the integer literals in src[start:end] that are sizes."""
    spans, i, opaque = [], start, 0  # opaque: depth inside a nested call we do not look into
    stack = []
    while i < end:
        j = skip_literal(src, i)
        if j != i:
            i = j
            continue
        c = src[i]
        m = IDENT.match(src, i)
        if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == '_')):
            name = m.group()
            k = m.end()
            while k < end and src[k] in ' \t':
                k += 1
            if k < end and src[k] == '(':
                stack.append(name not in TRANSPARENT)
                opaque += stack[-1]
                i = k + 1
                continue
            i = m.end()
            continue
        if c == '(':
            stack.append(False)
        elif c == ')' and stack:
            opaque -= stack.pop()
        elif c.isdigit() and not (src[i - 1].isalnum() or src[i - 1] in '_.'):
            m = NUMBER.match(src, i)
            tok = m.group()
            if not opaque and tok.isdigit() and tok != '0' \
                    and neighbour(src, i - 1, -1) not in FACTOR_NEIGHBOURS \
                    and neighbour(src, m.end(), 1) not in FACTOR_NEIGHBOURS:
                spans.append((i, m.end()))
            i = m.end()
            continue
        i += 1
    return spans


def wrap(src):
    spans, i = [], 0
    while i < len(src):
        j = skip_literal(src, i)
        if j != i:
            i = j
            continue
        m = IDENT.match(src, i)
        if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == '_')):
            k = m.end()
            if m.group() in CALLS and k < len(src) and src[k] == '(':
                args, _ = call_args(src, k)
                for n in CALLS[m.group()]:
                    if n < len(args):
                        spans += px_literals(src, *args[n])
            i = m.end()
            continue
        i += 1
    out, last = [], 0
    for a, b in sorted(set(spans)):
        out += [src[last:a], 'ui_px(', src[a:b], ')']
        last = b
    out.append(src[last:])
    text = ''.join(out)
    if spans and INCLUDE not in text:
        lines = text.split('\n')
        at = max(n for n, l in enumerate(lines) if l.startswith('#include'))
        lines.insert(at + 1, INCLUDE)
        text = '\n'.join(lines)
    return text, len(set(spans))


def unwrap(src):
    out, i = [], 0
    while True:
        j = src.find('ui_px(', i)
        if j < 0:
            out.append(src[i:])
            break
        if j > 0 and (src[j - 1].isalnum() or src[j - 1] == '_'):
            out.append(src[i:j + 6])
            i = j + 6
            continue
        args, close = call_args(src, j + 5)
        out += [src[i:j], src[args[0][0]:args[0][1]]]
        i = close + 1
    return '\n'.join(l for l in ''.join(out).split('\n') if l != INCLUDE)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == 'wrap':
        total = 0
        for path in sys.argv[2:]:
            src = open(path).read()
            text, n = wrap(src)
            if text != src:
                open(path, 'w').write(text)
            print('%5d  %s' % (n, path))
            total += n
        print('%5d  total' % total)
    elif len(sys.argv) >= 2 and sys.argv[1] == 'check':
        base = sys.argv[2] if len(sys.argv) > 2 else '095e282'
        files = subprocess.run(['git', 'diff', '--relative', '--name-only', base, '--', '*.c', '*.h'],
                               capture_output=True, text=True, check=True).stdout.split()
        clean = 0
        for path in files:
            try:
                old = subprocess.run(['git', 'show', '%s:./%s' % (base, path)], capture_output=True,
                                     text=True, check=True).stdout
            except subprocess.CalledProcessError:
                print('new file: ' + path)
                continue
            new = unwrap(open(path).read())
            if new == old:
                clean += 1
                continue
            print('--- %s: differs beyond ui_px()' % path)
            print(''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True), 'base', 'unwrapped', n=1)))
        print('%d of %d changed files are only ui_px() wrapping' % (clean, len(files)))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
