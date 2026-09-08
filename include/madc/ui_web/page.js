// ui_web/page.js — the web target's ONE embedded page: the DOM applier for
// the engine's keyed operations (web_model::compose) and the input relay
// that posts raw key spellings, printable runs and pointer positions back
// (web_model::apply_input). No key -> action logic lives here and no
// editor knowledge: the page reconciles what the engine composed and
// reports what the user did — keys in the TUI's own vocabulary
// (tui_key_name spellings), a pointer as the line and column it hit.
(function () {
  'use strict';
  var root = document.getElementById('root');
  var kb = document.getElementById('kb');
  var measure = document.getElementById('measure');
  var nodes = new Map();            // key -> element
  var visited = new Set();          // keys seen since the last "root" op
  var placed = new Map();           // parent element -> children placed this cycle
  var lastRows = 0, lastCols = 0;

  function post(obj) {
    if (typeof window.madc === 'function') {
      try { window.madc(JSON.stringify(obj)); } catch (e) { /* the host is gone */ }
    }
  }

  // ---- workbench slots ---------------------------------------------------
  // A container holding region'd children is the workbench grid. Each
  // REGION is a slot element of it (created on demand, one per region);
  // the grid places the SLOT, and the nodes docked into that region STACK
  // inside it in tree order — the editor slot holds every window's status
  // line and edit node (S5: the ^K O split), the panel every docked panel —
  // so two nodes in one region never overlap, which is what a grid area
  // would do with two direct children. A child with no region flows to
  // the `foot` slot (the message line) instead of falling into whichever
  // grid cell auto-placement finds empty (the rail).
  function slotOf(container, region) {
    var s = container._slots && container._slots.get(region);
    if (!s) {
      s = document.createElement('div');
      s.className = 'slot';
      s.dataset.slot = region;
      container.appendChild(s);
      if (!container._slots) container._slots = new Map();
      container._slots.set(region, s);
    }
    return s;
  }

  function makeWorkbench(container) {
    if (container.classList.contains('workbench')) return;
    container.classList.add('workbench');
    // Siblings placed before the first region'd child arrived this cycle
    // were laid directly; they belong to the foot, ahead of what follows.
    var foot = slotOf(container, 'foot');
    var direct = Array.prototype.filter.call(container.children,
      function (c) { return c.classList.contains('node'); });
    for (var i = 0; i < direct.length; i++) foot.appendChild(direct[i]);
    if (direct.length) placed.set(foot, (placed.get(foot) || 0) + direct.length);
  }

  // A slot holding more than one edit node is a STACK of windows: the
  // focused one is marked and each window's header separates it.
  function markStacks() {
    document.querySelectorAll('.slot').forEach(function (s) {
      var n = 0;
      for (var c = s.firstElementChild; c; c = c.nextElementSibling)
        if (c.classList.contains('edit')) n++;
      s.classList.toggle('stacked', n > 1);
    });
  }

  // The text cell's height — the same cell the viewport report measures —
  // for a fixed-height window (the composer's `rows` hint).
  function cellHeight() {
    return measure.getBoundingClientRect().height;
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
    // Slice 3 workbench: a `region` node docks into that region's slot of
    // its parent's grid (the page's own CSS placement, keyed by data-slot),
    // and a parent that holds region'd children becomes the workbench
    // grid. Pre-order means the parent element already exists when a
    // region'd child arrives, so the class is set deterministically each
    // compose cycle (the parent's own op reset its className first,
    // region'd children re-add it). data-region on the node itself names
    // where it asked to go.
    if (op.region) el.dataset.region = op.region; else delete el.dataset.region;
    // The @gui theme (slice 3): a node's `theme` bag sets CSS custom
    // properties on the document root, so the workbench CSS reads them via
    // var(--name, fallback). The composer attaches it to the root group.
    if (op.theme) {
      for (var tk in op.theme)
        if (Object.prototype.hasOwnProperty.call(op.theme, tk))
          document.documentElement.style.setProperty('--' + tk, op.theme[tk]);
    }
    // Placement: ops arrive pre-order, siblings in order, so each parent's
    // next expected slot is a running count. An element already sitting in
    // its slot is LEFT ALONE — appendChild on an attached node detaches and
    // re-attaches its whole subtree, and for the editor (thousands of line
    // elements) that was a 100 ms renderer rebuild on every keystroke
    // (measured 2026-09-07: re-append 100 ms; a two-line patch 4 ms).
    var container = (op.parent ? nodes.get(op.parent) : null) || root;
    if (container !== root && (op.region || container.classList.contains('workbench'))) {
      makeWorkbench(container);
      container = slotOf(container, op.region || 'foot');
    }
    var slot = placed.get(container) || 0;
    if (container.children[slot] !== el)
      container.insertBefore(el, container.children[slot] || null);
    placed.set(container, slot + 1);
    return el;
  }

  function text(el, s) { el.textContent = s == null ? '' : String(s); }

  // One side of the status bar: its segments as discrete items.
  function segments(cls, segs) {
    var side = document.createElement('span');
    side.className = cls;
    var list = Array.isArray(segs) ? segs : [];
    for (var i = 0; i < list.length; i++) {
      var sg = list[i] || {};
      var item = document.createElement('span');
      item.className = 'sb-seat' + (sg.seat ? ' sb-' + sg.seat : '');
      if (sg.label) item.appendChild(span('sb-label', sg.label));
      item.appendChild(span('sb-text', sg.text || ''));
      side.appendChild(item);
    }
    return side;
  }

  function span(cls, s) {
    var e = document.createElement('span');
    e.className = cls;
    e.textContent = s;
    return e;
  }

  // ---- the prompts as dialogs (S6) ---------------------------------------
  // The core's prompt as a QUICK INPUT: the label, the input text the core
  // holds (every key still travels the one input path — the page draws the
  // text and a caret after it, it never edits), and the two keys every
  // prompt answers to. The composer's `popup` hint floats the node.
  function quickInput(el, p) {
    el.classList.add('quickinput');
    el.textContent = '';
    el.appendChild(span('qi-label', p.label || ''));
    var box = document.createElement('div');
    box.className = 'qi-input';
    box.appendChild(span('qi-text', p.input || ''));
    box.appendChild(span('caret', ' '));
    el.appendChild(box);
    el.appendChild(span('qi-hint', 'Enter \u21B5 confirms \u00B7 Esc cancels'));
  }

  // A question as a DIALOG: the label and one button per answer; a button
  // posts the action the composer named for it — what its key does in the
  // terminal — through the same {kind:'action'} a menu item posts.
  function confirmBox(el, c) {
    el.classList.add('confirm');
    el.textContent = '';
    el.appendChild(span('cf-question', c.label || ''));
    var row = document.createElement('div');
    row.className = 'cf-buttons';
    var choices = Array.isArray(c.choices) ? c.choices : [];
    for (var i = 0; i < choices.length; i++) {
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'cf-btn';
      b.dataset.action = choices[i].action || '';
      b.textContent = choices[i].label || '';
      row.appendChild(b);
    }
    el.appendChild(row);
  }

  // A BYTE column in a row's UTF-8 text -> the index into the page's
  // decoded (UTF-16) string: one unit per code point below U+10000, two for
  // a four-byte sequence (a surrogate pair). The engine speaks bytes (its
  // caret, selection and span offsets are document byte offsets), the DOM
  // speaks units; this is the ONE conversion, the inverse of the engine's
  // web_byte_col. A column past the text clamps to its length.
  function unitsOf(t, bytes) {
    var i = 0, b = 0;
    while (i < t.length && b < bytes) {
      var code = t.codePointAt(i);
      b += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
      i += code >= 0x10000 ? 2 : 1;
    }
    return i;
  }

  // One edit line: per-character class sets (highlight spans, selection,
  // caret) coalesced into runs. Columns arrive as BYTE offsets from the
  // engine and are converted to string indices per line (unitsOf), so a
  // multi-byte character costs one cell, not two or three.
  function renderLine(row, lineNo, caret, sel) {
    var t = row.t || '';
    var n = t.length;
    var classes = new Array(n + 1);
    for (var i = 0; i <= n; i++) classes[i] = '';
    var spans = row.s || [];
    for (var k = 0; k < spans.length; k++) {
      var start = unitsOf(t, spans[k][0]), end = unitsOf(t, spans[k][0] + spans[k][1]);
      var cls = ' c-' + spans[k][2];
      for (var c = start; c < end && c < n; c++) classes[c] += cls;
    }
    if (sel) {
      var l0 = sel[0][0], c0 = sel[0][1], l1 = sel[1][0], c1 = sel[1][1];
      if (lineNo >= l0 && lineNo <= l1) {
        var from = lineNo === l0 ? unitsOf(t, c0) : 0;
        var to = lineNo === l1 ? unitsOf(t, c1) : n + 1;
        for (var s2 = from; s2 < to && s2 <= n; s2++) classes[s2] += ' sel';
      }
    }
    if (caret && caret.line === lineNo) {
      var cc = unitsOf(t, caret.col);
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

  // The caret / selection state one line carries, as a comparable key:
  // two lines with equal keys and equal rows render identically, so a
  // line whose key did not change is left alone.
  function deco(lineNo, caret, sel) {
    var d = '';
    if (caret && caret.line === lineNo) d = 'c' + caret.col;
    if (sel) {
      var l0 = sel[0][0], l1 = sel[1][0];
      if (lineNo >= l0 && lineNo <= l1)
        d += '|s' + (lineNo === l0 ? sel[0][1] : 0) + '-' + (lineNo === l1 ? sel[1][1] : 'e');
    }
    return d;
  }

  // The editor line-DOM is applied INCREMENTALLY (web_model keeps the rows
  // it last emitted per key and sends ONE splice): the element holds its
  // rows (el._rows) and the caret and selection it drew (el._caret,
  // el._sel), with one line element per row. The container is cleared only
  // for a full paint, so native scrolling is never disturbed; a keystroke
  // touches the spliced lines and the lines whose caret / selection state
  // changed — nothing else. Returns true when the caret moved (or the node
  // was painted in full), the one case the caret is scrolled into view.
  function applyEdit(el, op) {
    if (op.tabwidth) el.style.tabSize = String(op.tabwidth);
    // A fixed height in text cells (the composer's `rows` hint — an
    // inactive window of a split, the terminal's own row budget); without
    // it the node flexes, and the active window takes what the fixed ones
    // leave. Measured before layout the cell is 0 — then the line height.
    if (op.rows) {
      var h = cellHeight();
      el.dataset.rows = op.rows;
      el.style.flex = '0 0 ' + (h ? (op.rows * h) + 'px' : (op.rows * 1.35) + 'em');
    } else {
      delete el.dataset.rows;
      el.style.flex = '';
    }
    var caret = op.focus ? op.caret : null;
    var sel = op.sel || null;
    if (op.lines) {
      // Full paint: this key's first render, or the first after a resync.
      el.textContent = '';
      el._rows = op.lines;
      for (var l = 0; l < el._rows.length; l++)
        el.appendChild(renderLine(el._rows[l], l, caret, sel));
      el._caret = caret; el._sel = sel;
      return true;
    }
    var rows = el._rows || [];
    var patch = op.patch || null;
    var ins = patch ? patch.ins.length : 0, del = patch ? patch.del : 0;
    // What the page holds must be the engine's basis: the row count it
    // expects before the splice, and one line element per row. Otherwise
    // ask for a full paint (a render the platform dropped before the page
    // loaded, a lost eval) — the thin client requests what it lacks.
    if (op.nlines == null || rows.length !== op.nlines - ins + del ||
        el.children.length !== rows.length) {
      post({ kind: 'resync' });
      return false;
    }
    var oldCaret = el._caret || null, oldSel = el._sel || null;
    var at = patch ? patch.at : 0;
    if (patch) {
      var ref = el.children[at + del] || null;   // the line after the splice
      for (var d = 0; d < del; d++) el.removeChild(el.children[at]);
      for (var k = 0; k < ins; k++)
        el.insertBefore(renderLine(patch.ins[k], at + k, caret, sel), ref);
      rows = rows.slice(0, at).concat(patch.ins, rows.slice(at + del));
      el._rows = rows;
    }
    // Every other line kept its row; re-render the ones whose caret /
    // selection state changed (its old index is its new index shifted
    // past the splice).
    for (var j = 0; j < rows.length; j++) {
      if (patch && j >= at && j < at + ins) continue;
      var i = j < at ? j : j - ins + del;
      if (deco(i, oldCaret, oldSel) !== deco(j, caret, sel))
        el.replaceChild(renderLine(rows[j], j, caret, sel), el.children[j]);
    }
    el._caret = caret; el._sel = sel;
    return !!caret && (!oldCaret || oldCaret.line !== caret.line || oldCaret.col !== caret.col);
  }

  function applyNode(op) {
    var el = elementFor(op);
    var cls = op['class'];
    if (cls === 'heading') {
      el.textContent = '';
      el.appendChild(span('label', op.label || ''));
      el.appendChild(span('text', op.text || ''));
    } else if (cls === 'status' || cls === 'content' || cls === 'item') {
      el.classList.remove('quickinput', 'confirm');
      if (cls === 'status' && op.items) {
        // The status bar as chrome (S3): each side is a row of SEGMENTS —
        // the composer's expanded format seats {seat, label, text} — laid
        // as discrete items (.sb-seat.sb-<letter>: a label span when the
        // format gave one, then the text), left and right apart (.status
        // is flex space-between).
        el.textContent = '';
        el.appendChild(segments('sb-left', op.items.left));
        el.appendChild(segments('sb-right', op.items.right));
      } else if (op.prompt) {
        quickInput(el, op.prompt);
      } else if (op.confirm) {
        confirmBox(el, op.confirm);
      } else {
        text(el, op.text);
      }
      // A popup a press outside dismisses: the composer's action, kept on
      // the element for the mousedown handler (data, never a key).
      if (op.dismiss) el.dataset.dismiss = op.dismiss; else delete el.dataset.dismiss;
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
      return applyEdit(el, op);
    }
    // group / separator / node: structure only — children carry it.
    return false;
  }

  function prune() {
    nodes.forEach(function (el, key) {
      if (!visited.has(key)) {
        if (el.parentNode) el.parentNode.removeChild(el);
        nodes['delete'](key);
      }
    });
  }

  window.madcApply = function (ops) {
    var moved = false;
    for (var i = 0; i < ops.length; i++) {
      var op = ops[i];
      if (op.op === 'root') { visited = new Set(); placed = new Map(); }
      else if (op.op === 'node') { moved = applyNode(op) || moved; visited.add(op.key); }
      else if (op.op === 'end') { prune(); markStacks(); }
    }
    kb.focus();
    // Keyboard navigation must move the viewport, not just the caret: the
    // edit div is overflow:auto, so a caret past the fold is off-screen until
    // its element is scrolled into view — ONLY when the caret moved (or an
    // editor was painted in full): a recompose that left the caret where it
    // was (a resize, a wake) never pulls a wheel-scrolled view back to it.
    if (!moved) return;
    var car = document.querySelector('.caret');
    if (car && car.scrollIntoView) car.scrollIntoView({ block: 'nearest', inline: 'nearest' });
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

  // ---- pointer: a gesture on an editor, hit-tested to (line, column) ----
  // The page knows geometry, the engine knows the document: a press, a
  // drag and a release on an edit node are posted as the index of the line
  // element and the UTF-16 column the browser's caret hit test resolved to
  // (in the TUI vocabulary's spirit — a fact about where, never what to do).
  // web_model turns them into a byte offset over the rows it emitted for
  // that key and the application places its caret and selection: ONE caret
  // model for mouse and keyboard. Native selection is suppressed — the
  // engine draws the selection it owns. A drag posts only when the
  // position changed, so a held button costs nothing while it rests.
  var drag = null;                  // { el, line, col } while a button is down

  function editOf(target) {
    for (var el = target; el && el !== root; el = el.parentNode)
      if (el.classList && el.classList.contains('edit')) return el;
    return null;
  }

  function lineHeight(ed) {
    var f = ed.firstElementChild;
    return (f && f.getBoundingClientRect().height) || 16;
  }

  // The (line, col) under (x, y) within `ed`: the point is clamped into
  // the visible line area (a press past the last line lands on it, one
  // in the padding on the nearest line), the browser's caret hit test
  // yields a text position, the .line ancestor is the row and the text
  // before the position its UTF-16 column. Past the text on a line the
  // hit test itself resolves to the line's end.
  function hitPos(ed, x, y) {
    var first = ed.firstElementChild, last = ed.lastElementChild;
    if (!first) return null;
    var r = ed.getBoundingClientRect();
    var top = Math.max(r.top, first.getBoundingClientRect().top);
    var bottom = Math.min(r.top + ed.clientHeight, last.getBoundingClientRect().bottom) - 1;
    y = Math.min(Math.max(y, top), bottom);
    x = Math.min(Math.max(x, r.left), r.left + ed.clientWidth - 1);
    var node = null, off = 0;
    if (document.caretPositionFromPoint) {
      var p = document.caretPositionFromPoint(x, y);
      if (p) { node = p.offsetNode; off = p.offset; }
    } else if (document.caretRangeFromPoint) {
      var cr = document.caretRangeFromPoint(x, y);
      if (cr) { node = cr.startContainer; off = cr.startOffset; }
    }
    var line = null;
    for (var n = node; n && n !== ed; n = n.parentNode)
      if (n.parentNode === ed) { line = n; break; }
    if (!line) {
      // The hit test answered outside the rows (the container itself, a
      // gap): the line element under the clamped point, at its end.
      for (var m = document.elementFromPoint(x, y); m && m !== ed; m = m.parentNode)
        if (m.parentNode === ed) { line = m; break; }
      if (!line) return null;
      node = line; off = line.childNodes.length;
    }
    var rg = document.createRange();
    rg.setStart(line, 0);
    try { rg.setEnd(node, off); } catch (e) { rg.selectNodeContents(line); }
    return { line: Array.prototype.indexOf.call(ed.children, line), col: rg.toString().length };
  }

  function postPointer(phase, ed, x, y) {
    var pos = hitPos(ed, x, y);
    if (!pos) return;
    if (drag && phase === 'drag' && pos.line === drag.line && pos.col === drag.col) return;
    if (drag) { drag.line = pos.line; drag.col = pos.col; }
    post({ kind: 'pointer', phase: phase, key: ed.dataset.key, line: pos.line, col: pos.col });
  }

  // A dialog button: its action by name, nothing else (no key, no editor
  // knowledge — the composer named what the answer means).
  root.addEventListener('click', function (e) {
    var b = e.target && e.target.closest ? e.target.closest('.cf-btn') : null;
    if (!b || !b.dataset.action) return;
    e.preventDefault();
    post({ kind: 'action', action: b.dataset.action });
    kb.focus();
  });

  // A press OUTSIDE a popup that names a dismissal fires that action (the
  // quick input's cancel) instead of reaching what is underneath.
  function dismissTarget(target) {
    var pops = document.querySelectorAll('.node.popup[data-dismiss]');
    for (var i = 0; i < pops.length; i++)
      if (!pops[i].contains(target)) return pops[i].dataset.dismiss;
    return null;
  }

  root.addEventListener('mousedown', function (e) {
    if (e.button !== 0) return;
    var dismiss = dismissTarget(e.target);
    if (dismiss) {
      e.preventDefault();
      post({ kind: 'action', action: dismiss });
      kb.focus();
      return;
    }
    var ed = editOf(e.target);
    if (!ed) return;
    var r = ed.getBoundingClientRect();
    // The scrollbar is the browser's: a press on it scrolls, nothing more.
    if (e.clientX - r.left >= ed.clientWidth || e.clientY - r.top >= ed.clientHeight) return;
    e.preventDefault();               // no native selection: the engine draws its own
    drag = { el: ed, line: -1, col: -1 };
    postPointer('down', ed, e.clientX, e.clientY);
    kb.focus();
  });
  document.addEventListener('mousemove', function (e) {
    if (!drag) return;
    if (!(e.buttons & 1)) { drag = null; return; }   // the release happened elsewhere
    var ed = drag.el, r = ed.getBoundingClientRect();
    // Past the top or bottom edge the view creeps a line per move, so a
    // drag can select beyond the fold.
    if (e.clientY < r.top) ed.scrollTop -= lineHeight(ed);
    else if (e.clientY >= r.top + ed.clientHeight) ed.scrollTop += lineHeight(ed);
    postPointer('drag', ed, e.clientX, e.clientY);
  });
  document.addEventListener('mouseup', function (e) {
    if (!drag || e.button !== 0) return;
    var ed = drag.el;
    drag = null;
    postPointer('up', ed, e.clientX, e.clientY);
  });

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
