// SPDX-License-Identifier: AGPL-3.0-or-later
import { Component } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import styles from './ErrorBoundary.module.css';

interface State {
  error: Error | null;
}

/**
 * Keeps one broken page from blanking the whole editor: audio keeps running in the plugin, so
 * the rest of the UI (presets, meters, other sections) must stay usable.
 */
export class ErrorBoundary extends Component<{ children: ReactNode; resetKey: string }, State> {
  override state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  override componentDidUpdate(previous: { resetKey: string }) {
    if (previous.resetKey !== this.props.resetKey && this.state.error)
      this.setState({ error: null });
  }

  override componentDidCatch(error: Error, info: ErrorInfo) {
    console.warn('Sedef UI: a page failed to render', error, info.componentStack);
  }

  override render() {
    if (!this.state.error) return this.props.children;
    return (
      <div className={styles.fallback} role="alert">
        <p>This page could not be drawn. Your sound is not affected.</p>
        <button type="button" onClick={() => this.setState({ error: null })}>
          Try again
        </button>
      </div>
    );
  }
}
