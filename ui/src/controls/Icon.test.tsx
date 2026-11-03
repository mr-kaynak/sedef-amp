// SPDX-License-Identifier: AGPL-3.0-or-later
import { render } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { Icon, ICON_STROKE, ICON_STROKE_LABEL } from './Icon';
import { IconSwap } from './IconSwap';

/** The line width the icon paints, in CSS px: the viewBox stroke scaled from 16 units to size. */
const renderedStroke = (svg: SVGSVGElement) =>
  (Number(svg.getAttribute('stroke-width')) * Number(svg.getAttribute('width'))) / 16;

const svgOf = (ui: React.ReactElement) => render(ui).container.querySelector('svg')!;

describe('Icon', () => {
  it('draws the same 1.5px line at every size in use', () => {
    for (const size of [10, 12, 13, 14, 15, 16, 18]) {
      const svg = svgOf(<Icon name="close" size={size} />);
      expect(renderedStroke(svg)).toBeCloseTo(ICON_STROKE, 6);
    }
    expect(ICON_STROKE).toBe(1.5);
  });

  it('converts the rendered stroke into the 16-unit viewBox', () => {
    expect(svgOf(<Icon name="plus" />).getAttribute('stroke-width')).toBe('1.5');
    expect(svgOf(<Icon name="plus" size={12} />).getAttribute('stroke-width')).toBe('2');
    expect(svgOf(<Icon name="plus" size={10} />).getAttribute('stroke-width')).toBe('2.4');
    expect(Number(svgOf(<Icon name="plus" size={18} />).getAttribute('stroke-width'))).toBeCloseTo(
      4 / 3,
      6,
    );
  });

  it('takes the heavier label stroke beside medium and semibold text', () => {
    expect(ICON_STROKE_LABEL).toBe(2);
    const svg = svgOf(<Icon name="refresh" size={14} stroke={ICON_STROKE_LABEL} />);
    expect(renderedStroke(svg)).toBeCloseTo(2, 6);
    expect(svg).toHaveAttribute('stroke', 'currentColor');
    // Scaled by the viewBox, never non-scaling: the Stage transform must scale the line too.
    expect(svg.querySelector('[vector-effect]')).toBeNull();
  });
});

describe('IconSwap', () => {
  it('keeps both icons mounted and shows one, so the change can cross-fade', () => {
    const { container, rerender } = render(
      <IconSwap active={false} icon="copy" activeIcon="check" />,
    );
    const swap = container.firstElementChild!;
    expect(swap).toHaveAttribute('aria-hidden', 'true');
    const [idle, done] = [...swap.children];
    expect(swap.querySelectorAll('svg')).toHaveLength(2);
    expect(idle).toHaveAttribute('data-shown');
    expect(done).not.toHaveAttribute('data-shown');

    rerender(<IconSwap active icon="copy" activeIcon="check" />);
    expect(idle).not.toHaveAttribute('data-shown');
    expect(done).toHaveAttribute('data-shown');
    expect(swap.querySelectorAll('svg')).toHaveLength(2);
  });
});
