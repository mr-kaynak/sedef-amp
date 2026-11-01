// SPDX-License-Identifier: AGPL-3.0-or-later
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import type { ReactNode } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ErrorBoundary } from './ErrorBoundary';

describe('ErrorBoundary', () => {
  afterEach(cleanup);

  function Broken({ broken }: { broken: boolean }): ReactNode {
    if (broken) throw new Error('boom');
    return <p>Drawn</p>;
  }

  it('offers Close instead of Try again when given one, and draws afresh on a new key', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const error = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    const close = vi.fn();
    const { rerender } = render(
      <ErrorBoundary resetKey="open" onClose={close}>
        <Broken broken />
      </ErrorBoundary>,
    );
    expect(screen.getByRole('alert')).toHaveTextContent('could not be drawn');
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(close).toHaveBeenCalledOnce();

    rerender(
      <ErrorBoundary resetKey="closed" onClose={close}>
        <Broken broken={false} />
      </ErrorBoundary>,
    );
    expect(screen.getByText('Drawn')).toBeInTheDocument();
    warn.mockRestore();
    error.mockRestore();
  });
});
