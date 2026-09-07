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
  var pendingCaretSig = null;       // focused caret sig this compose (per apply)

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
      // Virtualized: the engine emits only a WINDOW of lines ([op.top,
      // op.top+N)) plus the document line count (op.total). We render the
      // window between two spacer divs sized to the off-window lines, so the
      // native scrollbar spans the whole document while the DOM holds only
      // the visible window (O(window), not O(document)). Line numbers stay
      // ABSOLUTE (op.top + local index), so renderLine's caret/selection
      // matching is unchanged.
      // Preserve the scroll position across the DOM swap: clearing the
      // container empties it, so the browser would clamp scrollTop to 0 and
      // NOT restore it — a scroll-driven recompose would snap to the top.
      // A given document line sits at line*lineHeight regardless of the
      // window offset, so the same scrollTop shows the same lines.
      var savedScroll = el.scrollTop;
      el.textContent = '';
      if (op.tabwidth) el.style.tabSize = String(op.tabwidth);
      var top = op.top || 0;
      var lines = op.lines || [];
      var total = (op.total != null) ? op.total : (top + lines.length);
      // The focused edit's caret signature — so madcApply scrolls the caret
      // into view ONLY when it actually moved (a keyboard/edit gesture), never
      // on a scroll-driven recompose (which must leave the view where the user
      // scrolled it).
      if (op.focus && op.caret)
        pendingCaretSig = op.key + '#' + op.caret.line + ':' + op.caret.col;
      var topSpacer = document.createElement('div');
      topSpacer.className = 'vspacer';
      el.appendChild(topSpacer);
      var firstLine = null;
      for (var l = 0; l < lines.length; l++) {
        var ln = renderLine(lines[l], top + l, op.focus ? op.caret : null, op.sel);
        el.appendChild(ln);
        if (!firstLine) firstLine = ln;
      }
      var bottomSpacer = document.createElement('div');
      bottomSpacer.className = 'vspacer';
      el.appendChild(bottomSpacer);
      // The REAL rendered line height (one layout read per compose, over the
      // small windowed DOM) makes the spacers and the scroll math agree.
      var lh = firstLine ? firstLine.getBoundingClientRect().height : (el._lineH || 0);
      if (lh > 0) {
        topSpacer.style.height = (top * lh) + 'px';
        var below = total - top - lines.length;
        bottomSpacer.style.height = (below > 0 ? below * lh : 0) + 'px';
        el._lineH = lh;
      }
      el._winTop = top;
      el._winCount = lines.length;
      el._total = total;
      el._key = op.key;
      ensureScrollListener(el);
      // Restore the scroll position now that the spacers give the container
      // its full scrollHeight (the clamp is gone). Record what we set: this
      // programmatic write fires a scroll event, and the listener must NOT
      // treat it as a user scroll (that would re-post -> recompose -> restore
      // -> ... a feedback loop that pins the wheel in place = "jiggle").
      el.scrollTop = savedScroll;
      el._modelScrollTop = el.scrollTop;
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

  // A virtualized edit reports viewport scrolls so the engine can move its
  // window (web_model owns the top line, like the grid model's _scroll). We
  // post ONLY when the view nears the rendered window's edge: the overscan
  // buffer covers small scrolls and the caret's scrollIntoView nudges, so
  // typing never round-trips and there is no scroll<->recompose feedback
  // loop. The engine echoes a new window; the native scroll position is
  // preserved across the DOM swap because the total height is stable.
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
        // Skip the scroll event our own restore fired (else post -> recompose
        // -> restore -> post loops as a jiggle). A real wheel/scrollbar move
        // changes scrollTop by far more than 1px away from what we set.
        if (Math.abs(el.scrollTop - (el._modelScrollTop || 0)) < 1) return;
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
    pendingCaretSig = null;                 // set by the focused edit's applyNode
    for (var i = 0; i < ops.length; i++) {
      var op = ops[i];
      if (op.op === 'root') visited = new Set();
      else if (op.op === 'node') { applyNode(op); visited.add(op.key); }
      else if (op.op === 'end') prune();
    }
    kb.focus();
    // Keyboard navigation must move the viewport to follow the caret, but a
    // scroll-driven recompose must NOT: scrolling into view only when the
    // focused caret actually moved keeps mouse-wheel / scrollbar scrolling
    // from snapping back to the caret. `nearest` is a no-op when the caret is
    // already visible.
    if (pendingCaretSig !== lastCaretSig) {
      var car = document.querySelector('.caret');
      if (car && car.scrollIntoView) car.scrollIntoView({ block: 'nearest', inline: 'nearest' });
      lastCaretSig = pendingCaretSig;
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
