#!/usr/bin/env python3
"""hybridize -- generate guest-form C++ for the x64 hybrid build from the
pcj-decomp source, which stays the single source of truth (never edited).

  hybridize.py gen  --unit FEScript --va 001077C0 [--va ...] [--unit ... --va ...] -o build/hybrid
  hybridize.py scan UNIT [UNIT...]        # classify every MATCH function: translatable or why not

Approach (docs/hybridize.md):
  * Parse the unit with libclang as i386-pc-windows-msvc (MSVC record layout,
    MSVC mangling).  Layout truth = clang's MSVC layout of the pcj-decomp
    headers (-fdump-record-layouts for base/vfptr offsets, Type.get_offset for
    fields), cross-checked against symbols/classes.json sizes.
  * Views: namespace gv mirrors the original names.  Each record becomes a
    packed struct with explicit padding, pointer fields as GuestPtr<T>,
    uint32_t vfptr slots, bases flattened, static_asserts on every offset and
    on sizeof.  Because the names are the same, type names inside translated
    bodies need no rewriting.
  * Bodies: the original source text is spliced; only AST nodes that need it
    are rewritten (this -> self, implicit member -> self->m, calls -> native
    or guest calls, virtual calls -> guest vtable dispatch, implicit upcasts ->
    explicit offset adjust, globals -> guest VA).
"""
import sys, os, re, csv, json, argparse, subprocess, collections
import clang.cindex as ci

ci.Config.set_library_file(os.environ.get('LIBCLANG', '/usr/lib/llvm-18/lib/libclang.so.1'))
CK, TK = ci.CursorKind, ci.TypeKind
_REPO_DECOMP = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'pcj-decomp')
DECOMP = os.environ.get('PCJ_DECOMP') or (_REPO_DECOMP if os.path.isdir(_REPO_DECOMP) else '/home/claude/pcj-decomp')
ARGS = ['-x', 'c++', '-target', 'i386-pc-windows-msvc', '-fms-extensions', '-fms-compatibility',
        '-fms-compatibility-version=13.10', '-I' + DECOMP + '/include', '-isystem', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shim'), '-w', '-Wno-non-pod-varargs']


class AnonMember:
    """flattened-field entry for an unnamed union/struct member (its members live in the enclosing scope)"""
    def __init__(self, leaves): self.leaves = leaves     # [(member name, absolute offset)]


class Unsupported(Exception):
    pass


class NeedPP(Exception):
    """the function uses preprocessor macros: its text must come from the preprocessed unit
    (deliberately not an Unsupported, so no fallback path swallows it)"""


_PREDEF = None
def predefined_macros():
    global _PREDEF
    if _PREDEF is None:
        out = subprocess.run(['clang++', '-E', '-dM', *ARGS, '-'], input='', capture_output=True, text=True).stdout
        _PREDEF = {l.split()[1].split('(')[0] for l in out.splitlines() if l.startswith('#define ')}
    return _PREDEF


# ---------------------------------------------------------------- symbols
def load_symbols():
    fn = {}
    for r in csv.DictReader(open(os.path.join(DECOMP, 'symbols', 'functions.csv'))):
        # the same mangled name also exists in XDK libraries (operator new in d3dx8/dsound/...):
        # the first game/enginex copy wins
        old = fn.get(r['name'])
        if old is None or (old['origin'] not in ('game', 'enginex') and r['origin'] in ('game', 'enginex')):
            fn[r['name']] = r
    data = {}
    p = os.path.join(DECOMP, 'symbols', 'symbols.csv')
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            nm = r.get('name') or r.get('mangled')
            if nm: data[nm] = r
    # file statics with no symbol (internal linkage), VAs read from the original code: mangled,va
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'statics.csv')
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            data[r['mangled']] = {'va': r['va']}
    st = {r['va'].lower(): r for r in csv.DictReader(open(os.path.join(DECOMP, 'status', 'status.csv')))}
    classes = json.load(open(os.path.join(DECOMP, 'symbols', 'classes.json')))
    return fn, data, st, classes


# ---------------------------------------------------------------- layout dump
class Layout:
    """Parsed `-fdump-record-layouts` (MSVC ABI): per record size, align,
    direct base offsets, own vfptr offset, virtual bases."""
    def __init__(self, path, force=()):
        # sizeof() on every complete record forces clang to lay it out (and dump it)
        src = f'#include "{path}"\n' + ''.join(f'int __hy_force_{i} = sizeof(::{n});\n' for i, n in enumerate(force))
        out = subprocess.run(['clang++', '-fsyntax-only', *ARGS, '-Xclang', '-fdump-record-layouts', '-'],
                             input=src, capture_output=True, text=True).stdout
        self.rec = {}
        cur = None
        for line in out.splitlines():
            if line.startswith('*** Dumping AST Record Layout'):
                cur = None; continue
            m = re.match(r'^\s*(\d+)(?::\d+-\d+)? \|( *)(.*)$', line)
            if m:
                off, ind, txt = int(m.group(1)), len(m.group(2)), m.group(3)
                if cur is None:
                    name = norm_name(txt)
                    cur = self.rec.setdefault(name, dict(bases=[], vfptr=[], vbase=False, size=None, align=None, union=txt.startswith('union')))
                    continue
                if ind != 3: continue
                if txt.endswith('vftable pointer)'): cur['vfptr'].append(off)
                elif txt.endswith('vbtable pointer)') or txt.endswith('(virtual base)'): cur['vbase'] = True
                elif txt.endswith('(base)') or txt.endswith('(primary base)'):
                    cur['bases'].append((norm_name(txt.rsplit(' (', 1)[0]), off))
                continue
            m = re.match(r'^\s*\| \[sizeof=(\d+), (?:dsize=\d+, )?align=(\d+)', line)
            if m and cur is not None and cur['size'] is None:
                cur['size'], cur['align'] = int(m.group(1)), int(m.group(2))

    def get(self, name):
        return self.rec.get(name)

    def base_offset(self, derived, base):
        if derived == base: return 0
        r = self.rec.get(derived)
        if not r: raise Unsupported(f'no layout for {derived}')
        for b, off in r['bases']:
            try: return off + self.base_offset(b, base)
            except Unsupported: pass
        raise Unsupported(f'{base} is not a non-virtual base of {derived}')


# ---------------------------------------------------------------- helpers
RECS = (CK.CLASS_DECL, CK.STRUCT_DECL, CK.UNION_DECL)

def qname(c, real=False):
    """View name. A record nested in a record is flattened (Outer__Inner): C++ cannot forward-declare
    a nested class, and a view of Outer may need Inner (or the reverse) in any order. The outer view
    carries `typedef Outer__Inner Inner;`, so the spliced `Outer::Inner` still resolves.
    real=True keeps the source spelling (classes.json keys)."""
    if c.kind in RECS:
        ts = rec_name(c.type)
        if '<' in ts:   # template instance: one view per specialization
            head, args = ts.split('<', 1)
            return head + '_T_' + re.sub(r'_+', '_', sanitize(args.rstrip('>'))).strip('_')
    parts = []
    while c is not None and c.kind != CK.TRANSLATION_UNIT:
        if c.kind in RECS and anon(c.spelling) and c.spelling:
            parts.append((f'__anon_{c.location.line}_{c.location.column}', True))   # unnamed struct used as a member type
        elif c.spelling and not (c.kind == CK.ENUM_DECL and anon(c.spelling)): parts.append((c.spelling, c.kind in RECS))
        c = c.semantic_parent
    parts.reverse()
    out = ''
    for i, (nm, isrec) in enumerate(parts):
        if i: out += '__' if (isrec and parts[i - 1][1] and not real) else '::'
        out += nm
    return out

def rec_decl(t):
    t = t.get_canonical()
    if t.kind == TK.RECORD:
        d = t.get_declaration()
        return d.get_definition() or d
    return None

def rec_name(t):
    """Layout-dump spelling of a record type (canonical, qualified)."""
    return norm_name(t.get_canonical().spelling)


def norm_name(s):
    return re.sub(r'\b(class|struct|union|enum) ', '', s)

def pointee_rec(t):
    t = t.get_canonical()
    if t.kind in (TK.POINTER, TK.LVALUEREFERENCE):
        return rec_decl(t.get_pointee())
    return None

def anon(sp):
    return not sp or sp.startswith('(') or 'anonymous' in sp or 'unnamed' in sp

def mentions_template_params(c):
    """True if c is an instantiated function template / member of a class template whose source text
    (the dependent pattern) names a template parameter: splicing it would paste `T` into the output."""
    f = ci.conf.lib.clang_getSpecializedCursorTemplate
    f.argtypes, f.restype = [ci.Cursor], ci.Cursor
    t = f(c)
    if t is None or t.kind == CK.NO_DECL_FOUND or t.kind.is_invalid(): return False
    names = set()
    for x in [t] + ([t.semantic_parent] if t.semantic_parent is not None and t.semantic_parent.kind == CK.CLASS_TEMPLATE else []):
        for ch in x.get_children():
            if ch.kind in (CK.TEMPLATE_TYPE_PARAMETER, CK.TEMPLATE_NON_TYPE_PARAMETER, CK.TEMPLATE_TEMPLATE_PARAMETER):
                names.add(ch.spelling)
    return any(tk.kind == ci.TokenKind.IDENTIFIER and tk.spelling in names for tk in c.get_tokens())

def sanitize(s):
    return re.sub(r'\W', '_', s)

# MSVC integer keywords that the spliced bodies spell (plain C++ has no __int64)
MSVC_KEYWORDS = '#define __int8 char\n#define __int16 short\n#define __int32 int\n#define __int64 long long'

# pcj-decomp include/types.h; the spliced bodies spell locals and casts with these names
PCJ_TYPEDEF_NAMES = {'s8', 'u8', 's16', 'u16', 's32', 'u32', 's64', 'u64', 'f32'}
PCJ_TYPEDEFS = ('typedef int8_t s8; typedef uint8_t u8; typedef int16_t s16; typedef uint16_t u16; '
                'typedef int32_t s32; typedef uint32_t u32; typedef int64_t s64; typedef uint64_t u64; typedef float f32;')

BUILTIN = {
    TK.BOOL: 'bool', TK.CHAR_S: 'char', TK.SCHAR: 'int8_t', TK.CHAR_U: 'uint8_t', TK.UCHAR: 'uint8_t',
    TK.SHORT: 'int16_t', TK.USHORT: 'uint16_t', TK.INT: 'int32_t', TK.UINT: 'uint32_t',
    TK.LONG: 'int32_t', TK.ULONG: 'uint32_t', TK.LONGLONG: 'int64_t', TK.ULONGLONG: 'uint64_t',
    TK.FLOAT: 'float', TK.DOUBLE: 'double', TK.LONGDOUBLE: 'double', TK.WCHAR: 'uint16_t', TK.VOID: 'void',
}
SIZE = {'bool': 1, 'char': 1, 'int8_t': 1, 'uint8_t': 1, 'int16_t': 2, 'uint16_t': 2, 'int32_t': 4, 'uint32_t': 4,
        'int64_t': 8, 'uint64_t': 8, 'float': 4, 'double': 8}


class Gen:
    def __init__(self, unit, pp=False):
        self.fn, self.data, self.status, self.classes = SYMS
        self.unit = unit
        self.path = unit if os.path.exists(unit) else \
            next(p for d in ('game', 'enginex') for p in [os.path.join(DECOMP, 'src', d, unit + '.cpp')] if os.path.exists(p))
        self.src_path, self.pp, self._macros = self.path, pp, None
        if pp:
            # macro-using functions: parse the preprocessed unit, where every macro is already expanded
            import tempfile
            self.pp_dir = tempfile.mkdtemp(prefix='hypp_')
            self.path = os.path.join(self.pp_dir, os.path.basename(self.src_path)[:-4] + '.pp.cpp')
            r = subprocess.run(['clang++', '-E', '-P', *ARGS, self.src_path], capture_output=True, text=True)
            if r.returncode: raise SystemExit(f'{unit}: preprocessing failed: {r.stderr[:200]}')
            open(self.path, 'w').write(r.stdout)
        self.tu = ci.Index.create().parse(self.path, args=ARGS)
        # VC 7.1 accepts a few things clang rejects even in MS mode (a const
        # char array from a ?: of string literals passed as char*, non-POD
        # objects through ...). They only matter if a translated function
        # contains them; its generated code then fails to compile and says so.
        MSVC_OK = ("cannot initialize a parameter of type 'char *' with an lvalue of type 'const char",
                   'through variadic function')
        errs = [d for d in self.tu.diagnostics if d.severity >= 3]
        soft = [d for d in errs if any(k in d.spelling for k in MSVC_OK)]
        if soft: print(f'{unit}: tolerated {len(soft)} MSVC-only diagnostic(s), first at line {soft[0].location.line}')
        errs = [d for d in errs if d not in soft]
        if errs: raise SystemExit(f'{unit}: parse errors: {errs[:3]}')
        force = []
        for c in self.tu.cursor.walk_preorder():
            if c.kind in (CK.CLASS_DECL, CK.STRUCT_DECL) and c.is_definition() and c.spelling and \
                    c.semantic_parent.kind in (CK.TRANSLATION_UNIT, CK.NAMESPACE, CK.CLASS_DECL, CK.STRUCT_DECL) and \
                    c.type.get_size() > 0:
                if c.type.get_canonical().kind == TK.RECORD and 'anonymous' not in c.type.spelling:
                    force.append(rec_name(c.type))
        self.layout = Layout(self.path, sorted(set(force)))
        self.defs = {}
        for c in self.tu.cursor.walk_preorder():
            if c.kind in (CK.CXX_METHOD, CK.FUNCTION_DECL, CK.CONSTRUCTOR, CK.DESTRUCTOR, CK.FUNCTION_TEMPLATE) \
                    and c.is_definition():
                try: self.defs.setdefault(c.mangled_name, c)
                except Exception: pass
        self._src = {}
        self.need_rec = collections.OrderedDict()   # qualified name -> cursor
        self.bf_names = set()  # bitfield members (no offsetof)
        self.opaque = {}       # qualified name -> cursor of records declared but never defined in this unit
        self.native = {}       # mangled -> native fn name (functions translated in this generation)
        self.out_funcs = []    # generated function texts
        self.sites = {}        # CJ_SITE_<n> token -> call key ('d', callee VA) / ('v', vtable byte offset)
        self.top_calls = None  # callee VAs the original of the swap target being translated calls (orig_sites)
        self.protos = []
        self.notes = []
        self.enums = collections.OrderedDict()
        self.fwd, self._fwdc = set(), {}
        self.cur = self.cls = None
        self.fr = None         # guest frame of the function being translated (gap 1)

    # ---- record structure that libclang hides on implicit template specializations
    # (get_children() of an instantiated class is empty: no base specifiers, no fields)
    def registry(self):
        if getattr(self, '_reg', None) is None:
            self._reg = {}
            for c in self.tu.cursor.walk_preorder():
                try: t = c.type.get_canonical()
                except Exception: continue
                if t.kind == TK.RECORD:
                    d = rec_decl(t)
                    if d is not None and d.is_definition(): self._reg.setdefault(rec_name(t), d)
        return self._reg

    def base_decls(self, d):
        """[(layout name, decl or None)] of the direct bases of d, in layout order."""
        specs = [b for b in d.get_children() if b.kind == CK.CXX_BASE_SPECIFIER]
        if specs: return [(rec_name(b.type), rec_decl(b.type)) for b in specs]
        lay = self.layout.get(rec_name(d.type))
        if not lay or not lay['bases']: return []
        return [(bname, self.registry().get(bname)) for bname, _ in lay['bases']]

    def own_fields(self, d):
        fs = [f for f in d.get_children() if f.kind == CK.FIELD_DECL]
        if not fs and d.kind in (CK.CLASS_DECL, CK.STRUCT_DECL) and '<' in d.type.get_canonical().spelling:
            fs = list(d.type.get_canonical().get_fields())
        return fs

    def unnamed_members(self, d):
        """Anonymous union/struct members (`union { ... };`): libclang lists only the record, not its
        implicit field. A record that is the type of a named field is not one of these."""
        used = {rec_decl(f.type).hash for f in self.own_fields(d)
                if f.type.get_canonical().kind == TK.RECORD and anon(rec_decl(f.type).spelling)}
        return [x for x in d.get_children() if x.kind in (CK.UNION_DECL, CK.STRUCT_DECL) and x.is_definition()
                and anon(x.spelling) and x.hash not in used]

    def flat_layout_only(self, bname, base_off, out):
        """Base whose declaration libclang does not expose: vfptrs and nested bases from the layout
        dump, its fields stay padding (a body that touches one fails to compile, which is safe)."""
        lay = self.layout.get(bname)
        if lay is None or lay['size'] is None: raise Unsupported(f'no layout for {bname}')
        if lay['vbase']: raise Unsupported(f'{bname}: virtual base')
        if lay['union']: raise Unsupported(f'{bname}: union')
        for off in lay['vfptr']:
            out.append((base_off + off, '__vfptr' if base_off + off == 0 else f'__vfptr_{base_off+off:x}', 'uint32_t', '', 4, None))
        for nb, noff in lay['bases']:
            bd = self.registry().get(nb)
            if bd is not None: self.flat_fields(bd, base_off + noff, out, set(), prefix=nb.split('::')[-1] + '__')
            else: self.flat_layout_only(nb, base_off + noff, out)

    def macro_names(self):
        if self._macros is None:
            out = subprocess.run(['clang++', '-E', '-dM', *ARGS, self.src_path], capture_output=True, text=True).stdout
            self._macros = {l.split()[1].split('(')[0] for l in out.splitlines() if l.startswith('#define ')} - predefined_macros()
        return self._macros

    def uses_macro(self, c):
        """True if an identifier token of the function's own extent names a macro of the unit (the AST
        shows the expansion, but the source text we splice shows the invocation)."""
        if c.extent.start.file is None: return False
        m = self.macro_names()
        toks = list(c.get_tokens())
        if not toks: return True       # the extent lies inside a macro expansion (macro-generated function)
        return any(t.kind == ci.TokenKind.IDENTIFIER and t.spelling in m for t in toks)

    # ---- source text
    def text(self, c):
        f = c.extent.start.file
        if f is None: raise Unsupported('node without file')
        if f.name not in self._src: self._src[f.name] = open(f.name, 'rb').read().decode('latin-1')
        return self._src[f.name], c.extent.start.offset, c.extent.end.offset, f.name

    def va_of(self, c):
        try: m = c.mangled_name
        except Exception: return None
        r = self.fn.get(m)
        return r['va'].lower() if r else None

    # ---- type mapping in native signatures / casts
    def want(self, d):
        if d is None: return
        dd = d.get_definition()
        if dd is not None: d = dd            # any declaration cursor of a complete record means its definition
        q = qname(d)
        if not d.is_definition():
            # never completed in this unit (only pointers to it exist in the source): opaque `struct X;`
            self.opaque[q] = d
            if self.outer_rec(d) is not None: self.want(self.outer_rec(d))
            return
        if q not in self.need_rec:
            self.need_rec[q] = d
            if self.outer_rec(d) is not None: self.want(self.outer_rec(d))   # nested record: defined inside its outer

    def ntype(self, t, keep_const=False):
        """Native C++ spelling for a parameter / return / local of type t."""
        t = t.get_canonical()
        if t.kind in BUILTIN: return BUILTIN[t.kind]
        if t.kind == TK.POINTER:
            p = t.get_pointee().get_canonical()
            if p.kind == TK.FUNCTIONPROTO: return 'uint32_t'
            q = ' const' if p.is_const_qualified() else ''
            return self.ntype(p) + q + ' *'
        if t.kind == TK.LVALUEREFERENCE:
            return ('const ' if keep_const and t.get_pointee().is_const_qualified() else '') + self.ntype(t.get_pointee()) + ' &'
        if t.kind == TK.RECORD:
            d = rec_decl(t); self.want(d); return 'gv::' + qname(d)
        if t.kind == TK.ENUM:
            if anon(t.get_declaration().spelling): return 'int32_t'
            self.want_enum(t.get_declaration()); return 'gv::' + qname(t.get_declaration())
        raise Unsupported(f'type {t.spelling} ({t.kind.name})')

    def outer_rec(self, d):
        """enclosing class/struct of a nested record (None for namespace-level records)"""
        p = d.semantic_parent
        if p is not None and p.kind in (CK.CLASS_DECL, CK.STRUCT_DECL, CK.UNION_DECL) and not anon(p.spelling) and \
                 '<' not in p.type.get_canonical().spelling:   # specializations flatten their nested names
            return rec_decl(p.type) or p
        return None

    def want_enum(self, d):
        p = d.semantic_parent
        if p.kind in (CK.CLASS_DECL, CK.STRUCT_DECL): self.want(p)
        else: self.enums[qname(d)] = d

    # ---------------------------------------------------------------- views
    def field_type(self, t, nm):
        t = t.get_canonical()
        if t.kind in BUILTIN and t.kind != TK.VOID: return BUILTIN[t.kind], ''
        if t.kind in (TK.POINTER, TK.LVALUEREFERENCE):
            p = t.get_pointee().get_canonical()
            if p.kind == TK.FUNCTIONPROTO: return 'uint32_t', ''
            if p.kind == TK.VOID: return 'GuestPtr<void>', ''
            if p.kind == TK.RECORD:
                d = rec_decl(p); self._fwdc[qname(d)] = d
                if self.outer_rec(d) is not None: self.want(self.outer_rec(d))
                return f'GuestPtr<gv::{qname(d)}>', ''
            if p.kind == TK.POINTER:
                inner, arr = self.field_type(p, nm)
                return f'GuestPtr<{inner}>', ''
            inner, arr = self.field_type(p, nm)
            if arr: return 'GuestPtr<void>', ''
            return f'GuestPtr<{inner}>', ''
        if t.kind == TK.RECORD:
            d = rec_decl(t); self.want(d); return 'gv::' + qname(d), ''
        if t.kind == TK.ENUM:
            if anon(t.get_declaration().spelling): return 'int32_t', ''
            self.want_enum(t.get_declaration()); return 'gv::' + qname(t.get_declaration()), ''
        if t.kind == TK.CONSTANTARRAY:
            inner, arr = self.field_type(t.element_type, nm)
            return inner, f'[{t.element_count}]' + arr
        raise Unsupported(f'field {nm}: {t.spelling} ({t.kind.name})')

    def flat_fields(self, d, base_off, out, seen_names, prefix=''):
        lay = self.layout.get(rec_name(d.type))
        if lay is None: raise Unsupported(f'no layout for {qname(d)}')
        if lay['vbase']: raise Unsupported(f'{qname(d)}: virtual base')
        if lay['union']: raise Unsupported(f'{qname(d)}: union')
        for off in lay['vfptr']:
            out.append((base_off + off, '__vfptr' if base_off + off == 0 else f'__vfptr_{base_off+off:x}', 'uint32_t', '', 4, None))
        bdecls = dict(self.base_decls(d))
        for bname, boff in lay['bases']:
            bd = bdecls.get(bname)
            if bd is None: self.flat_layout_only(bname, base_off + boff, out)
            else: self.flat_fields(bd, base_off + boff, out, seen_names, prefix=bname.split('::')[-1] + '__')
        for f in self.own_fields(d):
            off = d.type.get_offset(f.spelling)
            if f.is_bitfield():
                ty = BUILTIN.get(f.type.get_canonical().kind)
                if ty is None: raise Unsupported(f'{qname(d)}::{f.spelling} bitfield type')
                usz = f.type.get_canonical().get_size()
                uoff = base_off + (off // (8 * usz)) * usz
                if out and out[-1][5] == 'bf' and out[-1][0] == uoff and out[-1][2] == ty:
                    o = out[-1]; out[-1] = (o[0], o[1], ty, o[3] + [(f.spelling, f.get_bitfield_width())], usz, 'bf')
                else:
                    out.append((uoff, f'__bf_{uoff:x}', ty, [(f.spelling, f.get_bitfield_width())], usz, 'bf'))
                continue
            if off < 0 or off % 8: raise Unsupported(f'{qname(d)}::{f.spelling} offset {off}')
            ty, arr = self.field_type(f.type, f.spelling)
            out.append((base_off + off // 8, f.spelling, ty, arr, f.type.get_canonical().get_size(), (d, f)))
        for ad in self.unnamed_members(d):
            text, sub = self.anon_inline(ad)
            if not sub: raise Unsupported(f'{qname(d)}: empty unnamed member')
            o = d.type.get_offset(sub[0][0])
            if o < 0 or o % 8: raise Unsupported(f'{qname(d)}: offset of unnamed member')
            aoff = base_off + o // 8 - sub[0][1]
            out.append((aoff, '', text, '', ad.type.get_size(), AnonMember([(n, aoff + r) for n, r in sub])))

    def anon_inline(self, d, name=None):
        """Text of a union / unnamed struct, members at their MSVC offsets (explicit padding in structs).
        Returns (text, [(leaf member name, offset relative to d)])."""
        lay = self.layout.get(rec_name(d.type))
        if lay is None or lay['size'] is None: raise Unsupported(f'no layout for {qname(d)}')
        if lay['vbase'] or lay['vfptr'] or lay['bases']: raise Unsupported(f'{qname(d)}: base or vfptr in a union/unnamed record')
        isu, size = lay['union'], lay['size']
        parts, leaves, pos = [], [], 0
        self._anon_n = getattr(self, '_anon_n', 0)
        items = []          # (relative offset, member text, leaves, size)
        bfs = {}            # (unit offset, type) -> [(name, width)]
        for f in self.own_fields(d):
            if f.is_bitfield():
                bty = BUILTIN.get(f.type.get_canonical().kind)
                if bty is None: raise Unsupported(f'{qname(d)}::{f.spelling} bitfield type')
                usz = f.type.get_canonical().get_size()
                o = d.type.get_offset(f.spelling)
                if o < 0: raise Unsupported(f'{qname(d)}::{f.spelling} bitfield offset')
                bfs.setdefault(((o // (8 * usz)) * usz, bty, usz), []).append((f.spelling, f.get_bitfield_width()))
                self.bf_names.add(f.spelling)
                continue
            ty, arr = self.field_type(f.type, f.spelling)
            o = d.type.get_offset(f.spelling)
            if o < 0 or o % 8: raise Unsupported(f'{qname(d)}::{f.spelling} offset {o}')
            items.append((o // 8, f'{ty} {f.spelling}{arr}', [(f.spelling, 0)], f.type.get_canonical().get_size()))
        for (uoff, bty, usz), members in bfs.items():
            # MSVC bitfield unit: ms_struct reproduces its allocation
            bits = ' '.join(f'{bty} {n}:{w};' for n, w in members)
            items.append((0 if isu else uoff, f'struct __attribute__((ms_struct)) {{ {bits} }}', [(n, 0) for n, _ in members if n], usz))
        for ad in self.unnamed_members(d):
            text, sub = self.anon_inline(ad)
            if not sub: raise Unsupported(f'{qname(d)}: empty unnamed member')
            if isu: rel = 0
            else:
                o = d.type.get_offset(sub[0][0])
                if o < 0 or o % 8: raise Unsupported(f'{qname(d)}: offset of unnamed member')
                rel = o // 8 - sub[0][1]
            items.append((rel, text, sub, ad.type.get_size()))
        for rel, member, sub, msz in sorted(items, key=lambda x: x[0]):
            if not isu:
                if rel < pos: raise Unsupported(f'{qname(d)}: overlapping members')
                if rel > pos:
                    parts.append(f'uint8_t __apad{self._anon_n}[0x{rel - pos:x}];'); self._anon_n += 1
            parts.append(member + ';')
            leaves += [(n, rel + r) for n, r in sub]
            pos = max(pos, rel + msz)
        if pos < size:
            parts.append(f'uint8_t __apad{self._anon_n}[0x{size - pos:x}];'); self._anon_n += 1
        return ('union' if isu else 'struct') + (' ' + name if name else '') + ' { ' + ' '.join(parts) + ' }', leaves

    def view(self, d):
        q = qname(d)
        lay = self.layout.get(rec_name(d.type))
        if lay is None or lay['size'] is None: raise Unsupported(f'no layout for {q}')
        if lay['union']:
            text, leaves = self.anon_inline(d, q.split('::')[-1])
            cj = self.classes.get(qname(d, True))
            asserts = [f'static_assert(offsetof(gv::{q}, {n}) == 0x{o:x}, "{q}::{n}");' for n, o in leaves if n not in self.bf_names]
            asserts.append(f'static_assert(sizeof(gv::{q}) == 0x{lay["size"]:x}, "sizeof {q}");')
            return q, text + ';', asserts
        fields = []
        self.flat_fields(d, 0, fields, set())
        fields.sort(key=lambda x: x[0])
        names = collections.Counter(f[1] for f in fields)
        lines, pos, asserts, pad = [], 0, [], 0
        own = {f[1] for f in fields if isinstance(f[5], tuple) and f[5][0] == d}
        used = set()
        for off, nm, ty, arr, sz, origin in fields:
            if off < pos: raise Unsupported(f'{q}: overlapping field {nm} at {off:#x}')
            if off > pos:
                lines.append(f'    uint8_t __pad{pad}[0x{off-pos:x}];'); pad += 1
            if isinstance(origin, AnonMember):
                lines.append(f'    {ty};  // 0x{off:03x} unnamed union/struct member')
                for n, o in origin.leaves:
                    if n not in self.bf_names: asserts.append(f'static_assert(offsetof(gv::{q}, {n}) == 0x{o:x}, "{q}::{n}");')
                pos = off + sz
                continue
            if origin == 'bf':
                # MSVC bitfield unit: ms_struct reproduces MSVC allocation inside the unit
                bits = ' '.join(f'{ty} {b}:{w};' for b, w in arr)
                lines.append(f'    struct __attribute__((ms_struct)) {{ {bits} }};  // 0x{off:03x} bitfield unit')
                pos = off + sz
                continue
            vn = nm
            if names[nm] > 1 and nm not in own or vn in used:
                vn = sanitize(qname(origin[0]) if isinstance(origin, tuple) else 'b') + '__' + nm
            used.add(vn)
            lines.append(f'    {ty} {vn}{arr};{" " * max(1, 34 - len(ty) - len(vn) - len(arr))}// 0x{off:03x}')
            asserts.append(f'static_assert(offsetof(gv::{q}, {vn}) == 0x{off:x}, "{q}::{vn}");')
            pos = off + sz
        if lay['size'] > pos:
            lines.append(f'    uint8_t __pad{pad}[0x{lay["size"]-pos:x}];')
        # nested enums used anywhere
        nested = []
        for e in d.get_children():
            if e.kind == CK.ENUM_DECL and e.spelling:
                nested.append(self.enum_text(e, '    '))
            elif e.kind in RECS and not anon(e.spelling) and '<' not in e.type.get_canonical().spelling:
                ed = rec_decl(e.type) or e
                self._fwdc[qname(ed)] = ed
                nested.append(f'    typedef gv::{qname(ed)} {e.spelling};')
        size_note = ''
        cj = self.classes.get(qname(d, True))
        if cj and cj.get('size') and int(str(cj['size']), 0) != lay['size']:
            self.notes.append(f'SIZE MISMATCH {q}: clang MSVC layout {lay["size"]:#x} vs classes.json {cj["size"]}')
            size_note = f'  /* classes.json says {cj["size"]} */'
        vt = f'  /* guest vftable 0x{cj["vtable"]["va"]} */' if cj and cj.get('vtable') else ''
        kw = 'struct'
        body = '\n'.join(nested + lines)
        asserts.append(f'static_assert(sizeof(gv::{q}) == 0x{lay["size"]:x}, "sizeof {q}");{size_note}')
        return q, f'{kw} {q.split("::")[-1]} {{{vt}\n{body}\n}};', asserts

    def enum_text(self, e, ind=''):
        vals = ', '.join(f'{k.spelling} = {k.enum_value}' for k in e.get_children() if k.kind == CK.ENUM_CONSTANT_DECL)
        return f'{ind}enum {"" if anon(e.spelling) else e.spelling + " "}: int32_t {{ {vals} }};'

    # ---------------------------------------------------------------- bodies
    def nname(self, c):
        va = self.va_of(c)
        if va: return 'N_' + sanitize(qname(c)) + '_' + va
        import zlib   # operator+ / operator- sanitize to the same name: disambiguate by mangling
        return 'N_' + sanitize(qname(c)) + '_inl%04x' % (zlib.crc32((self._mangled(c) or qname(c)).encode()) & 0xffff)

    def translate(self, c, swapped=False):
        """Translate function definition c into a native function (string)."""
        m = c.mangled_name
        if m in self.native: return self.native[m]
        name = self.nname(c)
        self.native[m] = name
        n_out, n_proto = len(self.out_funcs), len(self.protos)
        saved = (self.cur, self.cls, self.fr, self.top_calls)
        if swapped:
            va = self.va_of(c)
            self.top_calls = {k[1] for k, _, _, _ in orig_sites(va)} if va else None
        try:
            self._translate(c, name, swapped)
        except BaseException:
            # nothing of a failed translation stays behind (ctors fall back to a call by VA)
            del self.out_funcs[n_out:]; del self.protos[n_proto:]
            self.native.pop(m, None)
            raise
        finally:
            self.cur, self.cls, self.fr, self.top_calls = saved
        return name

    def _translate(self, c, name, swapped):
        if not self.pp and self.uses_macro(c): raise NeedPP(qname(c))
        if c.kind == CK.FUNCTION_TEMPLATE or mentions_template_params(c):
            raise Unsupported('function template instantiation (the body is the dependent pattern)')
        self.cur = c
        ctor, dtor = c.kind == CK.CONSTRUCTOR, c.kind == CK.DESTRUCTOR
        self.cls = c.semantic_parent if (c.kind == CK.CXX_METHOD and not c.is_static_method()) or ctor or dtor else None
        if c.type.is_function_variadic(): raise Unsupported('variadic')
        self.fr = Frame()
        self.fr.esc = self.escapes(c)
        params, pre = [], []
        if self.cls is not None:
            self.want(self.cls)
            params.append(f'gv::{qname(self.cls)} *self')
        for a in c.get_arguments():
            nm = a.spelling or "_a%d" % len(params)
            # a by-value record argument of a swapped function stays in the guest frame (the
            # callee owns that copy under the MSVC ABI), so its address may be passed on
            byrec = a.type.get_canonical().kind == TK.RECORD
            amp = ' &' if swapped and byrec else ''
            if a.hash in self.fr.esc and not amp and a.type.get_canonical().kind != TK.LVALUEREFERENCE:
                # address of a by-value parameter reaches guest code: copy it into the guest frame
                params.append(f'{self.ntype(a.type)} __p_{nm}')
                pre.append(self.frame_decl(a.type, nm) + f' cjg::spill(&{nm}, __p_{nm});')
            else:
                params.append(f'{self.ntype(a.type, True)}{amp} {nm}')    # a const & parameter must accept const arguments
        ret = self.ntype(c.result_type)
        body = next((ch for ch in c.get_children() if ch.kind == CK.COMPOUND_STMT), None)
        if body is None: raise Unsupported('no body')
        bsrc, bs, be, _ = self.text(body)
        if bsrc[bs:bs + 1] != '{' or bsrc[be - 1:be] != '}':
            # the extent of a macro-generated body is the macro invocation: splicing it would paste the
            # unexpanded invocation into the generated file
            if not self.pp: raise NeedPP(qname(c))
            raise Unsupported('function generated by a macro (not recoverable from the preprocessed unit)')
        if ctor: pre += self.ctor_prologue(c)
        if dtor: pre += self.dtor_vptr(self.cls)
        btxt = self.emit(body)
        usings = self.scope_aliases(body)
        if usings: btxt = '{ ' + ' '.join(usings) + btxt[1:]
        post = self.dtor_epilogue(self.cls) if dtor else []
        if self.fr.size or pre or post:
            # a swap target's frame goes into the original's local area when it fits (cjg::Frame::Area)
            fl = [f'    cjg::Frame F(0x{self.fr.size:x}, {self.fr.align}{", cjg::Frame::Area{}" if swapped else ""});'] if self.fr.size else []
            if post:   # members/bases are destroyed after the body, also on early return
                fl.append('    auto __dtor_members = cjg::defer([&] { ' + ' '.join(post) + ' });')
            btxt = '{\n' + '\n'.join(fl + ['    ' + x for x in pre]) + '\n' + btxt + '\n}'
        if swapped: btxt = self.resolve_sites(btxt, self.va_of(c))   # ctor prologue + body + dtor epilogue, in text order
        src, s, e, fname = self.text(c)
        hdr = f'// {qname(c)}  VA {self.va_of(c) or "(inline, no VA)"}  {os.path.relpath(self.src_path if self.pp else fname, DECOMP)}:{c.location.line}{" (preprocessed)" if self.pp else ""}'
        self.protos.append(f'static {ret} {name}({", ".join(params)});')
        self.out_funcs.append(f'{hdr}\nstatic {ret} {name}({", ".join(params)})\n{btxt}\n')
        return name

    def scope_aliases(self, body):
        """The body of a member function was written inside the class scope, where nested types
        (and typedefs) of the class and its bases are visible by their bare names. The generated
        free function is not in that scope: alias each one the body names. File-scope typedefs
        (StateFn, EXListApplyFunc) are aliased the same way, to their guest-side spelling."""
        out, seen = [], set()
        for n in body.walk_preorder():
            if n.kind != CK.TYPE_REF: continue
            r = n.referenced
            if r is None or r.spelling in seen: continue
            par = r.semantic_parent
            if r.kind in (CK.TYPEDEF_DECL, CK.TYPE_ALIAS_DECL):
                if par is not None and par.kind in RECS and self.cls is None: continue
                if r.spelling in PCJ_TYPEDEF_NAMES: continue
                try: ut = self.ntype(r.underlying_typedef_type)
                except Unsupported: continue
                seen.add(r.spelling)
                out.append(f'using {r.spelling} = {ut};')
            elif self.cls is not None and par is not None and par.kind in RECS and r.kind in RECS and \
                    not anon(r.spelling) and self.outer_rec(r) is not None:
                d = rec_decl(r.type) or r
                self.want(d); seen.add(r.spelling)
                out.append(f'using {r.spelling} = gv::{qname(d)};')
        return out

    def splice(self, c):
        src, s, e, f = self.text(c)
        out, pos = [], s
        for ch in sorted(c.get_children(), key=lambda x: x.extent.start.offset):
            if ch.extent.start.file is None or ch.extent.start.file.name != f: continue
            cs, ce = ch.extent.start.offset, ch.extent.end.offset
            if cs < pos or ce > e or ce <= cs: continue
            out.append(src[pos:cs]); out.append(self.emit(ch)); pos = ce
        out.append(src[pos:e])
        return ''.join(out)

    def emit(self, c):
        k = c.kind
        if c.extent.start.file is not None and c.location.file is not None and \
                c.extent.start.file.name != c.location.file.name:
            raise Unsupported('macro expansion in body')
        if k == CK.CXX_THIS_EXPR: return 'self'
        if k == CK.STRING_LITERAL:
            gl = guest_literal(c)
            if gl: return gl
        if k == CK.ARRAY_SUBSCRIPT_EXPR or (k == CK.UNARY_OPERATOR and [t.spelling for t in c.get_tokens()][:1] == ['*']):
            # indexing / dereferencing a GuestPtr<T> needs a complete T (sizeof) in the generated file
            er = rec_decl(c.type)
            if er is not None: self.want(er)
        if k == CK.INTEGER_LITERAL:
            tk = [t.spelling for t in c.get_tokens()]
            if len(tk) == 1 and tk[0].lower().endswith(('i64', 'i32', 'i16', 'i8')):
                lit = tk[0]; u = lit.lower().endswith(('ui64', 'ui32', 'ui16', 'ui8'))
                body = lit[:-4] if u and lit.lower().endswith(('ui64',)) else lit
                import re as _re
                m = _re.match(r'^(.*?)(u?i(64|32|16|8))$', lit, _re.I)
                return m.group(1) + ('ULL' if u else 'LL') if m.group(3) == '64' else m.group(1) + ('u' if u else '')
        if k == CK.UNARY_OPERATOR:
            kids = list(c.get_children())
            toks = [t.spelling for t in c.get_tokens()][:1]
            if toks == ['&'] and len(kids) == 1:
                ko = strip(kids[0])
                if ko is not None and ko.kind == CK.DECL_REF_EXPR and ko.referenced is not None and \
                        ko.referenced.kind in (CK.FUNCTION_DECL, CK.CXX_METHOD):
                    return self.emit(ko)
        if k in (CK.CSTYLE_CAST_EXPR, CK.CXX_STATIC_CAST_EXPR, CK.CXX_REINTERPRET_CAST_EXPR) and \
                c.type.get_canonical().kind == TK.POINTER:
            r = self.pointer_cast(c)
            if r is not None: return r
        if k in (CK.CSTYLE_CAST_EXPR, CK.CXX_STATIC_CAST_EXPR, CK.CXX_REINTERPRET_CAST_EXPR) and \
                c.type.get_canonical().kind in (TK.INT, TK.UINT, TK.LONG, TK.ULONG):
            kids = [x for x in c.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
            if len(kids) == 1 and kids[0].type.get_canonical().kind == TK.POINTER:
                # the integer value of a pointer is its guest address (a host address outside the window aborts)
                return f'(({BUILTIN[c.type.get_canonical().kind]})cjg::to_guest({self.emit(kids[0])}))'
        if k == CK.DECL_STMT:
            r = self.decl_stmt(c)
            if r is not None: return r
        if k in (CK.CSTYLE_CAST_EXPR, CK.CXX_STATIC_CAST_EXPR) and c.type.get_canonical().kind == TK.RECORD:
            n = c
            for _ in range(4):
                kids = [x for x in n.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
                if len(kids) != 1: break
                n = kids[0]
                if n.kind == CK.CALL_EXPR and n.referenced is not None and n.referenced.kind == CK.CONSTRUCTOR:
                    return self.temp_ctor(n)
            raise Unsupported('cast to a record type without a converting constructor')
        if k == CK.CXX_FUNCTIONAL_CAST_EXPR and c.type.get_canonical().kind == TK.RECORD:
            cc = [x for x in c.get_children() if x.kind == CK.CALL_EXPR and x.referenced is not None
                  and x.referenced.kind == CK.CONSTRUCTOR]
            if cc: return self.temp_ctor(cc[0])
            n = c
            for _ in range(4):
                kids = [x for x in n.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
                if len(kids) != 1: break
                n = kids[0]
                if n.kind == CK.CALL_EXPR and n.referenced is not None and n.referenced.kind == CK.CONSTRUCTOR:
                    return self.temp_ctor(n)
            raise Unsupported('functional cast to a record type without a visible constructor call')
        if k == CK.UNEXPOSED_EXPR and c.referenced is not None and c.referenced.kind == CK.CONSTRUCTOR:
            kids = list(c.get_children())
            if len(kids) == 1: return self.emit(kids[0])
        if k == CK.CALL_EXPR and c.referenced is not None and c.referenced.kind == CK.CONSTRUCTOR:
            return self.temp_ctor(c)
        if k == CK.MEMBER_REF_EXPR and c.referenced is not None and c.referenced.kind == CK.FIELD_DECL:
            kids = list(c.get_children())
            if not kids: return 'self->' + c.spelling
            bt = kids[0].type.get_canonical()
            self.want(pointee_rec(bt) if bt.kind == TK.POINTER else rec_decl(bt))
            self.want(c.referenced.semantic_parent)
            return self.splice(c)
        if k == CK.UNEXPOSED_EXPR:
            kids = list(c.get_children())
            if len(kids) == 1:
                to, fr = pointee_rec(c.type), pointee_rec(kids[0].type)
                if to is not None and fr is not None and qname(to) != qname(fr) and \
                        c.type.get_canonical().kind == TK.POINTER:
                    off = self.layout.base_offset(rec_name(fr.type), rec_name(to.type))
                    self.want(to)
                    return f'cjg::up<gv::{qname(to)}>({self.emit(kids[0])}, {off})'
                # derived -> base conversion of a record lvalue (reference binding)
                to, fr = rec_decl(c.type), rec_decl(kids[0].type)
                if to is not None and fr is not None and qname(to) != qname(fr):
                    off = self.layout.base_offset(rec_name(fr.type), rec_name(to.type))
                    self.want(to)
                    ke = self.emit(kids[0])
                    if not is_lvalue(kids[0], self): ke = f'cjg::spill({self.slot(kids[0].type)}, {ke})'   # a prvalue has no address
                    return f'(*cjg::up<gv::{qname(to)}>(&({ke}), {off}))'
        if k == CK.CALL_EXPR and c.referenced is not None:
            return self.call(c)
        if k == CK.CALL_EXPR and c.referenced is None:
            kids = list(c.get_children())
            ce = strip(kids[0]) if kids else None
            ct = ce.type.get_canonical() if ce is not None else None
            if ct is not None and ((ct.kind == TK.POINTER and ct.get_pointee().get_canonical().kind in (TK.FUNCTIONPROTO, TK.FUNCTIONNOPROTO))
                                   or ct.kind in (TK.FUNCTIONPROTO, TK.FUNCTIONNOPROTO)) and \
                    not (ce.kind == CK.DECL_REF_EXPR and ce.referenced is not None and ce.referenced.kind == CK.FUNCTION_DECL):
                # call through a function pointer (a guest address): splicing it would call a host function
                raise Unsupported('call to a function pointer (no callee declaration)')
        if k == CK.DECL_REF_EXPR and c.referenced is not None:
            r = c.referenced
            if r.kind == CK.VAR_DECL and (r.semantic_parent.kind != CK.FUNCTION_DECL and
                                          r.semantic_parent.kind != CK.CXX_METHOD and
                                          r.semantic_parent.kind not in (CK.COMPOUND_STMT,)) and \
                    r.storage_class != ci.StorageClass.NONE or (r.kind == CK.VAR_DECL and r.semantic_parent.kind in (CK.CLASS_DECL, CK.STRUCT_DECL, CK.NAMESPACE, CK.TRANSLATION_UNIT)):
                return self.global_ref(r)
            if r.kind == CK.ENUM_CONSTANT_DECL:
                return f'(({self.ntype(r.semantic_parent.type) if r.semantic_parent.spelling else "int32_t"}){r.enum_value})'
            if r.kind == CK.FUNCTION_DECL or (r.kind == CK.CXX_METHOD and r.is_static_method()):
                # address of a function: a function pointer is its guest entry VA (ntype maps function
                # pointers to uint32_t, calls through one are refused)
                va = self.va_of(r)
                if va: return f'((uint32_t)0x{va}u)'
                raise Unsupported(f'function address taken ({qname(r)} has no VA)')
            if r.kind == CK.CXX_METHOD:
                raise Unsupported('member function address taken')
        if k in (CK.CXX_NEW_EXPR, CK.CXX_DELETE_EXPR) and '4' not in GAPS: raise Unsupported('new/delete')
        if k == CK.CXX_NEW_EXPR: return self.new_expr(c)
        if k == CK.CXX_DELETE_EXPR: return self.delete_expr(c)
        if k in (CK.CXX_DYNAMIC_CAST_EXPR, CK.CXX_TYPEID_EXPR, CK.CXX_TRY_STMT, CK.CXX_THROW_EXPR):
            raise Unsupported(k.name)
        if k == CK.VAR_DECL:
            t = c.type.get_canonical()
            if t.kind == TK.RECORD:
                d = rec_decl(t)
                if any(x.kind == CK.CONSTRUCTOR and not x.is_default_constructor() is False for x in []): pass
                self.want(d)
            if c.storage_class == ci.StorageClass.STATIC: raise Unsupported('function-local static')
        if k == CK.TYPE_REF:
            d = rec_decl(c.type) if c.type.get_canonical().kind == TK.RECORD else None
            if d is not None: self.want(d)
            elif c.type.get_canonical().kind == TK.ENUM: self.want_enum(c.type.get_canonical().get_declaration())
        if k == CK.UNARY_OPERATOR:
            # &local passed anywhere: host address; only allowed when it stays native (checked at runtime)
            pass
        return self.splice(c)

    # ================================================================ gap 1: guest frame
    def frame_decl(self, t, nm):
        """`T &nm = *F.at<T>(off);` -- a local/param that lives on the guest stack, in guest
        layout (pointers as GuestPtr), viewed through the generated view."""
        ty, arr = self.field_type(t, nm)
        off = self.fr.alloc(t)
        if arr: return f'{ty} (&{nm}){arr} = *F.at<{ty}{arr}>(0x{off:x});'
        return f'{ty} &{nm} = *F.at<{ty}>(0x{off:x});'

    def slot(self, t):
        """guest-frame slot pointer for a temporary of type t (sret target, spilled prvalue)"""
        t = t.get_canonical()
        ty, arr = self.field_type(t, 'tmp')
        if arr: raise Unsupported('array temporary')
        return f'F.at<{ty}>(0x{self.fr.alloc(t):x})'

    def escapes(self, fn):
        """Locals/params of fn whose address can reach guest code: operand of unary &, bound
        to a reference parameter, object of a member call (incl. member operators), initializer
        of a reference variable, array-typed, or a record with a non-trivial ctor/dtor (its
        constructor sees `this`).  Conservative: everything else may stay a host local."""
        esc = set()
        local = {}
        for x in fn.walk_preorder():
            if x.kind in (CK.VAR_DECL, CK.PARM_DECL): local[x.hash] = x
        def mark(e):
            r = lvalue_root(e)
            if r is not None and r.hash in local: esc.add(r.hash)
        for x in fn.walk_preorder():
            k = x.kind
            if k == CK.UNARY_OPERATOR:
                toks = [t.spelling for t in x.get_tokens()]
                if toks and toks[0] == '&': mark(next(iter(x.get_children()), None))
            elif k == CK.CALL_EXPR and x.referenced is not None:
                f = x.referenced
                if f.kind not in (CK.CXX_METHOD, CK.FUNCTION_DECL, CK.CONSTRUCTOR, CK.CONVERSION_FUNCTION): continue
                args = list(x.get_arguments())
                params = list(f.get_arguments())
                if f.kind in (CK.CXX_METHOD, CK.CONVERSION_FUNCTION) and not f.is_static_method():
                    if f.spelling.startswith('operator') and len(args) == len(params) + 1:
                        if not (f.spelling == 'operator=' and self.implicit(f)): mark(args[0])
                        args = args[1:]
                    else:
                        kids = list(x.get_children())
                        if kids:
                            mr = next((y for y in kids[0].walk_preorder() if y.kind == CK.MEMBER_REF_EXPR), None)
                            ob = [y for y in mr.get_children() if y.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)] if mr else []
                            if ob and ob[0].type.get_canonical().kind != TK.POINTER: mark(ob[0])
                for i, a in enumerate(args):
                    pt = params[i].type.get_canonical() if i < len(params) else a.type.get_canonical()
                    if pt.kind == TK.LVALUEREFERENCE and not (f.spelling == 'operator=' and self.implicit(f)) \
                            and not (f.kind == CK.CONSTRUCTOR and self.implicit(f)):
                        mark(a)
            elif k == CK.VAR_DECL:
                t = x.type.get_canonical()
                if t.kind == TK.LVALUEREFERENCE:
                    kids = [y for y in x.get_children() if y.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
                    if kids: mark(kids[-1])
                elif t.kind in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY):
                    esc.add(x.hash)
                elif t.kind == TK.RECORD:
                    d = rec_decl(t)
                    ctor = self.init_ctor(x)
                    if not self.ctor_noop(d, ctor) or not self.dtor_noop(d): esc.add(x.hash)
            elif k == CK.RETURN_STMT and fn.result_type.get_canonical().kind == TK.LVALUEREFERENCE:
                mark(next(iter(x.get_children()), None))
        return esc

    def init_ctor(self, v):
        """(constructor cursor or None, init expr or None, args) for VAR_DECL v of record type"""
        kids = [y for y in v.get_children() if y.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
        if not kids: return None
        e = strip(kids[-1])
        if e.kind == CK.CALL_EXPR and e.referenced is not None and e.referenced.kind == CK.CONSTRUCTOR:
            return e.referenced
        return None

    # ---- triviality (what MSVC would emit for a ctor/dtor)
    def implicit(self, f):
        # An implicitly declared member sits at its class's own location. clang gives an
        # implicitly *defined* copy constructor an empty COMPOUND_STMT body (the memberwise
        # copy is not a statement), so the body test alone called it user-written and
        # ctor_noop then dropped the copy (FEObject::MathmosUpdate, docs/determinism.md).
        p = f.semantic_parent
        if p is not None and f.location.file is not None and p.location.file is not None and \
                f.location.file.name == p.location.file.name and \
                (f.location.line, f.location.column) == (p.location.line, p.location.column):
            return True
        d = self.defs.get(self._mangled(f)) or f.get_definition()
        if d is None: return f.location.file is None or not self._declared_in_source(f)
        return not any(ch.kind == CK.COMPOUND_STMT for ch in d.get_children())

    def _mangled(self, f):
        try: return f.mangled_name
        except Exception: return None

    def _declared_in_source(self, f):
        # implicit members point at the class location; a user declaration has its own tokens
        toks = [t.spelling for t in f.get_tokens()][:4]
        return f.spelling.split('<')[0].lstrip('~') in ''.join(toks) or (toks and toks[0] == '~')

    def has_vfptr(self, d):
        lay = self.layout.get(rec_name(d.type))
        if lay is None: raise Unsupported(f'no layout for {qname(d)}')
        def lv(l): return bool(l['vfptr']) or any(lv(self.layout.get(b)) for b, _ in l['bases'] if self.layout.get(b))
        return lv(lay)

    def rec_members(self, d):
        for f in self.own_fields(d):
            if f.type.get_canonical().kind == TK.RECORD:
                yield f, rec_decl(f.type.get_canonical())

    def bases(self, d):
        out = []
        for bname, bd in self.base_decls(d):
            if bd is None: raise Unsupported(f'base {bname} of {qname(d)} not resolved')
            out.append(bd)
        return out

    def default_ctor(self, d):
        return next((x for x in d.get_children() if x.kind == CK.CONSTRUCTOR and x.is_default_constructor()), None)

    def find_dtor(self, d):
        return next((x for x in d.get_children() if x.kind == CK.DESTRUCTOR), None)

    def ctor_noop(self, d, ctor):
        """True if constructing d with ctor does nothing observable (no vptr, empty bodies)"""
        if ctor is not None and ctor.is_copy_constructor() and self.implicit(ctor): return True
        if self.has_vfptr(d): return False
        if ctor is not None and not self.implicit(ctor):
            body = self.ctor_def(ctor)
            if body is None: return False
            cb = next(ch for ch in body.get_children() if ch.kind == CK.COMPOUND_STMT)
            if list(cb.get_children()) or self.init_list(body): return False
        return all(self.ctor_noop(b, self.default_ctor(b)) for b in self.bases(d)) and \
            all(self.ctor_noop(m, self.default_ctor(m)) for _, m in self.rec_members(d))

    def dtor_noop(self, d):
        dt = self.find_dtor(d)
        if dt is not None and not self.implicit(dt):
            body = self.ctor_def(dt)
            if body is None: return False
            cb = next(ch for ch in body.get_children() if ch.kind == CK.COMPOUND_STMT)
            if list(cb.get_children()) or self.has_vfptr(d): return False
        return all(self.dtor_noop(b) for b in self.bases(d)) and all(self.dtor_noop(m) for _, m in self.rec_members(d))

    def ctor_def(self, f):
        d = self.defs.get(self._mangled(f)) or f.get_definition()
        if d is None or not any(ch.kind == CK.COMPOUND_STMT for ch in d.get_children()): return None
        return d

    def init_list(self, c):
        """[(kind 'base'|'member', target cursor, init expr)] of constructor definition c"""
        kids = list(c.get_children())
        last_parm = max([i for i, k in enumerate(kids) if k.kind == CK.PARM_DECL], default=0)
        out, i = [], last_parm + 1 if any(k.kind == CK.PARM_DECL for k in kids) else 1
        while i < len(kids) and kids[i].kind != CK.COMPOUND_STMT:
            k = kids[i]
            if k.kind == CK.MEMBER_REF and i + 1 < len(kids):
                out.append(('member', k.referenced, kids[i + 1])); i += 2; continue
            if k.kind in (CK.TYPE_REF, CK.TEMPLATE_REF) and i + 1 < len(kids) and kids[i + 1].kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF, CK.COMPOUND_STMT):
                out.append(('base', rec_decl(k.type) if k.kind == CK.TYPE_REF else None, kids[i + 1])); i += 2; continue
            i += 1
        return out

    # ---- construction / destruction statements
    def ctor_stmt(self, obj, d, ctor, args):
        """statement constructing record d at host view pointer expression obj"""
        self.want(d)
        for x in args:
            sx = strip(x)
            if sx is not None and sx.kind == CK.STRING_LITERAL and not guest_literal(sx):
                raise Unsupported('string literal argument to a constructor (host data cannot be passed)')
        if ctor is None or self.implicit(ctor):
            if ctor is not None and ctor.is_copy_constructor() and args:
                return f'cjg::assign({obj}, {self.emit(args[0])});'
            if ctor is not None and len(args) > 0: raise Unsupported(f'implicit constructor {qname(d)} with arguments')
            return self.synth_default(obj, d)
        if self.ctor_noop(d, ctor): return ''
        args = self.fix_args(ctor, args)
        body = self.ctor_def(ctor)
        if body is not None and not self.orig_calls(self.va_of(ctor)):
            try:
                n = self.translate(body)
                return f'{n}({", ".join([obj] + [self.native_arg(a) for a in args])});'
            except Unsupported:
                if not self.va_of(ctor): raise
        va = self.va_of(ctor)
        if not va: raise Unsupported(f'constructor {qname(ctor)} has no VA and no body')
        fargs = self.call_args(ctor, args)
        return f'cjg::call_this<void>(cjg::Site(0x{va}u, {self.site(("d", int(va, 16)))}), {obj}{"".join(", " + a for a in fargs)});'

    def synth_default(self, obj, d):
        """implicit default constructor: bases, guest vptr, record members (MSVC order)"""
        out = []
        for b in self.bases(d):
            off = self.layout.base_offset(rec_name(d.type), rec_name(b.type))
            self.want(b)
            out.append(self.ctor_stmt(f'cjg::up<gv::{qname(b)}>({obj}, {off})', b, self.default_ctor(b), []))
        out += self.vptr_stmt(obj, d)
        for f, m in self.rec_members(d):
            out.append(self.ctor_stmt(f'&({obj})->{f.spelling}', m, self.default_ctor(m), []))
        return ' '.join(x for x in out if x)

    def vptr_stmt(self, obj, d):
        """MSVC writes the class's own guest vftable VA after the bases are constructed (gap 4)"""
        if not self.has_vfptr(d): return []
        lay = self.layout.get(rec_name(d.type))
        cj = self.classes.get(qname(d, True))
        if not cj or not cj.get('vtable'): raise Unsupported(f'guest vftable of {qname(d)} not in classes.json')
        allv = []
        self.flat_fields(d, 0, allv, set())
        if any(f[1].startswith('__vfptr_') for f in allv): raise Unsupported(f'{qname(d)}: secondary vfptr in ctor')
        return [f'({obj})->__vfptr = 0x{cj["vtable"]["va"]}u;']

    def dtor_stmt(self, obj, d):
        if self.dtor_noop(d): return ''
        dt = self.find_dtor(d)
        if dt is None or self.implicit(dt):
            return ' '.join(x for x in self.member_base_dtors(obj, d) if x)
        body = self.ctor_def(dt)
        if body is not None and not self.orig_calls(self.va_of(dt)):
            try:
                return f'{self.translate(body)}({obj});'
            except Unsupported:
                if not self.va_of(dt): raise
        va = self.va_of(dt)
        if not va: raise Unsupported(f'destructor {qname(dt)} has no VA and no body')
        return f'cjg::call_this<void>(cjg::Site(0x{va}u, {self.site(("d", int(va, 16)))}), {obj});'

    def member_base_dtors(self, obj, d):
        out = [self.dtor_stmt(f'&({obj})->{f.spelling}', m) for f, m in reversed(list(self.rec_members(d)))]
        for b in reversed(self.bases(d)):
            off = self.layout.base_offset(rec_name(d.type), rec_name(b.type))
            self.want(b)
            out.append(self.dtor_stmt(f'cjg::up<gv::{qname(b)}>({obj}, {off})', b))
        return out

    def ctor_prologue(self, c):
        d = self.cls
        il = self.init_list(c)
        out = []
        for b in self.bases(d):
            off = self.layout.base_offset(rec_name(d.type), rec_name(b.type))
            self.want(b)
            ent = next((e for k, t, e in il if k == 'base' and t is not None and qname(t) == qname(b)), None)
            o = f'cjg::up<gv::{qname(b)}>(self, {off})'
            if ent is not None:
                ce = strip(ent)
                if ce.kind == CK.CALL_EXPR and ce.referenced is not None and ce.referenced.kind == CK.CONSTRUCTOR:
                    out.append(self.ctor_stmt(o, b, ce.referenced, list(ce.get_arguments())))
                else: raise Unsupported('base initializer shape')
            else:
                out.append(self.ctor_stmt(o, b, self.default_ctor(b), []))
        if any(k == 'base' and t is None for k, t, e in il): raise Unsupported('template base initializer')
        out += self.vptr_stmt('self', d)
        mem = {t.hash: e for k, t, e in il if k == 'member' and t is not None}
        for f in d.get_children():
            if f.kind != CK.FIELD_DECL: continue
            e = mem.get(f.hash)
            ft = f.type.get_canonical()
            if ft.kind == TK.RECORD:
                m = rec_decl(ft)
                if e is not None:
                    ce = strip(e)
                    if ce.kind == CK.CALL_EXPR and ce.referenced is not None and ce.referenced.kind == CK.CONSTRUCTOR:
                        out.append(self.ctor_stmt(f'&self->{f.spelling}', m, ce.referenced, list(ce.get_arguments())))
                    else:
                        out.append(f'cjg::assign(&self->{f.spelling}, {self.emit(e)});')
                else:
                    out.append(self.ctor_stmt(f'&self->{f.spelling}', m, self.default_ctor(m), []))
            elif e is not None:
                if ft.kind in (TK.CONSTANTARRAY,): raise Unsupported('array member initializer')
                out.append(f'self->{f.spelling} = {self.emit(e)};')
        return [x for x in out if x]

    def dtor_vptr(self, d):
        return self.vptr_stmt('self', d)

    def dtor_epilogue(self, d):
        return [x for x in self.member_base_dtors('self', d) if x]

    def decl_stmt(self, c):
        """DECL_STMT with guest-frame locals or constructed records: one statement per variable"""
        vs = [v for v in c.get_children() if v.kind == CK.VAR_DECL]
        if not vs or self.fr is None: return None
        special = any(v.hash in self.fr.esc or v.type.get_canonical().kind == TK.RECORD for v in vs)
        if not special: return None
        out = []
        for v in vs:
            if v.storage_class == ci.StorageClass.STATIC: raise Unsupported('function-local static')
            t = v.type.get_canonical()
            kids = [y for y in v.get_children() if y.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
            init = kids[-1] if kids else None
            # implicit wrappers whose extent starts at the declarator name would splice `v = `
            while init is not None and init.kind == CK.UNEXPOSED_EXPR and len(list(init.get_children())) == 1 \
                    and init.extent.start.offset <= v.location.offset:
                init = next(iter(init.get_children()))
            if init is not None and strip(init).kind == CK.CXX_FUNCTIONAL_CAST_EXPR and t.kind == TK.RECORD:
                cc = [x for x in strip(init).get_children() if x.kind == CK.CALL_EXPR and x.referenced is not None
                      and x.referenced.kind == CK.CONSTRUCTOR]
                if cc: init = cc[0]     # T v = T(args): constructed in place (copy elided)
            esc = v.hash in self.fr.esc
            nm = v.spelling
            if t.kind == TK.RECORD:
                d = rec_decl(t); self.want(d)
                if esc: out.append(self.frame_decl(t, nm))
                else: out.append(f'gv::{qname(d)} {nm};')
                ce = strip(init) if init is not None else None
                if ce is not None and ce.kind == CK.CALL_EXPR and ce.referenced is not None and ce.referenced.kind == CK.CONSTRUCTOR:
                    out.append(self.ctor_stmt(f'&{nm}', d, ce.referenced, list(ce.get_arguments())))
                elif init is not None:
                    # copy-initialisation from a prvalue (C++17: elided, no copy ctor runs)
                    out.append(f'cjg::assign(&{nm}, {self.emit(init)});')
                else:
                    out.append(self.ctor_stmt(f'&{nm}', d, self.default_ctor(d), []))
                dt = self.dtor_stmt(f'&{nm}', d)
                if dt: out.append(f'auto __dtor_{nm} = cjg::defer([&] {{ {dt} }});')
            elif esc:
                if t.kind == TK.LVALUEREFERENCE:
                    out.append(f'{self.ntype(v.type)} {nm} = {self.emit(init)};')
                    continue
                if t.kind in (TK.CONSTANTARRAY,):
                    et = t.element_type.get_canonical()
                    if et.kind == TK.RECORD and not (self.ctor_noop(rec_decl(et), self.default_ctor(rec_decl(et))) and self.dtor_noop(rec_decl(et))):
                        raise Unsupported('array of constructed records')
                    if init is not None and et.kind == TK.RECORD: raise Unsupported('initialised local array of records')
                out.append(self.frame_decl(t, nm))
                if init is not None and t.kind in (TK.CONSTANTARRAY,):
                    ty, arr = self.field_type(t, nm)
                    out.append(f'{{ const {ty} __i_{nm}{arr} = {self.emit(init)}; std::memcpy(&{nm}, &__i_{nm}, sizeof __i_{nm}); }}')
                elif init is not None: out.append(f'{nm} = {self.emit(init)};')
            else:
                if t.kind in (TK.CONSTANTARRAY,): raise Unsupported('host local array')
                out.append(f'{self.ntype(v.type)} {nm}' + (f' = {self.emit(init)};' if init is not None else ';'))
        return ' '.join(out)

    def temp_ctor(self, c):
        """a constructed temporary: built in a guest-frame slot, yields an lvalue view"""
        f = c.referenced
        d = rec_decl(c.type)
        if d is None: raise Unsupported('temporary of non-record type')
        if any(n.kind == CK.STRING_LITERAL and not guest_literal(n) for n in c.walk_preorder()):
            raise Unsupported('string literal argument to a constructor (host data cannot be passed)')
        # a temporary built straight into a by-value argument slot is destroyed by the callee (MSVC): allowed
        byval_arg = getattr(self, '_byval_arg', None)
        if not self.dtor_noop(d) and not (byval_arg is not None and c.extent == byval_arg.extent and c.hash == byval_arg.hash):
            raise Unsupported(f'temporary {qname(d)} with a destructor at line {c.location.line}')
        args = list(c.get_arguments())
        if f.is_copy_constructor() and self.implicit(f) and args:
            return self.emit(args[0])
        self.want(d)
        sl = self.slot(c.type)
        st = self.ctor_stmt('__t', d, f, args)
        return f'(*([&]() -> gv::{qname(d)} * {{ gv::{qname(d)} *__t = {sl}; {st} return __t; }}()))'

    # ================================================================ gap 4: new / delete
    def find_op(self, d, name, nargs, ptr_arg=False):
        """class-specific operator new/delete in d's hierarchy, else the global one in the TU"""
        c = d
        while c is not None:
            for x in c.get_children():
                if x.kind == CK.CXX_METHOD and x.spelling == name and len(list(x.get_arguments())) == nargs: return x
            bs = self.bases(c); c = bs[0] if bs else None
        for x in self.tu.cursor.get_children():
            if x.kind == CK.FUNCTION_DECL and x.spelling == name and len(list(x.get_arguments())) == nargs:
                a = list(x.get_arguments())
                if nargs == 2 and ptr_arg != (a[1].type.get_canonical().kind == TK.POINTER): continue
                return x
        raise Unsupported(f'{name}/{nargs} not found')

    def call_fn(self, f, argexprs, ret='void *'):
        # the inline operator new/delete of EXMemory.h is what the original inlined: translate it
        # (it calls _EXAlloc / EXFree by VA); otherwise call the out-of-line copy by VA
        d = self.ctor_def(f)
        if d is not None and not self.orig_calls(self.va_of(f)):
            try: return f'{self.sub_translate(d)}({", ".join(argexprs)})'
            except Unsupported:
                pass
        va = self.va_of(f)
        if va: return f'cjg::call_cdecl<{ret}>(cjg::Site(0x{va}u, {self.site(("d", int(va, 16)))}){"".join(", " + a for a in argexprs)})'
        raise Unsupported(f'{qname(f)} has no VA and no body')

    def new_expr(self, c):
        pt = c.type.get_canonical().get_pointee().get_canonical()
        toks = [t.spelling for t in c.get_tokens()]
        kids = [k for k in c.get_children() if k.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
        cc = [k for k in kids if strip(k).kind == CK.CALL_EXPR and strip(k).referenced is not None
              and strip(k).referenced.kind == CK.CONSTRUCTOR]
        place = [k for k in kids if k not in cc]
        is_arr = '[' in toks[:toks.index('(') if '(' in toks and toks.index('(') > 1 else len(toks)] and toks[1] != '('
        if is_arr:
            # new T[n]: only element types without construction (no array cookie)
            if pt.kind == TK.RECORD: raise Unsupported('array new of records')
            if len(place) != 1: raise Unsupported('array new shape')
            op = self.find_op(None, 'operator new[]', 1)
            esz = pt.get_size()
            return f'(({self.ntype(pt)} *){self.call_fn(op, [f"(uint32_t)(({self.emit(place[0])}) * {esz}u)"])})'
        if pt.kind != TK.RECORD:
            if place: raise Unsupported('placement new of non-record')
            op = self.find_op(None, 'operator new', 1)
            ini = f' *__p = ({self.emit(kids[0])});' if kids else ''
            nt = self.ntype(pt)
            return f'([&]() -> {nt} * {{ {nt} *__p = ({nt} *){self.call_fn(op, [f"{pt.get_size()}u"])}; if (__p) {{{ini}}} return __p; }}())'
        d = rec_decl(pt); self.want(d)
        q = qname(d)
        sz = self.layout.get(rec_name(d.type))['size']
        if len(place) == 1 and place[0].type.get_canonical().kind == TK.POINTER:
            mem = f'(void *)({self.emit(place[0])})'       # placement new: the inline returns p
        else:
            op = self.find_op(d, 'operator new', 1 + len(place))
            pargs = [f'({BUILTIN.get(p.type.get_canonical().kind, "uint32_t")})({self.emit(p)})' for p in place]
            mem = self.call_fn(op, [f'{sz}u'] + pargs)
        ctor = strip(cc[0]).referenced if cc else self.default_ctor(d)
        args = list(strip(cc[0]).get_arguments()) if cc else []
        st = self.ctor_stmt('__p', d, ctor, args)
        # operator new is the game's allocator (guest heap); the constructor writes the guest vptr
        return f'([&]() -> gv::{q} * {{ gv::{q} *__p = (gv::{q} *){mem}; if (__p) {{ {st} }} return __p; }}())'

    def delete_expr(self, c):
        toks = [t.spelling for t in c.get_tokens()]
        is_arr = len(toks) > 2 and toks[1] == '['
        kids = [k for k in c.get_children()]
        if len(kids) != 1: raise Unsupported('delete shape')
        e = kids[0]
        pt = e.type.get_canonical().get_pointee().get_canonical() if e.type.get_canonical().kind == TK.POINTER else None
        if pt is None: raise Unsupported('delete of non-pointer')
        if is_arr:
            if pt.kind == TK.RECORD: raise Unsupported('array delete of records')
            op = self.find_op(None, 'operator delete[]', 1)
            return f'({self.call_fn(op, [self.emit(e)], "void")})'
        if pt.kind != TK.RECORD:
            op = self.find_op(None, 'operator delete', 1)
            return f'({self.call_fn(op, [self.emit(e)], "void")})'
        d = rec_decl(pt); self.want(d)
        q = qname(d)
        dt = self.find_dtor(d)
        virt = dt is not None and dt.is_virtual_method()
        c2 = d
        while not virt and c2 is not None:
            x = self.find_dtor(c2); virt = x is not None and x.is_virtual_method()
            bs = self.bases(c2); c2 = bs[0] if bs else None
        if virt:
            # MSVC: scalar deleting destructor through the guest vftable, flag 1 (frees)
            c2 = d
            while c2 is not None:
                cj = self.classes.get(qname(c2, True))
                if cj and cj.get('vtable'):
                    sl = [i for i, n in enumerate(cj['vtable']['slots']) if n.startswith('??_G') or n.startswith('??_E')]
                    if sl:
                        return f'([&] {{ gv::{q} *__p = {self.emit(e)}; if (__p) cjg::vcall<void>(__p, 0, cjg::VSlot({sl[0]}, {self.site(("v", 4 * sl[0]))}), (uint32_t)1); }}())'
                bs = self.bases(c2); c2 = bs[0] if bs else None
            raise Unsupported(f'deleting destructor slot of {q} not in classes.json')
        op = self.find_op(d, 'operator delete', 1)
        st = self.dtor_stmt('__p', d)
        return f'([&] {{ gv::{q} *__p = {self.emit(e)}; if (__p) {{ {st} {self.call_fn(op, ["(void *)__p"], "void")}; }} }}())'

    def global_ref(self, r):
        m = r.mangled_name
        row = self.data.get(m)
        va = row and (row.get('va') or row.get('addr'))
        if not va:
            if r.linkage == ci.LinkageKind.INTERNAL:
                raise Unsupported(f'file-static {qname(r)} has no VA (internal linkage, no symbol: {m})')
            raise Unsupported(f'global {qname(r)} has no VA ({m})')
        # guest storage type: pointers are 4-byte GuestPtr<>, arrays keep their extent
        ty, arr = self.field_type(r.type, qname(r))
        return f'(*cjg::g<{ty}{arr}>(0x{va}u))'

    # ---- calls
    def pointer_cast(self, c):
        """Explicit cast to a pointer type. The operand may be a GuestPtr<> (a pointer field), which a
        C-style cast cannot convert to an unrelated pointer type, and a derived/base cast between
        record pointers has to adjust by the MSVC base offset, which the view structs do not model."""
        kids = [x for x in c.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
        if len(kids) != 1: return None
        o = kids[0]
        ft, tt = o.type.get_canonical(), c.type.get_canonical()
        if ft.kind != TK.POINTER: return None
        tp, fp = tt.get_pointee().get_canonical(), ft.get_pointee().get_canonical()
        if tp.kind == TK.FUNCTIONPROTO or fp.kind == TK.FUNCTIONPROTO: return None
        try: tn = self.ntype(tt)
        except Unsupported: return None
        off = 0
        to, fr = pointee_rec(tt), pointee_rec(ft)
        if to is not None and fr is not None and qname(to) != qname(fr) and c.kind != CK.CXX_REINTERPRET_CAST_EXPR:
            self.want(to); self.want(fr)
            try: off = self.layout.base_offset(rec_name(fr.type), rec_name(to.type))        # upcast
            except Unsupported:
                try: off = -self.layout.base_offset(rec_name(to.type), rec_name(fr.type))  # downcast
                except Unsupported: off = 0                                                # unrelated: plain reinterpretation
        return f'cjg::pcast<{tn}>({self.emit(o)}, {off})'

    def this_obj(self, o):
        """Object expression of a member call. A record prvalue (`(a - b).Length()`) has no address:
        materialise it in a guest-frame slot first, so `this` is a guest address."""
        e = self.emit(o)
        if o.type.get_canonical().kind == TK.RECORD and not is_lvalue(o, self):
            e = f'cjg::spill({self.slot(o.type)}, {e})'
        return e

    def noop_call(self, c, f):
        """A call to an inline function whose body is empty (ACTOR_TRACE(...) and the like: retail kept the call
        with its arguments and the strings stripped): evaluate the arguments for their side effects (guest
        calls) and drop the call and any literal. Returns the expression, or None when f is not such a function."""
        if f.kind != CK.FUNCTION_DECL: return None
        d = f.get_definition()
        if d is None: return None
        body = [x for x in d.get_children() if x.kind == CK.COMPOUND_STMT]
        if len(body) != 1 or any(True for _ in body[0].get_children()): return None
        parts = []
        for a in c.get_arguments():
            sa = strip(a)
            if sa is not None and sa.kind in (CK.STRING_LITERAL, CK.INTEGER_LITERAL, CK.FLOATING_LITERAL): continue
            parts.append(f'(void)({self.emit(a)})')
        return '(' + ', '.join(parts + ['(void)0']) + ')'

    def call(self, c):
        f = c.referenced
        if f.kind == CK.CONSTRUCTOR: return self.temp_ctor(c)
        if f.kind not in (CK.CXX_METHOD, CK.FUNCTION_DECL, CK.CONVERSION_FUNCTION):
            raise Unsupported(f'call to {f.kind.name}')
        nc = self.noop_call(c, f)
        if nc is not None: return nc
        args = list(c.get_arguments())
        kids = list(c.get_children())
        is_op = f.spelling.startswith('operator')
        obj, objtype, qualified, arrow = None, None, False, True
        objconst = False
        # a conversion operator (`strcmp(p, s.m_Name)` through EXFixedArray::operator char *) is a member
        # call too: without this it became a cdecl call with no `this`
        if f.kind in (CK.CXX_METHOD, CK.CONVERSION_FUNCTION) and not f.is_static_method():
            if is_op and len(args) == len(list(f.get_arguments())) + 1:
                o = args[0]; args = args[1:]
                obj, objtype = f'&({self.this_obj(o)})', o.type
                so = strip(o)
                objconst = so is not None and so.type.get_canonical().is_const_qualified()
            else:
                mref = None
                for x in kids[0].walk_preorder():
                    if x.kind == CK.MEMBER_REF_EXPR: mref = x; break
                if mref is None: raise Unsupported(f'member call shape {qname(f)}')
                src, s, e, _ = self.text(mref)
                mk = [x for x in mref.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
                seg = src[s:e]
                qualified = '::' in seg.split(f.spelling)[0][-40:] if mk else '::' in seg
                if not mk:
                    obj, objtype = 'self', None
                else:
                    ot = mk[0]
                    arrow = '->' in src[ot.extent.end.offset:e]
                    oe = self.emit(ot) if arrow else self.this_obj(ot)
                    obj = oe if arrow else f'&({oe})'
                    objtype = ot.type.get_canonical().get_pointee() if arrow else ot.type
                    so = strip(ot)
                    if so is not None:
                        st = so.type.get_canonical()
                        objconst = (st.get_pointee().get_canonical().is_const_qualified() if st.kind == TK.POINTER else st.is_const_qualified()) \
                            if (st.kind == TK.POINTER) == arrow else False
            if objconst:
                obj = f'cjg::unconst({obj})'      # const method on a const object: the generated self is not const
            # this adjustment to the method's class
            sc = rec_decl(objtype) if objtype is not None else self.cls
            if objtype is not None and sc is None:
                sc = rec_decl(objtype.get_canonical())
            mc = f.semantic_parent
            off = self.layout.base_offset(rec_name(sc.type), rec_name(mc.type))
            if off: obj = f'cjg::up<gv::{qname(mc)}>({obj}, {off})'
            elif rec_name(sc.type) != rec_name(mc.type): obj = f'cjg::up<gv::{qname(mc)}>({obj}, 0)'   # views are flat structs: a base at +0 still needs the typed pointer for a native callee
            self.want(mc); self.want(sc)
            objcls = mc
        else:
            objcls = None
        fargs = self.call_args(f, args)
        ret = self.ntype(f.result_type)
        rt_ = f.result_type.get_canonical()
        if rt_.kind == TK.LVALUEREFERENCE and rt_.get_pointee().get_canonical().kind == TK.POINTER \
                and rt_.get_pointee().get_canonical().get_pointee().get_canonical().kind != TK.FUNCTIONPROTO:
            # a returned T*& names a 4-byte guest pointer slot: view it as GuestPtr<T>
            ret = f'GuestPtr<{self.ntype(rt_.get_pointee().get_canonical().get_pointee())}> &'
        sret = self.returns_indirect(f)
        if sret and '3' not in GAPS: raise Unsupported(f'struct return from {qname(f)}')
        if f.kind == CK.CXX_METHOD and f.is_virtual_method() and is_dynamic_call(c):
            vfoff, slot = self.vslot(sc, f)
            if sret:
                return f'cjg::vcall_sret({self.slot(f.result_type)}, {obj}, {vfoff}, cjg::VSlot({slot}, {self.site(("v", 4 * slot))}){"".join(", " + a for a in fargs)})'
            return f'cjg::vcall<{ret}>({obj}, {vfoff}, cjg::VSlot({slot}, {self.site(("v", 4 * slot))}){"".join(", " + a for a in fargs)})'
        va = self.va_of(f)
        d = self.defs.get(f.mangled_name) or (f.get_definition() if f.get_definition() is not None else None)
        if d is not None and (not any(ch.kind == CK.COMPOUND_STMT for ch in d.get_children()) or
                              f.spelling.startswith('operator') and 'operator' not in self.text(d)[0][self.text(d)[1]:self.text(d)[2]]):
            d = None   # implicitly defined (no source body)
        if va is None and d is None and f.spelling == 'operator=' and objcls is not None and len(args) == 1:
            # implicit copy assignment: memberwise copy of the view (GuestPtr fields copy their VA)
            return f'cjg::assign({obj}, {self.emit(args[0])})'
        # a callee with a VA that the original really calls is called by VA, also when its body was
        # translated as another swap target (it then goes through its own switch): native code must push
        # the same arguments and return VA as the original, and the callee's own selection must hold
        hdr_inl = False
        if d is not None and va is not None and HDR_INLINE and self.top_calls is not None and not self.orig_calls(va) \
                and not (f.kind == CK.CXX_METHOD and f.is_virtual_method()) and (d.location.file is not None and d.location.file.name.endswith('.h')):
            # a header-defined accessor the original of the swap target never calls: the original inlined it
            # (no call, no return-address / saved-register residue on the guest stack): translate it as well
            hdr_inl = True
        if d is not None and (va is None or (f.mangled_name in self.native and not self.orig_calls(va)) or self.inline_in_header(d, va) or hdr_inl):
            # no out-of-line copy (inlined in the original): translate its body natively
            try:
                n = self.sub_translate(d)
            except Unsupported:
                if not hdr_inl or va is None: raise
                n = None
            if n is not None:
                args = self.fix_args(f, args)
                pre = [obj] if objcls is not None else []
                return f'{n}({", ".join(pre + [self.native_arg(a) for a in args])})'
        if va is None and f.kind == CK.FUNCTION_DECL and f.spelling.startswith('cjmod_'):
            # mod helper: native code in src/native/hybrid/cjmod.c (scalar arguments only)
            return f'::{f.spelling}({", ".join(self.emit(a) for a in args)})'
        if va is None:
            hb = BUILTIN_HOST.get(f.spelling)
            if hb and f.kind == CK.FUNCTION_DECL: return f'{hb}({", ".join(self.emit(a) for a in args)})'
            raise Unsupported(f'callee {qname(f)} has no VA and no body')
        cc = 'this' if objcls is not None else ('std' if 'stdcall' in f.type.spelling else 'cdecl')
        pre = [obj] if objcls is not None else []
        if sret:
            # hidden return slot on the guest stack: the recompiled callee writes it (gap 3)
            if cc == 'std': raise Unsupported('sret __stdcall')
            return f'cjg::call_{cc}_sret({self.slot(f.result_type)}, cjg::Site(0x{va}u, {self.site(("d", int(va, 16)))}){"".join(", " + a for a in pre + fargs)})'
        return f'cjg::call_{cc}<{ret}>(cjg::Site(0x{va}u, {self.site(("d", int(va, 16)))}){"".join(", " + a for a in pre + fargs)})'

    def inline_in_header(self, d, va):
        # A definition with a VA counts as out-of-line callable; prefer calling it (it is what MSVC
        # emitted). Inline header bodies with a VA are still called by VA: semantics are identical.
        return False

    def site(self, key):
        n = len(self.sites); self.sites[n] = key
        return f'CJ_SITE_{n}'

    def site_pop(self, key):
        """bytes the callee's own `ret N` pops, from the original binary (callee_pop); -1 = unknown (virtual call,
        no size, several differing rets): the runtime then uses the declared signature"""
        if key[0] != 'd': return -1
        p = callee_pop(key[1])
        return -1 if p is None else p

    def resolve_sites(self, txt, va):
        """CJ_SITE_<n> tokens of a swap target's own body -> the return VA of the matching call
        instruction of the original (same callee / same vtable offset, k-th occurrence in text order
        = k-th in address order), its esp depth, and the callee's cleanup (callee_pop).
        Tokens of shared helper bodies stay unresolved (ret 0, see cmd_gen) but still get the cleanup."""
        avail = collections.defaultdict(list)
        for k, r, dp, _ in orig_sites(va) if va else []: avail[k].append((r, dp))
        def sub(m):
            key = self.sites[int(m.group(1))]
            pop = self.site_pop(key)
            q = avail.get(key)
            if not q: return f'0u, 0u, {pop}' if pop >= 0 else '0u'
            r, dp = q.pop(0)
            if pop >= 0: return f'0x{r:08x}u, {dp:#x}u, {pop}'
            return f'0x{r:08x}u, {dp:#x}u' if dp > 0 else f'0x{r:08x}u'
        return re.sub(r'CJ_SITE_(\d+)', sub, txt)

    def unresolved_sites(self, txt):
        def sub(m):
            pop = self.site_pop(self.sites[int(m.group(1))])
            return f'0u, 0u, {pop}' if pop >= 0 else '0u'
        return re.sub(r'CJ_SITE_(\d+)', sub, txt)

    def orig_calls(self, va):
        """the original of the swap target calls va (so the native code must call it too, by VA,
        instead of translating its body inline: same return VA and stack residue)"""
        return va is not None and self.top_calls is not None and int(va, 16) in self.top_calls

    def sub_translate(self, d):
        saved = (self.cur, self.cls)
        try:
            return self.translate(d)
        finally:
            self.cur, self.cls = saved

    def native_arg(self, a):
        return self.emit(a)

    def fix_args(self, f, args):
        """replace default-argument nodes (no source location) by the parameter's default expr"""
        params = list(f.get_arguments()) if f is not None else []
        out = []
        for i, a in enumerate(args):
            if a.extent.start.file is None and i < len(params):
                dk = [x for x in params[i].get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
                if not dk: raise Unsupported('default argument without expression')
                a = dk[-1]
            out.append(a)
        return out

    def call_args(self, f, args):
        args = self.fix_args(f, args)
        out = []
        params = list(f.get_arguments())
        for i, a in enumerate(args):
            sa = strip(a)
            if sa is not None and sa.kind == CK.STRING_LITERAL and guest_literal(sa):
                out.append(guest_literal(sa)); continue
            if (sa is not None and sa.kind == CK.STRING_LITERAL) or \
                    a.type.get_canonical().kind in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY):
                raise Unsupported('string literal / array argument to a guest call (host data cannot be passed)')
            pt = params[i].type.get_canonical() if i < len(params) else a.type.get_canonical()
            prev = getattr(self, '_byval_arg', None)
            if pt.kind == TK.RECORD:
                n = a
                while n is not None and n.kind in (CK.UNEXPOSED_EXPR,) and len(list(n.get_children())) == 1:
                    n = list(n.get_children())[0]
                self._byval_arg = n
            try:
                e = self.emit(a)
            finally:
                self._byval_arg = prev
            if pt.kind == TK.LVALUEREFERENCE:
                if not is_lvalue(a, self):
                    # a prvalue bound to a reference: materialise it in the guest frame
                    e = f'cjg::spill({self.slot(pt.get_pointee())}, {e})'
                e = f'cjg::ref({e})'
            elif pt.kind == TK.RECORD: e = f'cjg::byval({e})'
            elif pt.kind in BUILTIN: e = f'({BUILTIN[pt.kind]})({e})'
            out.append(e)
        return out

    def returns_indirect(self, f):
        t = f.result_type.get_canonical()
        if t.kind != TK.RECORD: return False
        sz = t.get_size()
        if f.kind == CK.CXX_METHOD and not f.is_static_method(): return True   # MSVC: methods return UDTs via sret
        # non-POD (user-declared destructor or copy constructor, e.g. EXString): always a hidden
        # result pointer, whatever the size (Text_Get 0xAFEC0 takes it at [esp+4])
        d = rec_decl(t)
        if d is not None:
            for ch in d.get_children():
                if ch.kind == CK.DESTRUCTOR or (ch.kind == CK.CONSTRUCTOR and ch.is_copy_constructor()):
                    return True
        return sz not in (1, 2, 4, 8)

    def vslot(self, sc, f):
        """(vfptr offset, slot index) of virtual f called through static class sc, from classes.json."""
        m = f.mangled_name
        lay = self.layout.get(rec_name(sc.type))
        c = sc
        while c is not None:
            cj = self.classes.get(qname(c, True))
            if cj and cj.get('vtable'):
                slots = cj['vtable']['slots']
                if m in slots:
                    off = self.layout.base_offset(rec_name(sc.type), rec_name(c.type))
                    if off != 0: raise Unsupported('virtual through non-primary vfptr')
                    return 0, slots.index(m)
            bases = self.base_decls(c)
            c = bases[0][1] if bases else None
        raise Unsupported(f'vtable slot for {qname(f)} via {qname(sc)} not found in classes.json')

    # ---------------------------------------------------------------- output
    def emit_views(self):
        done, order, asserts, texts = set(), [], [], {}
        # fixpoint: views can pull in more records (by-value fields)
        while True:
            todo = [q for q in self.need_rec if q not in done]
            if not todo: break
            for q in todo:
                done.add(q)
                try:
                    _, txt, a = self.view(self.need_rec[q])
                except Unsupported as e:
                    raise Unsupported(f'view {q}: {e}')
                texts[q] = txt; asserts += a; order.append(q)
        # dependency order: by-value fields first
        deps = {q: set() for q in order}
        for q in order:
            for m in re.finditer(r'(?<![\w<:])gv::([\w:]+) \w+(?:\[[^\]]*\])*;', texts[q]):   # by-value members, also inside unnamed unions
                n = m.group(1)
                while n not in deps and '::' in n: n = n.rsplit('::', 1)[0]     # nested enum of a record: needs the record
                if n in deps and n != q: deps[q].add(n)
        out, placed = [], set()
        def place(q, stack=()):
            if q in placed or q in stack: return
            for dq in sorted(deps[q], key=order.index): place(dq, stack + (q,))
            placed.add(q); out.append(q)
        for q in order: place(q)
        lines = ['namespace gv {', PCJ_TYPEDEFS]
        kind = {}
        for dd in list(self.opaque.values()) + list(self._fwdc.values()) + list(self.need_rec.values()):
            kind[qname(dd)] = 'union' if dd.kind == CK.UNION_DECL else 'struct'
        for q in sorted((set(qname(d) for d in self._fwdc.values()) | set(self.opaque)) - set(order)):
            lines.append(self.ns_wrap(q, f'{kind[q]} {q.split("::")[-1]};'))
        for q in order:
            lines.append(self.ns_wrap(q, f'{kind[q]} {q.split("::")[-1]};'))
        for q, e in self.enums.items():
            lines.append(self.ns_wrap(q, self.enum_text(e)))
        lines.append('#pragma pack(push, 1)')
        for q in out: lines.append(self.ns_wrap(q, texts[q]))
        lines.append('#pragma pack(pop)')
        lines.append('}  // namespace gv')
        return '\n'.join(lines), asserts

    def ns_wrap(self, q, txt):
        parts = q.split('::')[:-1]
        if not parts: return txt
        return ''.join(f'namespace {p} {{ ' for p in parts) + '\n' + txt + '\n' + '}' * len(parts)


class Frame:
    """guest-stack frame layout of one translated function"""
    def __init__(self):
        self.size, self.align, self.esc = 0, 4, set()
    def alloc(self, t):
        sz = max(t.get_size(), 1); al = max(min(t.get_align(), 16), 4)
        if sz <= 0: raise Unsupported(f'frame object of unknown size {t.spelling}')
        self.size = (self.size + al - 1) & ~(al - 1)
        off = self.size
        self.size += (sz + 3) & ~3
        self.align = max(self.align, al)
        return off


def strip(e):
    while e is not None and e.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR):
        kids = list(e.get_children())
        if len(kids) != 1: break
        e = kids[0]
    return e


def lvalue_root(e):
    """declaration whose storage expression e designates (through . member access,
    subscripts of arrays, parens and implicit conversions), or None"""
    while e is not None:
        k = e.kind
        if k in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR, CK.CSTYLE_CAST_EXPR, CK.CXX_STATIC_CAST_EXPR,
                 CK.CXX_REINTERPRET_CAST_EXPR, CK.CXX_CONST_CAST_EXPR):
            kids = [x for x in e.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
            if len(kids) != 1: return None
            e = kids[0]; continue
        if k == CK.MEMBER_REF_EXPR:
            kids = [x for x in e.get_children() if x.kind not in (CK.TYPE_REF, CK.NAMESPACE_REF, CK.TEMPLATE_REF)]
            if not kids or kids[0].type.get_canonical().kind == TK.POINTER: return None
            e = kids[0]; continue
        if k == CK.ARRAY_SUBSCRIPT_EXPR:
            kids = list(e.get_children())
            if not kids or kids[0].type.get_canonical().kind not in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY):
                b = strip(kids[0]) if kids else None
                if b is None or b.type.get_canonical().kind not in (TK.CONSTANTARRAY, TK.INCOMPLETEARRAY): return None
            e = kids[0]; continue
        if k == CK.DECL_REF_EXPR:
            r = e.referenced
            return r if r is not None and r.kind in (CK.VAR_DECL, CK.PARM_DECL) else None
        return None
    return None


def is_lvalue(a, g):
    e = strip(a)
    if e is None: return True
    k = e.kind
    if k in (CK.DECL_REF_EXPR, CK.MEMBER_REF_EXPR, CK.ARRAY_SUBSCRIPT_EXPR, CK.CXX_THIS_EXPR, CK.CXX_FUNCTIONAL_CAST_EXPR):
        return True
    if k == CK.UNARY_OPERATOR:
        toks = [t.spelling for t in e.get_tokens()]
        return bool(toks) and toks[0] in ('*', '++', '--')
    if k == CK.CALL_EXPR and e.referenced is not None:
        f = e.referenced
        if f.kind == CK.CONSTRUCTOR: return True                       # guest-frame temporary
        rt = f.result_type.get_canonical()
        if rt.kind == TK.LVALUEREFERENCE: return True
        if rt.kind == TK.RECORD and g.va_of(f) and g.returns_indirect(f): return True   # sret slot
        return False
    if k in (CK.CSTYLE_CAST_EXPR, CK.CXX_STATIC_CAST_EXPR, CK.CXX_REINTERPRET_CAST_EXPR):
        return e.type.get_canonical().kind == TK.LVALUEREFERENCE
    if k == CK.BINARY_OPERATOR or k == CK.COMPOUND_ASSIGNMENT_OPERATOR:
        toks = [t.spelling for t in e.get_tokens()]
        return '=' in toks and e.type.get_canonical().kind != TK.BOOL and k == CK.COMPOUND_ASSIGNMENT_OPERATOR
    return False


def is_prvalue_rec(a):
    e = strip(a)
    if e is None or e.type.get_canonical().kind != TK.RECORD: return False
    if e.kind == CK.CALL_EXPR and e.referenced is not None:
        if e.referenced.kind == CK.CONSTRUCTOR: return False
        return e.referenced.result_type.get_canonical().kind == TK.RECORD
    return False

def is_dynamic_call(c):
    fn = ci.conf.lib.clang_Cursor_isDynamicCall
    fn.argtypes = [ci.Cursor]; fn.restype = ci.c_int if hasattr(ci, 'c_int') else __import__('ctypes').c_int
    return bool(fn(c))


BUILTIN_HOST = {'memcpy': 'cjg::h_memcpy', 'memset': 'cjg::h_memset', 'strcmp': 'cjg::h_strcmp',
                'strlen': 'cjg::h_strlen', 'fabsf': 'fabsf', 'sqrtf': 'sqrtf', 'fabs': 'fabs', 'sqrt': 'sqrt',
                # x87 fsin/fcos/fpatan in the original: the host's double versions are the closest
                'sin': 'sin', 'cos': 'cos', 'atan2': 'atan2', 'sinf': 'sinf', 'cosf': 'cosf'}
SYMS = None


class GuestStrings:
    """String literals the retail XBE already holds: a native function passes the guest copy's VA
    (the original passed the same bytes), so no host data crosses into guest calls."""
    def __init__(self):
        self.secs = None
    def load(self):
        sys.path.insert(0, os.path.join(DECOMP, 'tools'))
        from xbe import XBE
        x = XBE(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'game', 'default.xbe'))
        self.secs = [(sc['va'], x.d[sc['raw']:sc['raw'] + sc['rsize']]) for sc in x.sections if sc['rsize']]
        self.cache = {}
    def find(self, b):
        """-> guest VA of a NUL-terminated copy of bytes b, or None"""
        if self.secs is None: self.load()
        if b in self.cache: return self.cache[b]
        needle = b + b'\0'; best = None
        for va, d in self.secs:
            i = d.find(needle)
            while i >= 0:
                if i == 0 or d[i - 1] == 0:   # a whole string, not the tail of a longer one
                    best = va + i; break
                i = d.find(needle, i + 1)
            if best is not None: break
        self.cache[b] = best
        return best


GSTR = GuestStrings()


def literal_bytes(c):
    """the bytes of a STRING_LITERAL cursor (adjacent pieces joined, C escapes decoded), or None"""
    toks = [t.spelling for t in c.get_tokens() if t.spelling.startswith('"')]
    if not toks: return None
    out = bytearray()
    for t in toks:
        body = t[1:-1]; i = 0
        while i < len(body):
            ch = body[i]
            if ch != '\\': out += ch.encode('latin-1', 'replace'); i += 1; continue
            i += 1; e = body[i]; i += 1
            if e == 'x':
                j = i
                while j < len(body) and body[j] in '0123456789abcdefABCDEF': j += 1
                out.append(int(body[i:j], 16) & 0xFF); i = j
            elif e in '01234567':
                j = i
                while j < len(body) and j < i + 2 and body[j] in '01234567': j += 1
                out.append(int(e + body[i:j], 8) & 0xFF); i = j
            else: out += {'n': b'\n', 't': b'\t', 'r': b'\r', '0': b'\0', 'a': b'\a', 'b': b'\b', 'f': b'\f', 'v': b'\v'}.get(e, e.encode('latin-1'))
    return bytes(out)


def guest_literal(c):
    """expression for a string literal cursor as a pointer into guest memory, or None when the XBE lacks it"""
    b = literal_bytes(c)
    if b is None: return None
    va = GSTR.find(b)
    if va is None: return None
    txt = b.decode('latin-1').replace('*/', '* /').replace('\n', '\\n')[:40]
    return f'cjg::h<char>(0x{va:08X}u) /* "{txt}" */'
GAPS = os.environ.get('HY_GAPS', '134')    # scan bookkeeping: which gap implementations are on


def find_def(g, va):
    va = va.lower().zfill(8)
    for m, c in g.defs.items():
        r = g.fn.get(m)
        if r and r['va'].lower() == va and c.location.file and c.location.file.name == g.path:
            return c
    for m, c in g.defs.items():
        r = g.fn.get(m)
        if r and r['va'].lower() == va: return c
    raise SystemExit(f'no definition for VA {va} in {g.unit}')


MODS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'mods')

def mod_overlay(unit, vas):
    """The unit's source with each mod VA's definition replaced by the one in mods/<unit>.cpp.
    In the mod file, each replacement starts with a line `// MOD <va>` and runs to the next
    `// MOD` line or the end; text before the first `// MOD` (declarations, cjmod_ prototypes)
    is inserted after the last #include that precedes the first replaced function. pcj-decomp itself is never modified."""
    g0 = Gen(unit)
    src = open(g0.src_path, encoding='latin-1').read()
    mp = os.path.join(MODS_DIR, unit + '.cpp')
    mod = open(mp, encoding='latin-1').read() if os.path.exists(mp) else ''
    parts = re.split(r'^// MOD ([0-9a-fA-F]{6,8})[^\n]*\n', mod, flags=re.M)
    prelude, repl = parts[0], {parts[i].lower().zfill(8): parts[i + 1] for i in range(1, len(parts), 2)}
    edits = []
    for va in vas:
        v = va.lower().zfill(8)
        st = g0.status.get(v, {}).get('status')
        if st not in ('MATCH', 'DIFF'): raise SystemExit(f'mod {v}: no decompiled source ({st}); write it in pcj-decomp first')
        if st != 'MATCH': print(f'  mod {v}: decomp source is DIFF (check it with CJ_SWAP_VERIFY)')
        if v not in repl: continue          # swap in the decompiled source itself
        c = find_def(g0, v)
        edits.append((c.extent.start.offset, c.extent.end.offset, repl[v]))
    for b, e, t in sorted(edits, reverse=True):
        src = src[:b] + '/* MOD (pcj-native mods/) */\n' + t.rstrip() + '\n' + src[e:]
    incs = list(re.finditer(r'^#include[^\n]*\n', src, flags=re.M))
    # after the last #include that comes before the first replaced function (some units have more
    # #includes further down, past the mod's functions)
    first = min((b for b, e, t in edits), default=len(src))
    before = [m for m in incs if m.end() <= first]
    at = before[-1].end() if before else (incs[-1].end() if incs else 0)
    src = src[:at] + prelude + src[at:]
    import tempfile
    d = tempfile.mkdtemp(prefix='hymod_')
    path = os.path.join(d, unit + '.cpp')
    open(path, 'w', encoding='latin-1').write(src)
    return path


def cmd_gen(a):
    groups, cur = [], None
    for tok in a.spec:
        if tok.startswith('unit=') or tok.startswith('mod='):
            cur = (tok.split('=', 1)[1], [], tok.startswith('mod=')); groups.append(cur)
        else: cur[1].append(tok)
    os.makedirs(a.out, exist_ok=True)
    manifest = []
    for unit, vas, is_mod in groups:
        src = mod_overlay(unit, vas) if is_mod else unit
        if is_mod:
            # the overlay lives in a temp dir: let its quoted includes resolve next to the original
            orig_dir = os.path.dirname(Gen(unit).src_path)
            if orig_dir not in ARGS: ARGS.extend(['-iquote', orig_dir])
        for pp in (False, True):
            g = Gen(src, pp=pp)
            tops = []
            try:
                for va in vas:
                    c = find_def(g, va)
                    st = g.status.get(va.lower().zfill(8), {}).get('status')
                    if st != 'MATCH' and not is_mod: raise SystemExit(f'{va} is {st}, not MATCH')
                    name = g.translate(c, swapped=True)
                    tops.append((va.lower().zfill(8), c, name))
            except NeedPP:
                if pp: raise
                continue
            break
        views, asserts = g.emit_views()
        cpp = [f'// GENERATED by tools/hybridize/hybridize.py from pcj-decomp {os.path.relpath(g.src_path, DECOMP)}{" (preprocessed: macro-using functions)" if g.pp else ""}. Do not edit.',
               '#include "hybrid/guest_rt.h"', '#include "hybrid/cjmod.h"', '#include <cmath>', MSVC_KEYWORDS, '', views, '', *asserts, '', 'namespace gv {']
        cpp += g.protos + ['']
        cpp += g.out_funcs
        cpp.append('}  // namespace gv\n')
        for va, c, name in tops:
            cpp.append(adapter_cpp(g, va, c, name))
            m = adapter_meta(g, va, c, name, unit)
            m['mod'] = is_mod
            manifest.append(m)
        if g.notes: cpp.insert(1, '\n'.join('// NOTE ' + n for n in g.notes))
        open(os.path.join(a.out, f'hy_{sanitize(unit)}{"_mod" if is_mod else ""}.cpp'), 'w').write(g.unresolved_sites('\n'.join(cpp)))
        print(f'{unit}: {len(tops)} swapped, {len(g.out_funcs)} native functions, {len(g.need_rec)} views')
        for n in g.notes: print('  NOTE', n)
    open(os.path.join(a.out, 'hybrid_swap.c'), 'w').write(swap_c(manifest))
    json.dump(manifest, open(os.path.join(a.out, 'manifest.json'), 'w'), indent=1)


# ---------------------------------------------------------------- adapters (guest ABI <-> native)
def stack_words(g, t):
    t = t.get_canonical()
    if t.kind in (TK.LONGLONG, TK.ULONGLONG, TK.DOUBLE, TK.LONGDOUBLE): return 2
    if t.kind == TK.RECORD: return (t.get_size() + 3) // 4
    return 1

def is_sret(g, c):
    return g.returns_indirect(c) if c.kind == CK.CXX_METHOD or c.kind == CK.FUNCTION_DECL else False

# ---------------------------------------------------------------- register clobbers of the original
# VC7.1 knows which registers a callee in the same object file really writes, and keeps
# values live across the call in the ones it does not touch: DoObjectAlign (0x00108730)
# holds xmm1/xmm3 across its call to TweakPosition, which only writes xmm0. The native
# version calls recompiled helpers (EXPlacementItem::SetPosition writes xmm1/xmm2), so
# the swap wrapper must restore every scratch register the original never writes.
# The set is computed from the original code (transitively through direct calls);
# an indirect call, or a callee without a known size, means "may write anything".
SCRATCH = ['eax', 'ecx', 'edx'] + [f'xmm{i}' for i in range(8)]
_XBE = None
def _xbe():
    global _XBE
    if _XBE is None:
        import struct
        _here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        _cands = [os.environ.get('PCJ_XBE', ''), os.path.join(_here, 'game', 'default.xbe'), os.path.join(DECOMP, 'orig', 'DEFAULT.XBE')]
        b = open(next(c for c in _cands if c and os.path.isfile(c)), 'rb').read()
        base, nsec, sh = struct.unpack_from('<I', b, 0x104)[0], struct.unpack_from('<I', b, 0x11C)[0], struct.unpack_from('<I', b, 0x120)[0]
        secs = []
        for i in range(nsec):
            _, va, vs, raw, rs = struct.unpack_from('<5I', b, sh - base + 56 * i)
            secs.append((va, rs, raw))
        sizes = {}
        for r in csv.DictReader(open(os.path.join(DECOMP, 'symbols', 'functions.csv'))):
            sizes[int(r['va'], 16)] = int(r['size'])
        _XBE = (b, secs, sizes)
    return _XBE

def _code(va, n):
    b, secs, _ = _xbe()
    for sva, rs, raw in secs:
        if sva <= va and va + n <= sva + rs:
            return b[raw + va - sva: raw + va - sva + n]
    return None

HDR_INLINE = os.environ.get('HY_INLINE_HDR', '0') == '1'
_POP = {}
def callee_pop(va, _depth=0):
    """Bytes the callee at va pops on return, read from the ORIGINAL binary instead of the declared signature:
    every `ret` / `ret N` of the function (and of a function it tail-jumps to) must agree, else None (the
    signature decides). Covers cdecl (ret), stdcall/thiscall (ret N), variadics (ret), VC7.1 register-convention
    statics, and callees whose declaration in the decomp does not match what the compiler emitted."""
    if va in _POP: return _POP[va]
    _POP[va] = None
    import capstone
    from capstone import x86
    size = _xbe()[2].get(va)
    code = _code(va, size) if size else None
    if code is None: return None
    pops, ok = set(), True
    for ins in _cs().disasm(code, va):
        if ins.mnemonic == 'int3': break
        if ins.group(x86.X86_GRP_RET):
            pops.add(ins.operands[0].imm if ins.operands else 0)
        elif ins.mnemonic == 'jmp':
            op = ins.operands[0] if ins.operands else None
            if op is None or op.type != x86.X86_OP_IMM: ok = False; break      # jmp reg / jmp [mem]: unknown
            if va <= op.imm < va + size: continue
            if _depth >= 3: ok = False; break
            t = callee_pop(op.imm, _depth + 1)
            if t is None: ok = False; break
            pops.add(t)
    if ok and len(pops) == 1: _POP[va] = next(iter(pops))
    return _POP[va]

def orig_writes(va, seen=None):
    """Scratch registers (SCRATCH names) the original function at va may write, transitively."""
    import capstone
    from capstone import x86
    seen = set() if seen is None else seen
    if va in seen: return set()
    seen.add(va)
    size = _xbe()[2].get(va)
    code = _code(va, size) if size else None
    if code is None: return set(SCRATCH)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
    out = set()
    for ins in md.disasm(code, va):
        try:
            _, wr = ins.regs_access()
        except capstone.CsError:
            return set(SCRATCH)
        for r in wr:
            n = ins.reg_name(r)
            n = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx',
                 'dl': 'edx', 'dh': 'edx', 'dx': 'edx'}.get(n, n)
            if n in SCRATCH: out.add(n)
        if ins.group(x86.X86_GRP_CALL) or ins.group(x86.X86_GRP_JUMP):
            op = ins.operands[0] if ins.operands else None
            if op is None or op.type != x86.X86_OP_IMM:
                if ins.group(x86.X86_GRP_CALL) or ins.mnemonic == 'jmp': return set(SCRATCH)
                continue
            tgt = op.imm
            if va <= tgt < va + size: continue          # branch inside the function
            out |= orig_writes(tgt, seen)               # call, or tail jump
            if len(out) == len(SCRATCH): return out
    return out

REGNO = {'eax': 0, 'ecx': 1, 'edx': 2, 'ebx': 3, 'esp': 4, 'ebp': 5, 'esi': 6, 'edi': 7}

_CS = None
def _cs():
    global _CS
    if _CS is None:
        import capstone
        _CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); _CS.detail = True
    return _CS

_FRAME = {}
def orig_frame(va):
    """The original's prologue stack writes, up to the first call/branch: `push reg` of a register not
    pushed before and not written since entry, `sub esp, imm`, `push ebp; mov ebp, esp`, with other
    instructions in between skipped (VC7.1 interleaves `mov reg, [esp+n]` with the saves).
    -> dict(ops=[('push', reg) | ('sub', n)], depth=bytes, area=(hi, size)): the local area is the
    first `push ecx` run / `sub esp` (ecx is never callee-saved, so a prologue `push ecx` is the
    4-byte local area); it lies at [esp_in - hi, esp_in - hi + size).
    The adapter repeats ops on the guest stack, so the native body's guest callees run at the esp the
    original has at its call sites and find the same saved words above them (docs/promote.md round 2;
    round 5 of docs/determinism.md was the leading-push-only version)."""
    try: va_i = int(va, 16) if isinstance(va, str) else va
    except (TypeError, ValueError): return dict(ops=[], depth=0, area=None)
    if va_i in _FRAME: return _FRAME[va_i]
    size = _xbe()[2].get(va_i)
    code = _code(va_i, size) if size else None
    ops, depth, area, dirty, pushed = [], 0, None, set(), set()
    from capstone import x86
    if code is not None:
        for ins in _cs().disasm(code[:96], va_i):
            m, o = ins.mnemonic, ins.op_str
            if ins.group(x86.X86_GRP_CALL) or ins.group(x86.X86_GRP_JUMP) or ins.group(x86.X86_GRP_RET): break
            if m == 'push' and o in REGNO and o != 'esp':
                if o in pushed or o in dirty: break
                pushed.add(o); ops.append(('push', o)); depth += 4
                if o == 'ecx':
                    if area is None: area = [depth, 4]
                    elif area[0] == depth - 4: area = [depth, area[1] + 4]
                continue
            if m == 'sub' and o.startswith('esp, '):
                try: n = int(o[5:], 0)
                except ValueError: break
                ops.append(('sub', n)); depth += n
                if area is None or area[0] == depth - n: area = [depth, (area[1] if area else 0) + n]
                continue
            if m == 'mov' and o == 'ebp, esp': continue
            try: _, wr = ins.regs_access()
            except Exception: break
            names = {ins.reg_name(r) for r in wr}
            if 'esp' in names: break
            for n in names:
                n = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dh': 'edx',
                     'dx': 'edx', 'bl': 'ebx', 'bh': 'ebx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi', 'bp': 'ebp'}.get(n, n)
                dirty.add(n)
    r = dict(ops=ops, depth=depth, area=tuple(area) if area else None)
    _FRAME[va_i] = r
    return r

def orig_prologue(va):
    return orig_frame(va)['ops']

_SITES = {}
_SAVED = ('ebx', 'esi', 'edi', 'ebp')
_R16 = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dh': 'edx',
        'dx': 'edx', 'bl': 'ebx', 'bh': 'ebx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi', 'bp': 'ebp'}
def orig_sites(va):
    """Call instructions of the original, in address order: [((kind, key), return VA, depth)] with
    ('d', callee VA) for `call imm` and ('v', disp) for `call [reg + disp]` or `call reg` after
    `mov reg, [x + disp]` (a vtable slot). depth = bytes between esp at entry and esp at the call
    before its own arguments are pushed (linear scan of push/pop/sub/add esp; VC7.1 saves registers
    late, `push ebp` / `push esi` after the first calls, so it differs per site).
    Used to give each guest call of the native body the original's return VA and esp
    (hybridize.py Gen.resolve_sites, runtime cjg_call_raw)."""
    va_i = int(va, 16) if isinstance(va, str) else va
    if va_i in _SITES: return _SITES[va_i][0]
    orig_sites_full(va_i)
    return _SITES[va_i][0]

def orig_sites_full(va_i):
    """-> (sites, saves): saves = [(slot depth, reg)] of the callee-saved register pushes (a push of
    ebx/esi/edi/ebp before that register is written)."""
    if va_i in _SITES: return _SITES[va_i]
    from capstone import x86
    size = _xbe()[2].get(va_i)
    code = _code(va_i, size) if size else None
    out, saves, lastload = [], [], {}
    depth, args, dirty, tdepth, dead, last_args = 0, 0, set(), {}, False, 0
    src = {}   # callee-saved register -> 0 (this, from ecx at entry) or esp_in offset of the argument word it was loaded from
    if code is not None:
        for ins in _cs().disasm(code, va_i):
            if dead:
                if ins.address in tdepth: depth = tdepth[ins.address]
                dead = False
            m, o = ins.mnemonic, ins.op_str
            if m == 'mov' and len(ins.operands) == 2 and ins.operands[0].type == x86.X86_OP_REG \
                    and ins.operands[1].type == x86.X86_OP_MEM and ins.operands[1].mem.index == 0:
                lastload[ins.reg_name(ins.operands[0].reg)] = ins.operands[1].mem.disp
            if m == 'mov' and len(ins.operands) == 2 and ins.operands[0].type == x86.X86_OP_REG:
                d0 = ins.reg_name(ins.operands[0].reg); o1 = ins.operands[1]
                if d0 in _SAVED:
                    if o1.type == x86.X86_OP_REG and ins.reg_name(o1.reg) == 'ecx' and 'ecx' not in dirty: src[d0] = 0
                    elif o1.type == x86.X86_OP_MEM and o1.mem.index == 0 and ins.reg_name(o1.mem.base) == 'esp' \
                            and o1.mem.disp - depth >= 4: src[d0] = o1.mem.disp - depth
                    else: src.pop(d0, None)
                    dirty.add(d0); continue
            if m == 'push':
                depth += 4
                if o in _SAVED and o not in dirty and o not in {r for _, r in saves}: saves.append((depth, o))
                elif not (o == 'ecx' and not out and args == 0 and all(r in ('ecx',) + _SAVED for r in [o])
                          and depth <= 8 and not saves): args += 1
                continue
            if m == 'pop': depth -= 4; continue
            if m in ('sub', 'add') and o.startswith('esp, '):
                try: n = int(o[5:], 0)
                except ValueError: n = 0
                if m == 'sub': depth += n
                else:
                    if last_args and n <= last_args: n = 0          # cdecl cleanup of the last call: already undone
                    elif last_args: n -= last_args
                    depth -= n
                last_args = 0
                continue
            last_args = 0
            if ins.group(x86.X86_GRP_CALL) and ins.operands:
                op = ins.operands[0]; ret = ins.address + ins.size
                base = depth - 4 * args
                key = None
                if op.type == x86.X86_OP_IMM: key = ('d', op.imm)
                elif op.type == x86.X86_OP_MEM and op.mem.base != 0 and op.mem.index == 0: key = ('v', op.mem.disp)
                elif op.type == x86.X86_OP_REG and ins.reg_name(op.reg) in lastload: key = ('v', lastload[ins.reg_name(op.reg)])
                if key: out.append((key, ret, base, dict(src)))
                last_args = 4 * args; depth = base; args = 0
                dirty.update(('eax', 'ecx', 'edx'))
                continue
            if ins.group(x86.X86_GRP_JUMP) and ins.operands and ins.operands[0].type == x86.X86_OP_IMM:
                tdepth.setdefault(ins.operands[0].imm, depth)
                if m == 'jmp': dead = True
                continue
            if ins.group(x86.X86_GRP_RET) or m == 'jmp': dead = True; continue
            try: _, wr = ins.regs_access()
            except Exception: wr = []
            for r in wr:
                n = _R16.get(ins.reg_name(r), ins.reg_name(r)); dirty.add(n); src.pop(n, None)
    _SITES[va_i] = (out, saves)
    return _SITES[va_i]

def adapter_meta(g, va, c, name, unit):
    thiscall = c.kind in (CK.CONSTRUCTOR, CK.DESTRUCTOR) or c.kind == CK.CXX_METHOD and not c.is_static_method()
    words = sum(stack_words(g, x.type) for x in c.get_arguments()) + (1 if is_sret(g, c) else 0)
    callee_pops = thiscall or 'stdcall' in c.type.spelling
    rk = c.result_type.get_canonical().kind
    if c.kind == CK.CONSTRUCTOR or is_sret(g, c): rk = TK.POINTER
    ret = 'void' if rk == TK.VOID else 'i64' if rk in (TK.LONGLONG, TK.ULONGLONG) else \
        'fp' if rk in (TK.FLOAT, TK.DOUBLE, TK.LONGDOUBLE) else 'i32'
    lay = g.layout.get(rec_name(c.semantic_parent.type)) if thiscall else None
    return dict(va=va, name=qname(c), unit=unit, thiscall=thiscall, arg_words=words,
                pop=4 + (4 * words if callee_pops else 0), ret=ret,
                this_size=min(lay['size'], 0x400) if lay else 0, native=f'cjn_{va}',
                keep=[r for r in SCRATCH if r not in orig_writes(int(va, 16)) and not (r == 'eax' and ret != 'void')])

def adapter_cpp(g, va, c, name):
    thiscall = c.kind in (CK.CONSTRUCTOR, CK.DESTRUCTOR) or c.kind == CK.CXX_METHOD and not c.is_static_method()
    sret = is_sret(g, c)
    lines = [f'/* guest-ABI entry for {qname(c)} (0x{va}): reads this/args from the guest frame. */',
             f'extern "C" void cjn_{va.upper()}(void)', '{']
    lines.append(f'    cjg_nat_enter(0x{va.upper()}u);   /* blame attribution: [HYB] reports name the innermost active native function */')
    lines.append('    uint32_t sp = cjg_esp() + 4;   /* past the return address */')
    fr = orig_frame(va) if os.environ.get('HY_PROLOGUE', '1') != '0' else dict(ops=[], depth=0, area=None)
    pro = fr['ops']
    if pro:
        lines.append('    uint32_t esp_in = cjg_esp();   /* original prologue: ' +
                     '; '.join(f'push {x[1]}' if x[0] == 'push' else f'sub esp, {x[1]:#x}' for x in pro) + ' */')
        for k, v in pro:
            lines.append(f'    cjg_push_reg({REGNO[v]});' if k == 'push' else f'    cjg_set_esp(cjg_esp() - {v:#x}u);')
        sv = [x for x in orig_sites_full(int(va, 16))[1] if x[0] > fr['depth']]   # registers the original saves later (shrink-wrapped)
        lines.append('    static const uint32_t saves[] = {' + ', '.join(f'{d:#x}u, {REGNO[r]}u' for d, r in sv) + ('' if sv else '0') + '};')
        rt = [(r, src) for _, r, _, src in orig_sites_full(int(va, 16))[0] if src]
        lines.append('    static const uint32_t regs[] = {' + ', '.join(
            f'{r:#x}u, ' + ', '.join(f'{(src[n] if n in src else 0xFFFFFFFF):#x}u' for n in ('ebx', 'esi', 'edi', 'ebp')) for r, src in rt) +
            ('' if rt else '0') + '};   /* per call site (return VA): ebx esi edi ebp = this (0) / argument word at esp_in+n / unknown */')
        lines.append(f'    cjg_ctx_enter(esp_in, saves, {len(sv)}, regs, {len(rt)});   /* call sites at the original esp (Site depth) */')
        if fr['area']:
            hi, asz = fr['area']
            lines.append(f'    cjg_area_set(esp_in - {hi:#x}u, {asz:#x}u);   /* the original\'s local area: the body Frame goes there if it fits */')
    call = []
    small_ret = (c.kind != CK.CONSTRUCTOR and not sret and c.result_type.get_canonical().get_size() in (1, 2)
                 and c.result_type.get_canonical().kind in (TK.BOOL, TK.CHAR_S, TK.SCHAR, TK.UCHAR, TK.CHAR_U, TK.SHORT, TK.USHORT, TK.WCHAR))
    if small_ret:
        lines.append('    uint32_t eax_in = cjg_eax();   /* 1/2-byte return: upper bits of eax stay as the caller left them */')
    if thiscall:
        lines.append('    uint32_t this_in = cjg_ecx();   /* the body\'s guest calls clobber ecx; a constructor returns this in eax */')
        lines.append(f'    auto *self = cjg::h<gv::{qname(c.semantic_parent)}>(this_in);')
        call.append('self')
    if sret:   # MSVC hidden return slot: first stack word (after ecx = this)
        lines.append(f'    auto *ret = cjg::h<{g.ntype(c.result_type)}>(cjg::rd32(sp));')
        lines.append('    sp += 4;')
    for i, x in enumerate(c.get_arguments()):
        t = x.type.get_canonical(); nt = g.ntype(x.type); w = stack_words(g, x.type)
        if t.kind == TK.LVALUEREFERENCE:
            lines.append(f'    auto &a{i} = *cjg::h<{g.ntype(t.get_pointee())}>(cjg::rd32(sp));')
        elif t.kind == TK.POINTER:
            lines.append(f'    auto *a{i} = cjg::h<{nt[:-2].strip()}>(cjg::rd32(sp));' if not nt.startswith('uint32_t') or '*' in nt
                         else f'    uint32_t a{i} = cjg::rd32(sp);')
        elif t.kind == TK.RECORD:
            lines.append(f'    {nt} &a{i} = *cjg::h<{nt}>(sp);')
        else:
            lines.append(f'    {nt} a{i}; cjg::rdv(sp, &a{i});')
        lines.append(f'    sp += {4 * w};')
        call.append(f'a{i}')
    rk = c.result_type.get_canonical().kind
    inv = f'{name}({", ".join(call)})'
    if c.kind == CK.CONSTRUCTOR: lines.append(f'    gv::{inv};'); lines.append('    cjg_set_eax(this_in);')
    elif sret: lines.append(f'    *ret = gv::{inv};'); lines.append('    cjg_set_eax(cjg::to_guest(ret));')
    elif rk == TK.VOID: lines.append(f'    gv::{inv};')
    elif rk in (TK.POINTER,): lines.append(f'    cjg_set_eax(cjg::to_guest(gv::{inv}));')
    elif rk in (TK.LONGLONG, TK.ULONGLONG):
        lines.append(f'    uint64_t r = (uint64_t)gv::{inv}; cjg_set_eax((uint32_t)r); cjg_set_edx((uint32_t)(r >> 32));')
    elif rk in (TK.FLOAT, TK.DOUBLE, TK.LONGDOUBLE): lines.append(f'    cjg_fp_push((double)gv::{inv});')
    elif rk == TK.LVALUEREFERENCE: lines.append(f'    cjg_set_eax(cjg::to_guest(&gv::{inv}));')
    elif rk == TK.RECORD:
        rsz = c.result_type.get_canonical().get_size()
        if rsz not in (1, 2, 4, 8): raise Unsupported('struct return')
        # MSVC returns a free function's UDT of size 1/2/4/8 in eax (edx:eax for 8)
        lines.append(f'    {{ auto r = gv::{inv}; uint64_t v = 0; std::memcpy(&v, &r, {rsz}); cjg_set_eax((uint32_t)v);' +
                     (' cjg_set_edx((uint32_t)(v >> 32));' if rsz == 8 else '') + ' }')
    elif small_ret:
        # 1/2-byte return: the original writes only al / ax and leaves the upper bits of the incoming eax
        n = c.result_type.get_canonical().get_size()
        lines.append(f'    cjg_set_{"al" if n == 1 else "ax"}(eax_in, (uint32_t)(int32_t)gv::{inv});')
    else: lines.append(f'    cjg_set_eax((uint32_t)(int32_t)gv::{inv});')
    if pro: lines += ['    cjg_area_set(0, 0);', '    cjg_ctx_leave();', '    cjg_set_esp(esp_in);']
    lines.append('    cjg_nat_leave();')
    lines.append('}\n')
    return '\n'.join(lines)

SWAP_HEAD = r'''/* GENERATED by tools/hybridize/hybridize.py -- do not edit.
 * Passed to tools.recomp as --exclude-manual: each `extern void sub_X_gen(void);`
 * keeps the recompiled body as sub_X_gen, and sub_X below is the switch.
 * CJ_NATIVE_SWAP=1 selects native for all functions here (or a comma list of VAs).
 * CJ_SWAP_LOG=N logs the first N calls of each function. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

extern uint32_t cj_det_frame;   /* src/determinism.c: MainUpdate frames so far */
static int s_mode = -1; static const char *s_sel; static uint32_t s_log_n = 48;
static uint64_t s_hash_n = 4096;   /* CJ_SWAP_HASH_N: state hashes only for the first N calls of a function (hot functions) */
static void init_mode(void)
{
    const char *e = getenv("CJ_NATIVE_SWAP"), *l = getenv("CJ_SWAP_LOG");
    /* "0"/unset: generated. A VA list starts with '0' too, so test the whole string. */
    s_mode = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    s_sel = (e && strchr(e, ',')) || (e && strlen(e) == 8) ? e : NULL;
    if (l) s_log_n = (uint32_t)strtoul(l, NULL, 0);
    if (getenv("CJ_SWAP_HASH_N")) s_hash_n = strtoull(getenv("CJ_SWAP_HASH_N"), NULL, 0);
    fprintf(stderr, "[SWAP] generated hybrid swaps: %s\n", s_mode ? (s_sel ? s_sel : "ALL NATIVE") : "generated");
}
static int selected(const char *va) { return s_mode && (!s_sel || strstr(s_sel, va)); }
/* src/native/hybrid/swap_verify.c: dual-execution check (CJ_SWAP_VERIFY) */
int cjv_want(const char *va, uint64_t n); void cjv_begin(void); int cjv_mid(void); void cjv_mid2(void); int cjv_self(void);
void cjv_skip(const char *va, const char *name, uint64_t n);
void cjv_end(const char *va, const char *name, uint64_t n, int has_ret);
/* Mods (pcj-native mods/, mod= in targets.txt) run native by default; CJ_MODS=0
 * turns every mod off (the recompiled original runs), CJ_MODS=<va list> keeps
 * only those. A/B selection (CJ_NATIVE_SWAP) also selects them. */
static int s_mods = -1; static const char *s_mods_sel;
static int mod_on(const char *va)
{
    if (s_mods < 0) {
        const char *e = getenv("CJ_MODS");
        s_mods = !(e && strcmp(e, "0") == 0);
        s_mods_sel = (e && strlen(e) >= 6 && strcmp(e, "1") != 0) ? e : NULL;
        fprintf(stderr, "[MODS] %s\n", s_mods ? (s_mods_sel ? s_mods_sel : "all mods on (CJ_MODS=0: off)") : "off (CJ_MODS=0)");
    }
    return (s_mods && (!s_mods_sel || strstr(s_mods_sel, va))) || selected(va);
}
static uint64_t fnv(uint64_t h, uint32_t v)
{ for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; } return h; }
static uint64_t mem_hash(uint64_t h, uint32_t va, uint32_t n)
{ if (!va) return fnv(h, 0); for (uint32_t i = 0; i + 4 <= n; i += 4) h = fnv(h, MEM32(va + i)); return h; }
/* Address-independent state hash: boot has a heap-order race (docs/native-swap.md), so
 * words that look like guest heap/contiguous pointers are hashed as a token, and each
 * such pointer's first 0x80 bytes are hashed too (catches writes through pointer fields). */
static int is_ptr(uint32_t v) { return (v >= 0x80000000u && v < 0x84000000u) || (v >= 0x00F80000u && v < 0x04000000u); }
static uint32_t norm(uint32_t v) { return is_ptr(v) ? 0x50545221u : v; }
static uint64_t deep_hash(uint64_t h, uint32_t va, uint32_t n)
{
    if (!va) return fnv(h, 0);
    for (uint32_t i = 0; i + 4 <= n; i += 4) {
        uint32_t v = MEM32(va + i);
        h = fnv(h, norm(v));
        if (is_ptr(v)) for (uint32_t j = 0; j < 0x80; j += 4) h = fnv(h, norm(MEM32(v + j)));
    }
    return h;
}
typedef struct { const char *va, *name; uint64_t calls, enter, esp_bad, csr_bad, hash; } swap_stat;
/* CJ_SWAP_DUMP=<VA> + CJ_SWAP_DUMP_DIR=dir (+ CJ_SWAP_DUMP_N, default 8): raw guest .data
 * before and after each of the first N calls of that function, to
 * dir/<VA>_<n>_{pre,post}.bin. Two deterministic runs (CJ_DET) in different modes
 * then show exactly which bytes the native and generated bodies write differently. */
static void swap_dump(const char *va, uint64_t n, const char *tag)
{
    static const char *dv, *dd; static uint64_t dn; static int init;
    char path[512]; FILE *f;
    if (!init) { init = 1; dv = getenv("CJ_SWAP_DUMP"); dd = getenv("CJ_SWAP_DUMP_DIR");
                 dn = getenv("CJ_SWAP_DUMP_N") ? strtoull(getenv("CJ_SWAP_DUMP_N"), NULL, 0) : 8; }
    if (!dv || !dd || n > dn || !strstr(dv, va)) return;
    snprintf(path, sizeof path, "%s/%s_%llu_%s.bin", dd, va, (unsigned long long)n, tag);
    if ((f = fopen(path, "wb"))) { fwrite((void *)XBOX_PTR(0x00394740u), 1, 0x00500E54u - 0x00394740u, f); fclose(f); }
}
static void swap_ck(swap_stat *s, uint64_t n)
{
    if ((n & (n - 1)) == 0 || n % 1000 == 0)
        fprintf(stderr, "[SWAP] ckpt %s calls=%llu enter(gen body)=%llu esp_bad=%llu csr_bad=%llu run_hash=%016llx\n",
                s->va, (unsigned long long)n, (unsigned long long)s->enter, (unsigned long long)s->esp_bad,
                (unsigned long long)s->csr_bad, (unsigned long long)s->hash);
}
'''

SWAP_FN = r'''
/* 0x{VA} {NAME} ({UNIT}) */
extern void sub_{VA}_gen(void);
extern void cjn_{VA}(void);
static swap_stat st_{VA} = {{"{VA}", "{NAME}", 0, 0, 0, 0, 1469598103934665603ull}};
void sub_{VA}_enter(void) {{ st_{VA}.enter++; }}
void sub_{VA}(void)
{{
    if (s_mode < 0) init_mode();
    swap_stat *s = &st_{VA};
    uint64_t n = ++s->calls;
    uint32_t self = g_ecx, esp0 = g_esp, b = g_ebx, si = g_esi, di = g_edi;
    int hs = n <= s_hash_n;
    uint64_t pre = 0;
    if (hs) {{ pre = deep_hash(1469598103934665603ull, {THIS}, {TSIZE});
    for (int i = 1; i <= {WORDS}; i++) pre = fnv(pre, norm(MEM32(esp0 + 4 * i))); }}
    int fp0 = g_fp_top;
    swap_dump(s->va, n, "pre");
    if (cjv_want("{VA}", n)) {{ cjv_begin(); sub_{VA}_gen(); if (!cjv_mid()) cjv_skip("{VA}", "{NAME}", n); else {{ sub_{VA}_gen(); cjv_mid2(); if (cjv_self()) sub_{VA}_gen(); else {{ {SAVE}cjn_{VA}(); g_esp += {POP};{RESTORE} }} cjv_end("{VA}", "{NAME}", n, {HASRET}); }} }}
    else if ({SEL}) {{ {SAVE}cjn_{VA}(); g_esp += {POP};{RESTORE} }} else sub_{VA}_gen();
    swap_dump(s->va, n, "post");
    if (g_esp != esp0 + {POP}) s->esp_bad++;
    if (g_ebx != b || g_esi != si || g_edi != di) s->csr_bad++;
    uint64_t post = 0;
    if (hs) {{ post = deep_hash(1469598103934665603ull, {THIS}, {TSIZE});
    for (int i = 1; i <= {WORDS}; i++) {{ uint32_t a = MEM32(esp0 + 4 * i); if (is_ptr(a)) post = deep_hash(post, a, 4); }} }}
    uint32_t r = norm({RET});
    if (hs) s->hash = fnv(fnv(fnv(s->hash, (uint32_t)pre), (uint32_t)post), r);
    if (n <= s_log_n)
        fprintf(stderr, "[SWAP] %s #%llu f=%u this=%08X ret=%08X esp+%d pre=%016llx post=%016llx\n", s->va,
                (unsigned long long)n, cj_det_frame, self, r, (int)(g_esp - esp0), (unsigned long long)pre, (unsigned long long)post);
    (void)fp0; swap_ck(s, n);
}}
'''

def _greg(r):
    return 'g_' + r

def keep_save(keep):
    # registers the original never writes (orig_writes): the native path must leave them as it found them
    if not keep: return ''
    return '/* keeps ' + ' '.join(keep) + ' */ ' + ' '.join(
        f'{"RecompXmm" if r.startswith("xmm") else "uint32_t"} k_{r} = {_greg(r)};' for r in keep) + ' '

def keep_restore(keep):
    return ''.join(f' {_greg(r)} = k_{r};' for r in keep)

def swap_c(manifest):
    out = [SWAP_HEAD]
    for m in manifest:
        ret = {'void': '0', 'i32': 'g_eax', 'i64': 'g_eax ^ g_edx',
               'fp': '(uint32_t)(int32_t)(g_fp_stack[g_fp_top & 7] * 4096.0)'}[m['ret']]
        sel = (f'mod_on("{m["va"].upper()}")' if m.get('mod') else f'selected("{m["va"].upper()}")')
        out.append(SWAP_FN.format(HASRET=0 if m['ret'] == 'void' else 1, SEL=sel, VA=m['va'].upper(), NAME=m['name'], UNIT=m['unit'], THIS='self' if m['thiscall'] else '0',
                                  TSIZE=m['this_size'], WORDS=m['arg_words'], POP=m['pop'], RET=ret,
                                  SAVE=keep_save(m.get('keep', [])), RESTORE=keep_restore(m.get('keep', []))))
    # Report for the frame-count based A/B (src/determinism.c, docs/determinism.md).
    out.append('\nstatic swap_stat *const s_all[] = {' + ', '.join('&st_' + m['va'].upper() for m in manifest) +
               '};\nvoid cj_swap_report(const char *tag)\n{\n'
               '    for (unsigned i = 0; i < sizeof s_all / sizeof s_all[0]; i++)\n'
               '        fprintf(stderr, "[SWAP] %s %s calls=%llu enter(gen body)=%llu esp_bad=%llu csr_bad=%llu run_hash=%016llx\\n",\n'
               '                tag, s_all[i]->va, (unsigned long long)s_all[i]->calls, (unsigned long long)s_all[i]->enter,\n'
               '                (unsigned long long)s_all[i]->esp_bad, (unsigned long long)s_all[i]->csr_bad,\n'
               '                (unsigned long long)s_all[i]->hash);\n}\n')
    return ''.join(out).replace('sub_' + 'X', 'sub_X')


# ---------------------------------------------------------------- scan (coverage)
def reason_key(e):
    pre = ''
    m = re.match(r'view [^:]+: (.*)$', e)
    if m: pre, e = 'in a view: ', m.group(1)
    e = re.sub(r'\b0x[0-9a-f]+\b|\b[0-9a-f]{8}\b|\b\d+\b', 'N', e)
    for pat, rep in [(r'^no layout for .*', 'no layout (record never completed in this TU)'),
                     (r'^constructor call .*', 'constructor call (local object / temporary)'),
                     (r'^struct return from .*', 'UDT return via hidden pointer (sret)'),
                     (r'^callee .* has no VA and no body', 'callee with no VA and no body (CRT/intrinsic)'),
                     (r'^function generated by a macro.*', 'function generated by a macro (not recoverable)'),
                     (r'^file-static .*', 'file-static without VA (internal linkage, no symbol)'),
                     (r'^global .*', 'global without VA in symbols.csv'),
                     (r'.* is not a non-virtual base of .*', 'base offset not found (template base naming)'),
                     (r'.*bitfield type', 'bitfield of enum/bool type'),
                     (r'^type .*\(CONSTANTARRAY\)', 'array-typed parameter/local in signature'),
                     (r'^vtable slot for .*', 'vtable slot not in classes.json'),
                     (r'^field .*', 'unsupported field type'),
                     (r'^type .*', 'unsupported type'),
                     (r'^call to .*', 'call through a pointer/field (function pointer)'),
                     (r'^base .* of .*', 'base not resolved'),
                     (r'.*: union$', 'union'), (r'.*: virtual base$', 'virtual base'),
                     (r'.*overlapping field.*', 'overlapping fields (anonymous union/struct)')]:
        if re.match(pat, e): return pre + rep
    return pre + e


def cmd_scan(a):
    tot = collections.Counter(); reasons = collections.Counter(); ok_bytes = all_bytes = 0; npp = 0
    for unit in a.units:
        try: g = Gen(unit)
        except SystemExit as e:
            print(e); continue
        ppg = None
        for m, c in list(g.defs.items()):
            r = g.fn.get(m)
            if not r or not c.location.file or c.location.file.name != g.path: continue
            if g.status.get(r['va'].lower(), {}).get('status') not in (('MATCH', 'DIFF') if a.diff else ('MATCH',)): continue
            sz = int(r['size']); all_bytes += sz
            def reset(x):
                x.native = {}; x.out_funcs = []; x.protos = []; x.need_rec = collections.OrderedDict(); x.opaque = {}
                x._fwdc = {}; x.enums = collections.OrderedDict(); x.fwd = set()
                return x
            g2 = reset(g)
            try:
                try:
                    g2.translate(c)
                except NeedPP:
                    # macro-using function: translate it from the preprocessed unit
                    if ppg is None: ppg = Gen(unit, pp=True)
                    g2 = reset(ppg)
                    c2 = ppg.defs.get(m)
                    if c2 is None: raise Unsupported('macro function not found in the preprocessed unit')
                    g2.translate(c2)
                    npp += 1
                g2.emit_views(); tot['ok'] += 1; ok_bytes += sz
                if a.v:
                    t = '\n'.join(g2.out_funcs)
                    ft = ('V' if 'cjg::vcall' in t else '-') + ('C' if 'cjg::call_' in t else '-') + \
                         ('P' if re.search(r'self->m_p\w+->', t) else '-')
                    print('OK  ', r['va'], r['size'], ft, qname(c), '@' + unit, g.status[r['va'].lower()]['status'])
            except Unsupported as e:
                tot['no'] += 1; reasons[reason_key(str(e))] += 1
                if a.v: print('NO  ', r['va'], qname(c), '--', e)
            except Exception as e:
                tot['err'] += 1; reasons['tool error: ' + type(e).__name__] += 1
                if a.v: print('ERR ', r['va'], qname(c), '--', repr(e)[:200])
    n = sum(tot.values())
    print(f'\n{"MATCH+DIFF" if a.diff else "MATCH"} functions: {n}  translatable: {tot["ok"]} ({100*tot["ok"]/max(n,1):.0f}%)  '
          f'bytes {ok_bytes}/{all_bytes} ({100*ok_bytes/max(all_bytes,1):.0f}%)  tool errors: {tot["err"]}  via preprocessed unit: {npp}')
    for k, v in reasons.most_common(25): print(f'  {v:5d}  {k}')


def cmd_sample(a):
    """Compile-clean rate: random sample (seed) of the OK functions of a `scan -v` listing,
    each generated alone as a swap target and compiled with clang++ -fsyntax-only."""
    import random, tempfile, glob
    ok = [l.split() for l in open(a.listing) if l.startswith('OK  ')]
    unit_of = {}
    for u in a.units:
        p = next((os.path.join(DECOMP, 'src', d, u + '.cpp') for d in ('game', 'enginex')
                  if os.path.exists(os.path.join(DECOMP, 'src', d, u + '.cpp'))), None)
        if p: unit_of[u] = p
    # map VA -> unit via functions.csv obj column
    va_unit = {r['va'].lower(): r['obj'].rsplit('.', 1)[0] for r in SYMS[0].values()}
    for row in ok:
        if row[-1].startswith('@'): row[-1] = row[-1][1:]; va_unit[row[1]] = row.pop()
    pick = random.Random(a.seed).sample(ok, min(a.n, len(ok)))
    here = os.path.dirname(os.path.abspath(__file__))
    inc = os.path.join(here, '..', '..', 'src', 'native')
    clean = 0; fails = collections.Counter()
    for row in pick:
        va = row[1]; unit = va_unit.get(va)
        d = tempfile.mkdtemp(prefix='hys_')
        r = subprocess.run([sys.executable, os.path.abspath(__file__), 'gen', 'unit=' + unit, va, '-o', d],
                           capture_output=True, text=True)
        if r.returncode:
            fails['gen failed'] += 1; print('GENFAIL', va, row[-1], r.stderr.strip()[-200:]); continue
        cpp = glob.glob(os.path.join(d, 'hy_*.cpp'))[0]
        r = subprocess.run(['clang++', '-std=c++17', '-fsyntax-only', '-w', '-I', inc, cpp], capture_output=True, text=True)
        if r.returncode == 0:
            clean += 1; print('CLEAN  ', va, row[-1])
        else:
            err = next((l for l in r.stderr.splitlines() if 'error:' in l), '?')
            fails[re.sub(r"'[^']*'", "'X'", err.split('error:', 1)[1].strip())[:90]] += 1
            print('FAIL   ', va, row[-1], '--', err.split('error:', 1)[1].strip()[:160])
    print(f'\nsample {len(pick)} (seed {a.seed}): {clean} compile clean ({100*clean/max(len(pick),1):.0f}%)')
    for k, v in fails.most_common(12): print(f'  {v:3d}  {k}')


def main():
    global SYMS
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    g = sp.add_parser('gen'); g.add_argument('spec', nargs='+', help='unit=NAME VA [VA...] [unit=NAME VA...]')
    g.add_argument('-o', '--out', default='build/hybrid')
    s = sp.add_parser('scan'); s.add_argument('units', nargs='+'); s.add_argument('-v', action='store_true')
    s.add_argument('--diff', action='store_true', help='also DIFF (written, not byte-matched) functions')
    m = sp.add_parser('sample'); m.add_argument('listing'); m.add_argument('units', nargs='*')
    m.add_argument('-n', type=int, default=80); m.add_argument('--seed', type=int, default=7)
    a = ap.parse_args()
    SYMS = load_symbols()
    {'gen': cmd_gen, 'scan': cmd_scan, 'sample': cmd_sample}[a.cmd](a)


if __name__ == '__main__':
    main()
