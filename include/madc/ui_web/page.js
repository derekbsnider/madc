// ui_web/page.js — the web target's ONE embedded page: the DOM applier for
// the engine's keyed operations (web_model::compose) and the input relay
// that posts raw key spellings and printable runs back (web_model::
// apply_input). No key -> action logic lives here and no editor knowledge:
// the page reconciles what the engine composed and reports what the user
// did, in the TUI's own vocabulary (tui_key_name spellings).
(function () {
  'use strict';
  var root = document.getElementById('root');
  var kb = document.getElementById('kb');
  var measure = document.getElementById('measure');
  var nodes = new Map();            // key -> element
  var visited = new Set();          // keys seen since the last "root" op
  var lastRows = 0, lastCols = 0;
  var lastCaretSig = null;          // focused caret sig at the last scroll-in
  var pendingCaret = null;          // focused edit's caret {line,col} this apply
  var pendingCaretEl = null;        // the focused edit element this apply

  function post(obj) {
    if (typeof window.madc === 'function') {
      try { window.madc(JSON.stringify(obj)); } catch (e) { /* the host is gone */ }
    }
  }

  // ---- the applier -------------------------------------------------------
  function elementFor(op) {
    var el = nodes.get(op.key);
    if (!el) {
      el = document.createElement('div');
      el.dataset.key = op.key;
      nodes.set(op.key, el);
    }
    el.className = 'node ' + op['class'] + (op.focus ? ' focus' : '') +
                   (op.popup ? ' popup' : '') + (op.tabs ? ' has-tabs' : '');
    // Slice 3 workbench: a `region` node docks into a grid slot (the
    // page's own CSS placement, keyed by data-region), and a parent that
    // holds region'd children becomes the workbench grid. Pre-order means
    // the parent element already exists when a region'd child arrives, so
    // the class is set deterministically each compose cycle (the parent's
    // own op reset its className first, region'd children re-add it).
    if (op.region) el.dataset.region = op.region; else delete el.dataset.region;
    // The @gui theme (slice 3): a node's `theme` bag sets CSS custom
    // properties on the document root, so the workbench CSS reads them via
    // var(--name, fallback). The composer attaches it to the root group.
    if (op.theme) {
      for (var tk in op.theme)
        if (Object.prototype.hasOwnProperty.call(op.theme, tk))
          document.documentElement.style.setProperty('--' + tk, op.theme[tk]);
    }
    var parent = op.parent ? nodes.get(op.parent) : null;
    (parent || root).appendChild(el);   // pre-order arrival keeps sibling order
    if (op.region && parent) parent.classList.add('workbench');
    return el;
  }

  function text(el, s) { el.textContent = s == null ? '' : String(s); }

  function span(cls, s) {
    var e = document.createElement('span');
    e.className = cls;
    e.textContent = s;
    return e;
  }

  // One edit line: per-character class sets (highlight spans, selection,
  // caret) coalesced into runs. Columns are BYTE offsets from the engine;
  // the page's text is the same bytes decoded, so ASCII-heavy documents
  // line up exactly and multi-byte runs stay within their span.
  function renderLine(row, lineNo, caret, sel) {
    var t = row.t || '';
    var n = t.length;
    var classes = new Array(n + 1);
    for (var i = 0; i <= n; i++) classes[i] = '';
    var spans = row.s || [];
    for (var k = 0; k < spans.length; k++) {
      var start = spans[k][0], len = spans[k][1], cls = ' c-' + spans[k][2];
      for (var c = start; c < start + len && c < n; c++) classes[c] += cls;
    }
    if (sel) {
      var l0 = sel[0][0], c0 = sel[0][1], l1 = sel[1][0], c1 = sel[1][1];
      if (lineNo >= l0 && lineNo <= l1) {
        var from = lineNo === l0 ? c0 : 0;
        var to = lineNo === l1 ? c1 : n + 1;
        for (var s2 = from; s2 < to && s2 <= n; s2++) classes[s2] += ' sel';
      }
    }
    if (caret && caret.line === lineNo) {
      var cc = Math.min(caret.col, n);
      classes[cc] += ' caret';
    }
    var line = document.createElement('div');
    line.className = 'line';
    var run = '', runCls = null;
    for (var j = 0; j <= n; j++) {
      var ch = j < n ? t[j] : (classes[j].indexOf('caret') >= 0 ? ' ' : '');
      if (ch === '') break;
      if (classes[j] !== runCls && run) {
        line.appendChild(runCls ? span(runCls.trim(), run) : document.createTextNode(run));
        run = '';
      }
      runCls = classes[j];
      run += ch;
    }
    if (run) line.appendChild(runCls ? span(runCls.trim(), run) : document.createTextNode(run));
    return line;
  }

  function applyNode(op) {
    var el = elementFor(op);
    var cls = op['class'];
    if (cls === 'heading') {
      el.textContent = '';
      el.appendChild(span('label', op.label || ''));
      el.appendChild(span('text', op.text || ''));
    } else if (cls === 'status' || cls === 'content' || cls === 'item') {
      if (cls === 'status' && op.items) {
        // The status bar as a justified item pair (slice 3): .status is
        // already flex space-between, so a left and a right span sit apart.
        el.textContent = '';
        el.appendChild(span('sb-left', op.items.left || ''));
        el.appendChild(span('sb-right', op.items.right || ''));
      } else {
        text(el, op.text);
      }
    } else if (cls === 'action') {
      text(el, '[' + (op.label || '') + ']');
    } else if (cls === 'list') {
      el.textContent = '';
      if (op.label != null) el.appendChild(span('label', op.label + ':'));
    } else if (cls === 'choice') {
      el.textContent = '';
      if (op.list) el.classList.add('list');
      if (op.label != null) el.appendChild(span('label', op.label));
      var opts = op.opts || [];
      for (var i = 0; i < opts.length; i++)
        el.appendChild(span('opt' + (i === op.sel ? ' sel' : ''), opts[i]));
    } else if (cls === 'edit') {
      // Virtualized (native-scroll windowing — the CodeMirror / react-window /
      // TanStack Virtual model). The engine emits only a WINDOW of lines
      // ([op.top, op.top+N)) plus the document line count (op.total). The
      // native `overflow:auto` container (el) is the SOLE owner of scrollTop:
      // it holds three PERSISTENT children — a top spacer, the line layer, a
      // bottom spacer — whose heights always sum to total*lineHeight. So the
      // scrollHeight is constant and the browser keeps scrollTop for free.
      // Each compose mutates ONLY the spacer heights and the line-layer
      // children; it NEVER clears `el` and NEVER writes `el.scrollTop` (that
      // is what caused the reset/restore/feedback-loop jiggle). Line numbers
      // stay ABSOLUTE (op.top + local index) so renderLine's caret/selection
      // matching is unchanged.
      var vtop = el._vtop, vlines = el._vlines, vbot = el._vbot;
      if (!vlines) {
        el.textContent = '';                 // ONE-TIME init only
        vtop = document.createElement('div'); vtop.className = 'v-top';
        vlines = document.createElement('div'); vlines.className = 'v-lines';
        vbot = document.createElement('div'); vbot.className = 'v-bot';
        el.appendChild(vtop); el.appendChild(vlines); el.appendChild(vbot);
        el._vtop = vtop; el._vlines = vlines; el._vbot = vbot;
        ensureScrollListener(el);
      }
      if (op.tabwidth) el.style.tabSize = String(op.tabwidth);
      var top = op.top || 0;
      var lines = op.lines || [];
      var total = (op.total != null) ? op.total : (top + lines.length);
      // Rebuild ONLY the inner line layer (the spacers persist and keep
      // scrollHeight >= scrollTop, so clearing vlines never clamps scrollTop).
      vlines.textContent = '';
      for (var l = 0; l < lines.length; l++)
        vlines.appendChild(renderLine(lines[l], top + l,
                                      op.focus ? op.caret : null, op.sel));
      // The rendered line height (measured once) sizes the spacers so the
      // scrollbar spans the whole document.
      if (!el._lineH && vlines.firstChild)
        el._lineH = vlines.firstChild.getBoundingClientRect().height;
      var lh = el._lineH || 0;
      if (lh > 0) {
        vtop.style.height = (top * lh) + 'px';
        var below = total - top - lines.length;
        vbot.style.height = (below > 0 ? below * lh : 0) + 'px';
      }
      el._winTop = top;
      el._winCount = lines.length;
      el._total = total;
      el._key = op.key;
      // The focused edit's caret — madcApply scrolls it into view ONLY when it
      // moved (a keyboard/edit gesture), never on a scroll-driven recompose.
      if (op.focus && op.caret) { pendingCaret = op.caret; pendingCaretEl = el; }
    }
    // group / separator / node: structure only — children carry it.
  }

  function prune() {
    nodes.forEach(function (el, key) {
      if (!visited.has(key)) {
        if (el.parentNode) el.parentNode.removeChild(el);
        nodes['delete'](key);
      }
    });
  }

  // A virtualized edit REPORTS viewport scrolls so the engine can move its
  // window (web_model owns the top line, like the grid model's _scroll). The
  // handler is strictly READ-ONLY: it reads scrollTop and posts a request when
  // the view nears the rendered window's edge — it NEVER writes scrollTop (the
  // cardinal rule of native-scroll windowing; writing it here is what looped).
  // The engine echoes a new window; because the container's scrollHeight is
  // constant (persistent spacers) and we never touch scrollTop, the native
  // position is preserved for free — no reset, no restore, no loop.
  // rAF bound to window: WebKit throws "Illegal invocation" on a detached
  // requestAnimationFrame, so bind it (setTimeout is the fallback).
  var raf = window.requestAnimationFrame
    ? window.requestAnimationFrame.bind(window)
    : function (f) { setTimeout(f, 16); };

  function ensureScrollListener(el) {
    if (el._scrollBound) return;
    el._scrollBound = true;
    var pending = false;
    el.addEventListener('scroll', function () {
      if (pending) return;
      pending = true;
      raf(function () {
        pending = false;
        var lh = el._lineH || 0;
        if (lh <= 0) return;
        var visTop = el.scrollTop / lh;
        var visRows = el.clientHeight / lh;
        var winTop = el._winTop || 0;
        var winEnd = winTop + (el._winCount || 0);
        var MARGIN = 2;
        if (visTop < winTop + MARGIN || visTop + visRows > winEnd - MARGIN)
          post({ kind: 'scroll', key: el._key, top: Math.max(0, Math.floor(visTop)) });
      });
    });
  }

  window.madcApply = function (ops) {
    pendingCaret = null;                    // set by the focused edit's applyNode
    pendingCaretEl = null;
    for (var i = 0; i < ops.length; i++) {
      var op = ops[i];
      if (op.op === 'root') visited = new Set();
      else if (op.op === 'node') { applyNode(op); visited.add(op.key); }
      else if (op.op === 'end') prune();
    }
    kb.focus();
    // Scroll the caret into view ONLY when it actually moved (a keyboard/edit
    // gesture) — never on a scroll-driven recompose, so mouse-wheel/scrollbar
    // scrolling is left exactly where the user put it. Pure arithmetic against
    // the native scroll position (no measurement, no fighting): a no-op when
    // the caret is already visible. This is the ONLY place we write scrollTop.
    if (pendingCaretEl && pendingCaret) {
      var sig = pendingCaretEl._key + '#' + pendingCaret.line + ':' + pendingCaret.col;
      if (sig !== lastCaretSig) {
        var lh = pendingCaretEl._lineH || 0;
        if (lh > 0) {
          var cy = pendingCaret.line * lh;
          var vTop = pendingCaretEl.scrollTop;
          var vH = pendingCaretEl.clientHeight;
          if (cy < vTop) pendingCaretEl.scrollTop = cy;
          else if (cy + lh > vTop + vH) pendingCaretEl.scrollTop = cy + lh - vH;
        }
        lastCaretSig = sig;
      }
    }
  };

  // ---- input: raw keys in the TUI vocabulary, printable runs as text ----
  var named = {
    Enter: 'enter', Tab: 'tab', Backspace: 'backspace', Escape: 'esc',
    ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
    Home: 'home', End: 'end', PageUp: 'pgup', PageDown: 'pgdn',
    Delete: 'del', Insert: 'ins'
  };
  var ctrlPunct = { '\\': '^\\', ']': '^]', '^': '^^', '_': '^_' };

  function keySpelling(e) {
    if (named[e.key]) return named[e.key];
    if (e.ctrlKey && !e.altKey && !e.metaKey && e.key.length === 1) {
      if (/[a-zA-Z]/.test(e.key)) return '^' + e.key.toLowerCase();
      if (ctrlPunct[e.key]) return ctrlPunct[e.key];
      if (e.key === ' ') return 'space';
    }
    return null;
  }

  kb.addEventListener('keydown', function (e) {
    var key = keySpelling(e);
    if (key) {
      e.preventDefault();
      post({ kind: 'key', key: key });
    }
    // Printables (no ctrl/alt/meta) reach the input event below as text.
  });
  kb.addEventListener('input', function () {
    var t = kb.value;
    kb.value = '';
    if (t) post({ kind: 'text', text: t });
  });
  document.addEventListener('mousedown', function () { setTimeout(function () { kb.focus(); }, 0); });

  // ---- viewport facts: rows x cols in text cells -------------------------
  function reportSize() {
    var r = measure.getBoundingClientRect();
    if (!r.width || !r.height) return;
    var cols = Math.max(1, Math.floor(root.clientWidth / r.width));
    var rows = Math.max(1, Math.floor(root.clientHeight / r.height));
    if (rows !== lastRows || cols !== lastCols) {
      lastRows = rows; lastCols = cols;
      post({ kind: 'resize', rows: rows, cols: cols });
    }
  }
  if (typeof ResizeObserver === 'function') new ResizeObserver(reportSize).observe(root);
  window.addEventListener('resize', reportSize);

  // ---- the test seam: the rendered text as one event -------------------
  // One line per text-bearing node, in tree order: a bar as "label text",
  // an edit as its lines, a choice as its options with the selected one in
  // brackets — the page's own text form of what it shows.
  window.madcSnapshot = function () {
    var lines = [];
    root.querySelectorAll('.node').forEach(function (el) {
      var c = el.classList;
      if (c.contains('heading')) {
        lines.push(((el.children[0] ? el.children[0].textContent : '') + ' ' +
                    (el.children[1] ? el.children[1].textContent : '')).trim());
      } else if (c.contains('edit')) {
        el.querySelectorAll('.line').forEach(function (l) { lines.push(l.textContent); });
      } else if (c.contains('choice')) {
        var parts = [];
        el.querySelectorAll('.opt').forEach(function (o) {
          parts.push(o.classList.contains('sel') ? '[' + o.textContent + ']' : o.textContent);
        });
        lines.push(parts.join(' '));
      } else if (c.contains('status') || c.contains('content') || c.contains('item') || c.contains('action')) {
        lines.push(el.textContent);
      }
    });
    post({ kind: 'snapshot', text: lines.join('\n') });
  };

  window.addEventListener('load', function () { kb.focus(); reportSize(); });
})();
