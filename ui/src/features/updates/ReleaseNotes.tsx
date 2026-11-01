// SPDX-License-Identifier: AGPL-3.0-or-later
// Renders release notes (untrusted Markdown) from markdownParse.ts's data as React elements, so
// nothing in them can become HTML. Links open in the system browser through the plugin
// (onOpenLink), never inside the WebView.
import { Fragment, useMemo } from 'react';
import type { MouseEvent, ReactNode } from 'react';
import { parseMarkdown, type Block, type Inline } from './markdownParse';
import styles from './ReleaseNotes.module.css';

function renderInline(nodes: Inline[], open: (url: string) => void): ReactNode[] {
  return nodes.map((node, i) => {
    switch (node.kind) {
      case 'text':
        return <Fragment key={i}>{node.text}</Fragment>;
      case 'code':
        return <code key={i}>{node.text}</code>;
      case 'strong':
        return <strong key={i}>{renderInline(node.children, open)}</strong>;
      case 'em':
        return <em key={i}>{renderInline(node.children, open)}</em>;
      case 'link': {
        const onClick = (e: MouseEvent) => {
          e.preventDefault();
          open(node.href);
        };
        return (
          <a
            key={i}
            href={node.href}
            title={node.href}
            onClick={onClick}
            onAuxClick={(e) => e.preventDefault()}
          >
            {renderInline(node.children, open)}
          </a>
        );
      }
    }
  });
}

function renderBlock(block: Block, i: number, open: (url: string) => void): ReactNode {
  switch (block.kind) {
    case 'heading': {
      // The panel's title is the h2; the notes' headings sit below it.
      const Tag = block.level <= 2 ? 'h3' : 'h4';
      return <Tag key={i}>{renderInline(block.children, open)}</Tag>;
    }
    case 'paragraph':
      return <p key={i}>{renderInline(block.children, open)}</p>;
    case 'list': {
      const Tag = block.ordered ? 'ol' : 'ul';
      return (
        <Tag key={i}>
          {block.items.map((item, j) => (
            <li key={j}>{renderInline(item, open)}</li>
          ))}
        </Tag>
      );
    }
    case 'quote':
      return (
        <blockquote key={i}>
          <p>{renderInline(block.children, open)}</p>
        </blockquote>
      );
    case 'code':
      return (
        <pre key={i}>
          <code>{block.text}</code>
        </pre>
      );
    case 'table':
      return (
        <div key={i} className={styles.tableWrap}>
          <table>
            <thead>
              <tr>
                {block.head.map((cell, j) => (
                  <th key={j} scope="col">
                    {renderInline(cell, open)}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {block.rows.map((row, r) => (
                <tr key={r}>
                  {row.map((cell, j) => (
                    <td key={j}>{renderInline(cell, open)}</td>
                  ))}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      );
    case 'rule':
      return <hr key={i} />;
  }
}

export interface ReleaseNotesProps {
  source: string;
  /** Called with a vetted https URL when a link is activated. */
  onOpenLink: (url: string) => void;
  className?: string;
}

export function ReleaseNotes({ source, onOpenLink, className }: ReleaseNotesProps) {
  const blocks = useMemo(() => parseMarkdown(source), [source]);
  return (
    <div className={[styles.notes, className].filter(Boolean).join(' ')}>
      {blocks.map((block, i) => renderBlock(block, i, onOpenLink))}
    </div>
  );
}
