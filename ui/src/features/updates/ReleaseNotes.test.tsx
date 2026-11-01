// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { ReleaseNotes } from './ReleaseNotes';

// Shaped like the v0.1.0 release notes: headings, a wrapped list, a table and a relative link.
const V010 = `The first public build: a complete guitar rig.

### Added

- **Pedals:** noise gate, Tide (compressor with tilt EQ and parallel blend), Pearl (transparent
  overdrive with mid boost).
- \`sedef-render\`, an offline renderer for presets.

## Install

Paths are in [docs/install.md](https://github.com/mr-kaynak/sedef-amp/blob/main/docs/install.md).

| Zip | Contains |
|---|---|
| \`SedefAmp-*-macOS.zip\` | VST3, AU, CLAP, standalone app |
| \`SedefAmp-*-Windows.zip\` | VST3, CLAP, standalone app (x64) |
`;

describe('ReleaseNotes', () => {
  it('renders headings, lists, code, tables and links as elements', () => {
    const { container } = render(<ReleaseNotes source={V010} onOpenLink={() => undefined} />);
    expect(screen.getByRole('heading', { level: 4, name: 'Added' })).toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 3, name: 'Install' })).toBeInTheDocument();
    expect(screen.getAllByRole('listitem')).toHaveLength(2);
    expect(screen.getByText('Pedals:').tagName).toBe('STRONG');
    expect(screen.getByText('sedef-render').tagName).toBe('CODE');
    expect(screen.getAllByRole('row')).toHaveLength(3);
    expect(screen.getByRole('columnheader', { name: 'Contains' })).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'docs/install.md' })).toHaveAttribute(
      'href',
      'https://github.com/mr-kaynak/sedef-amp/blob/main/docs/install.md',
    );
    expect(container.textContent).toContain('overdrive with mid boost).');
  });

  it('opens links through the plugin instead of navigating', () => {
    const open = vi.fn();
    render(<ReleaseNotes source="[Release](https://github.com/x/y/releases)" onOpenLink={open} />);
    const link = screen.getByRole('link', { name: 'Release' });
    const click = new MouseEvent('click', { bubbles: true, cancelable: true });
    fireEvent(link, click);
    expect(click.defaultPrevented).toBe(true);
    expect(open).toHaveBeenCalledWith('https://github.com/x/y/releases');
  });

  it('never renders HTML or active script links from the notes', () => {
    const open = vi.fn();
    const { container } = render(
      <ReleaseNotes
        onOpenLink={open}
        source={[
          '<script>window.__pwned = true</script>',
          '<img src=x onerror="window.__pwned = true">',
          '<a href="javascript:window.__pwned=true">raw anchor</a>',
          '[click me](javascript:window.__pwned=true)',
          '[or me](JAVASCRIPT:alert(1)) [data](data:text/html,<b>x</b>) [plain](http://a.example)',
          '<iframe src="data:text/html,x"></iframe>',
        ].join('\n\n')}
      />,
    );
    expect(container.querySelector('script, img, iframe, [onerror]')).toBeNull();
    expect(container.querySelectorAll('a')).toHaveLength(0);
    expect(screen.queryByRole('link')).toBeNull();
    expect(container.textContent).toContain('<script>window.__pwned = true</script>');
    expect(container.textContent).toContain('click me');
    // Markdown link targets that are not https are dropped, not shown.
    expect(container.textContent).not.toMatch(/\((javascript|data|http):/i);
    fireEvent.click(screen.getByText(/click me/));
    expect(open).not.toHaveBeenCalled();
    expect((window as { __pwned?: boolean }).__pwned).toBeUndefined();
  });
});
