// SPDX-License-Identifier: AGPL-3.0-or-later
// A small Markdown parser for release notes, which are untrusted text from the network. It
// reads a subset (headings, paragraphs, lists, quotes, code blocks, simple tables, rules; bold,
// italics, code and links inline) into plain data that Markdown.tsx renders as React elements, so
// nothing from the source ever becomes HTML: everything else, raw HTML included, stays literal
// text. Only absolute https links become links.
export type Inline =
  | { kind: 'text'; text: string }
  | { kind: 'code'; text: string }
  | { kind: 'strong'; children: Inline[] }
  | { kind: 'em'; children: Inline[] }
  | { kind: 'link'; href: string; children: Inline[] };

export type Block =
  | { kind: 'heading'; level: number; children: Inline[] }
  | { kind: 'paragraph'; children: Inline[] }
  | { kind: 'list'; ordered: boolean; items: Inline[][] }
  | { kind: 'quote'; children: Inline[] }
  | { kind: 'code'; text: string }
  | { kind: 'table'; head: Inline[][]; rows: Inline[][][] }
  | { kind: 'rule' };

/** The URL if it is an absolute https URL, else null (javascript:, data:, relative, ...). */
export function safeHref(raw: string): string | null {
  const text = raw.trim();
  if (!/^https:\/\//i.test(text)) return null;
  try {
    const url = new URL(text);
    return url.protocol === 'https:' && url.hostname ? url.href : null;
  } catch {
    return null;
  }
}

// --- Inline -------------------------------------------------------------------------------------

const ESCAPABLE = '\\`*_{}[]()#+-.!<>|~';
// Longer runs are shown as text: emphasis search is quadratic in the worst case.
const MAX_INLINE = 8000;

function isWordChar(c: string): boolean {
  return /[A-Za-z0-9]/.test(c);
}

/** Index of the "]" that closes the "[" at `open`; -1 when there is none. */
function closingBracket(src: string, open: number): number {
  let depth = 0;
  for (let i = open; i < src.length; i += 1) {
    const c = src.charAt(i);
    if (c === '\\') i += 1;
    else if (c === '[') depth += 1;
    else if (c === ']' && --depth === 0) return i;
  }
  return -1;
}

/** Closing marker for single-character emphasis that starts at `open`; -1 when there is none. */
function closingEmphasis(src: string, open: number, marker: string): number {
  for (let end = src.indexOf(marker, open + 1); end !== -1; end = src.indexOf(marker, end + 1)) {
    const before = src.charAt(end - 1);
    const after = src.charAt(end + 1);
    if (before === ' ' || before === marker || after === marker) continue;
    if (marker === '_' && isWordChar(after)) continue; // snake_case is not emphasis
    return end;
  }
  return -1;
}

export function parseInline(src: string, links = true): Inline[] {
  if (src.length > MAX_INLINE) return [{ kind: 'text', text: src }];
  const out: Inline[] = [];
  let text = '';
  const flush = () => {
    if (text) out.push({ kind: 'text', text });
    text = '';
  };
  const push = (...nodes: Inline[]) => {
    flush();
    out.push(...nodes);
  };
  const link = (target: string, children: Inline[]) => {
    const href = safeHref(target);
    // An unsafe target keeps its label as text; the target itself is dropped.
    if (href) push({ kind: 'link', href, children });
    else push(...children);
  };

  let i = 0;
  while (i < src.length) {
    const c = src.charAt(i);
    const next = src.charAt(i + 1);

    if (c === '\\' && next && ESCAPABLE.includes(next)) {
      text += next;
      i += 2;
      continue;
    }

    if (c === '`') {
      let run = 1;
      while (src.charAt(i + run) === '`') run += 1;
      const ticks = '`'.repeat(run);
      const end = src.indexOf(ticks, i + run);
      if (end !== -1) {
        push({ kind: 'code', text: src.slice(i + run, end).replace(/^ (.+) $/, '$1') });
        i = end + run;
      } else {
        text += ticks;
        i += run;
      }
      continue;
    }

    if ((c === '*' || c === '_') && next === c) {
      const end = src.indexOf(c + c, i + 2);
      if (end > i + 2) {
        push({ kind: 'strong', children: parseInline(src.slice(i + 2, end), links) });
        i = end + 2;
        continue;
      }
    }

    if (
      (c === '*' || c === '_') &&
      next &&
      next !== ' ' &&
      next !== c &&
      !(c === '_' && isWordChar(src.charAt(i - 1)))
    ) {
      const end = closingEmphasis(src, i, c);
      if (end !== -1) {
        push({ kind: 'em', children: parseInline(src.slice(i + 1, end), links) });
        i = end + 1;
        continue;
      }
    }

    if (c === '[' && links) {
      const close = closingBracket(src, i);
      const end = close !== -1 && src.charAt(close + 1) === '(' ? src.indexOf(')', close + 2) : -1;
      if (end !== -1) {
        // [label](target "title"): the title is dropped, angle brackets around the target too.
        const target = (
          src
            .slice(close + 2, end)
            .trim()
            .split(/\s+/)[0] ?? ''
        ).replace(/^<(.*)>$/, '$1');
        link(target, parseInline(src.slice(i + 1, close), false));
        i = end + 1;
        continue;
      }
    }

    if (c === '<' && links) {
      const m = /^<(https?:\/\/[^\s<>"]+)>/i.exec(src.slice(i, i + 2048));
      if (m?.[1]) {
        link(m[1], [{ kind: 'text', text: m[1] }]);
        i += m[0].length;
        continue;
      }
    }

    if ((c === 'h' || c === 'H') && links && !isWordChar(src.charAt(i - 1))) {
      const m = /^https:\/\/[^\s<>"]+/i.exec(src.slice(i, i + 2048));
      // Trailing punctuation belongs to the sentence, not the URL.
      const url = m?.[0].replace(/[.,;:!?'")\]*_]+$/, '');
      if (url && safeHref(url)) {
        link(url, [{ kind: 'text', text: url }]);
        i += url.length;
        continue;
      }
    }

    text += c;
    i += 1;
  }
  flush();
  return out;
}

// --- Blocks -------------------------------------------------------------------------------------

// Longer lines are shown as plain text, so no block pattern ever runs on more than this.
const MAX_LINE = 1000;
const FENCE = /^ {0,3}(`{3,}|~{3,})/;
const HEADING = /^ {0,3}(#{1,6})[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$/;
const RULE = /^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$/;
const ITEM = /^[ \t]*([-*+]|\d{1,9}[.)])[ \t]+(.*)$/;
const QUOTE = /^ {0,3}>[ \t]?(.*)$/;
const ALIGNMENT = /^:?-+:?$/;

/** Drops HTML comments (invisible on GitHub too) in one pass; an unclosed one stays as text. */
function stripComments(source: string): string {
  let out = '';
  let from = 0;
  for (;;) {
    const open = source.indexOf('<!--', from);
    const close = open === -1 ? -1 : source.indexOf('-->', open + 4);
    if (close === -1) return out + source.slice(from);
    out += source.slice(from, open);
    from = close + 3;
  }
}

/** Splits a table row on unescaped pipes. */
function cells(line: string): string[] {
  let row = line.trim();
  if (row.startsWith('|')) row = row.slice(1);
  if (row.endsWith('|') && !row.endsWith('\\|')) row = row.slice(0, -1);
  const out: string[] = [];
  let cell = '';
  for (let i = 0; i < row.length; i += 1) {
    const c = row.charAt(i);
    if (c === '\\' && row.charAt(i + 1) === '|') {
      cell += '|';
      i += 1;
    } else if (c === '|') {
      out.push(cell.trim());
      cell = '';
    } else {
      cell += c;
    }
  }
  out.push(cell.trim());
  return out;
}

/** The line under a table's header: "|---|:--:|", cell by cell rather than one pattern. */
function isTableRule(line: string): boolean {
  return line.includes('|') && cells(line).every((cell) => ALIGNMENT.test(cell));
}

export function parseMarkdown(source: string): Block[] {
  const lines = stripComments(source.replace(/\r\n?/g, '\n')).split('\n');
  const blocks: Block[] = [];
  let paragraph: string[] = [];
  let list: { ordered: boolean; items: string[] } | null = null;

  const endParagraph = () => {
    if (paragraph.length)
      blocks.push({ kind: 'paragraph', children: parseInline(paragraph.join(' ')) });
    paragraph = [];
  };
  const endList = () => {
    if (list)
      blocks.push({
        kind: 'list',
        ordered: list.ordered,
        items: list.items.map((item) => parseInline(item)),
      });
    list = null;
  };
  const line = (i: number) => lines[i] ?? '';

  for (let i = 0; i < lines.length; i += 1) {
    const current = line(i);

    const fence = FENCE.exec(current);
    if (fence?.[1]) {
      endParagraph();
      endList();
      const marker = fence[1];
      const body: string[] = [];
      for (i += 1; i < lines.length && !line(i).trimStart().startsWith(marker); i += 1)
        body.push(line(i));
      blocks.push({ kind: 'code', text: body.join('\n') });
      continue;
    }

    if (!current.trim()) {
      endParagraph();
      // A blank line between two items keeps the list going. The run is skipped as a whole.
      let n = i + 1;
      while (n < lines.length && !line(n).trim()) n += 1;
      if (line(n).length > MAX_LINE || !ITEM.test(line(n))) endList();
      i = n - 1;
      continue;
    }

    if (current.length > MAX_LINE) {
      endParagraph();
      endList();
      blocks.push({ kind: 'paragraph', children: [{ kind: 'text', text: current.trim() }] });
      continue;
    }

    const heading = HEADING.exec(current);
    if (heading?.[1]) {
      endParagraph();
      endList();
      blocks.push({
        kind: 'heading',
        level: heading[1].length,
        children: parseInline(heading[2] ?? ''),
      });
      continue;
    }

    if (RULE.test(current)) {
      endParagraph();
      endList();
      blocks.push({ kind: 'rule' });
      continue;
    }

    const next = line(i + 1);
    if (current.includes('|') && next.length <= MAX_LINE && isTableRule(next)) {
      endParagraph();
      endList();
      const head = cells(current).map((c) => parseInline(c));
      const rows: Inline[][][] = [];
      const isRow = (row: string) => row.includes('|') && row.trim() && row.length <= MAX_LINE;
      for (i += 2; i < lines.length && isRow(line(i)); i += 1)
        rows.push(cells(line(i)).map((c) => parseInline(c)));
      i -= 1;
      blocks.push({ kind: 'table', head, rows });
      continue;
    }

    const item = ITEM.exec(current);
    if (item?.[1]) {
      endParagraph();
      const ordered = /\d/.test(item[1]);
      if (list && list.ordered !== ordered) endList();
      list ??= { ordered, items: [] };
      list.items.push(item[2] ?? '');
      continue;
    }

    const quote = QUOTE.exec(current);
    if (quote) {
      endParagraph();
      endList();
      const quoted = [quote[1] ?? ''];
      while (i + 1 < lines.length && line(i + 1).length <= MAX_LINE && QUOTE.test(line(i + 1)))
        quoted.push(QUOTE.exec(line((i += 1)))?.[1] ?? '');
      blocks.push({ kind: 'quote', children: parseInline(quoted.join(' ').trim()) });
      continue;
    }

    // An indented line continues the list item above it.
    if (list && /^[ \t]/.test(current)) {
      const { items } = list;
      items[items.length - 1] = `${items[items.length - 1] ?? ''} ${current.trim()}`;
      continue;
    }

    endList();
    paragraph.push(current.trim());
  }
  endParagraph();
  endList();
  return blocks;
}
