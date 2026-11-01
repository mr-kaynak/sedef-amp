// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { parseInline, parseMarkdown, safeHref, type Inline } from './markdownParse';

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
