// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { parseInline, parseMarkdown, safeHref, type Block, type Inline } from './markdownParse';

const text = (t: string): Inline => ({ kind: 'text', text: t });

describe('safeHref', () => {
  it('accepts absolute https URLs only', () => {
    expect(safeHref('https://github.com/mr-kaynak/sedef-amp')).toBe(
      'https://github.com/mr-kaynak/sedef-amp',
    );
    expect(safeHref('  HTTPS://example.com/a?b=c#d ')).toBe('https://example.com/a?b=c#d');
    for (const bad of [
      'javascript:alert(1)',
      'JavaScript:alert(1)',
      ' javascript:alert(1)',
      'java\nscript:alert(1)',
      'data:text/html,<script>alert(1)</script>',
      'vbscript:msgbox(1)',
      'file:///etc/passwd',
      'http://example.com',
      '//example.com/x',
      '/relative',
      '#anchor',
      'https://',
    ])
      expect(safeHref(bad), bad).toBeNull();
  });
});

describe('parseInline', () => {
  it('reads bold, italics, code and links', () => {
    expect(parseInline('a **b** _c_ `d` [e](https://x.org)')).toEqual([
      text('a '),
      { kind: 'strong', children: [text('b')] },
      text(' '),
      { kind: 'em', children: [text('c')] },
      text(' '),
      { kind: 'code', text: 'd' },
      text(' '),
      { kind: 'link', href: 'https://x.org/', children: [text('e')] },
    ]);
  });

  it('keeps the label of an unsafe link as text and drops the target', () => {
    expect(parseInline('[click](javascript:alert(1))')).toEqual([text('click'), text(')')]);
    expect(parseInline('[a](data:text/html,x)')).toEqual([text('a')]);
    expect(parseInline('[a](http://plain.example)')).toEqual([text('a')]);
  });

  it('links bare https URLs without trailing punctuation', () => {
    expect(parseInline('See https://github.com/x/y/compare/v1...v2.')).toEqual([
      text('See '),
      {
        kind: 'link',
        href: 'https://github.com/x/y/compare/v1...v2',
        children: [text('https://github.com/x/y/compare/v1...v2')],
      },
      text('.'),
    ]);
    expect(parseInline('<https://a.example/b>')).toEqual([
      { kind: 'link', href: 'https://a.example/b', children: [text('https://a.example/b')] },
    ]);
    expect(parseInline('http://not-linked.example')).toEqual([text('http://not-linked.example')]);
  });

  it('leaves HTML, snake_case and stray markers alone', () => {
    expect(parseInline('<script>alert(1)</script>')).toEqual([text('<script>alert(1)</script>')]);
    expect(parseInline('a_b_c and 2 * 3 * 4')).toEqual([text('a_b_c and 2 * 3 * 4')]);
    expect(parseInline('\\*not em\\*')).toEqual([text('*not em*')]);
    expect(parseInline('`unclosed')).toEqual([text('`unclosed')]);
  });

  it('does not nest links in link labels', () => {
    expect(parseInline('[see https://a.example](https://b.example)')).toEqual([
      { kind: 'link', href: 'https://b.example/', children: [text('see https://a.example')] },
    ]);
  });
});

describe('parseMarkdown', () => {
  it('reads headings, lists, quotes, code, rules and paragraphs', () => {
    const blocks = parseMarkdown(
      [
        '# Title',
        'First line',
        'continues.',
        '',
        '- one',
        '- two',
        '  wrapped',
        '',
        '- three',
        '1. first',
        '2. second',
        '> quoted',
        '---',
        '```',
        '<b>raw</b>',
        '```',
        '<!-- hidden -->',
      ].join('\r\n'),
    );
    expect(blocks.map((b) => b.kind)).toEqual([
      'heading',
      'paragraph',
      'list',
      'list',
      'quote',
      'rule',
      'code',
    ]);
    expect(blocks[1]).toEqual({ kind: 'paragraph', children: [text('First line continues.')] });
    expect(blocks[2]).toEqual({
      kind: 'list',
      ordered: false,
      items: [[text('one')], [text('two wrapped')], [text('three')]],
    });
    expect(blocks[3]).toMatchObject({ kind: 'list', ordered: true });
    expect(blocks[6]).toEqual({ kind: 'code', text: '<b>raw</b>' });
  });

  it('reads simple tables', () => {
    const [table] = parseMarkdown('| Zip | Contains |\n|---|---|\n| `a.zip` | VST3, CLAP |\n');
    expect(table).toEqual({
      kind: 'table',
      head: [[text('Zip')], [text('Contains')]],
      rows: [[[{ kind: 'code', text: 'a.zip' }], [text('VST3, CLAP')]]],
    });
  });

  it('shows a pathologically long paragraph as text', () => {
    const source = '*a '.repeat(5000);
    expect(parseMarkdown(source)).toEqual([{ kind: 'paragraph', children: [text(source.trim())] }]);
  });
});

describe('parseMarkdown on hostile input', () => {
  // Each of these took seconds when blocks were matched with backtracking patterns, comments
  // stripped with a lazy one, or blank lines rescanned what followed them. Now each takes a few
  // milliseconds; the bound is loose for busy CI machines.
  const quickly = (source: string): Block[] => {
    const started = performance.now();
    const blocks = parseMarkdown(source);
    expect(performance.now() - started).toBeLessThan(1000);
    return blocks;
  };
  const plain = (line: string): Block[] => [{ kind: 'paragraph', children: [text(line.trim())] }];

  it('shows a line over 1000 characters as plain text', () => {
    const line = `# **Heading** ${'x'.repeat(1000)}`;
    expect(quickly(line)).toEqual(plain(line));
  });

  it('keeps table rules linear', () => {
    const rule = `|-${' '.repeat(990)}x`;
    expect(quickly(`a | b\n${rule}`)).toEqual([
      { kind: 'paragraph', children: [text(`a | b ${rule}`)] },
    ]);
    expect(quickly(`a | b\n|${'-|'.repeat(490)}`)[0]?.kind).toBe('table');
    const long = `|-${' '.repeat(60000)}x`;
    expect(quickly(`a | b\n${long}`)).toEqual([
      { kind: 'paragraph', children: [text('a | b')] },
      ...plain(long),
    ]);
  });

  it('keeps headings linear', () => {
    expect(quickly(`#${' '.repeat(990)}x`)).toEqual([
      { kind: 'heading', level: 1, children: [text('x')] },
    ]);
    expect(quickly(`# a${' #'.repeat(490)}x`)[0]?.kind).toBe('heading');
    expect(quickly(`${'#'.repeat(7)}${' '.repeat(990)}x`)[0]?.kind).toBe('paragraph');
  });

  it('keeps rules linear', () => {
    expect(quickly(`-${' -'.repeat(490)}`)).toEqual([{ kind: 'rule' }]);
    expect(quickly(`-${' -'.repeat(490)}x`)[0]?.kind).toBe('list');
  });

  it('skips a run of blank lines once', () => {
    expect(quickly(`- a${'\n'.repeat(60000)}- b`)).toEqual([
      { kind: 'list', ordered: false, items: [[text('a')], [text('b')]] },
    ]);
    expect(quickly('\n \n'.repeat(30000))).toEqual([]);
  });

  it('strips comments in one pass, unclosed ones too', () => {
    expect(quickly('a<!-- hidden -->b')).toEqual(plain('ab'));
    expect(quickly(`a\n${'<!--\n'.repeat(64000)}`)).toEqual([
      { kind: 'paragraph', children: [text(`a ${'<!-- '.repeat(64000).trim()}`)] },
    ]);
  });
});
