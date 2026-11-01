// SPDX-License-Identifier: AGPL-3.0-or-later
import { Component } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import styles from './ErrorBoundary.module.css';

interface State {
  error: Error | null;
}

interface Props {
  children: ReactNode;
  /** A new key clears the error and draws the children again. */
  resetKey: string;
  /** Classes for the fallback (an overlay positions it over what it replaces). */
  className?: string;
  /** When given, the fallback offers Close, which runs this, instead of Try again. */
  onClose?: () => void;
}

/**
 * Keeps one broken page from blanking the whole editor: audio keeps running in the plugin, so
 * the rest of the UI (presets, meters, other sections) must stay usable.
 */
export class ErrorBoundary extends Component<Props, State> {
  override state: State = { error: null };

  static getDerivedStateFromError(error: Error): State {
    return { error };
  }

  override componentDidUpdate(previous: Props) {
    if (previous.resetKey !== this.props.resetKey && this.state.error)
      this.setState({ error: null });
  }

  override componentDidCatch(error: Error, info: ErrorInfo) {
    console.warn('Sedef UI: a page failed to render', error, info.componentStack);
  }

  override render() {
    if (!this.state.error) return this.props.children;
    const { className, onClose } = this.props;
    return (
      <div className={`${styles.fallback} ${className ?? ''}`} role="alert">
        <p>This page could not be drawn. Your sound is not affected.</p>
        {onClose ? (
          <button type="button" onClick={onClose}>
            Close
          </button>
        ) : (
          <button type="button" onClick={() => this.setState({ error: null })}>
            Try again
          </button>
        )}
      </div>
    );
  }
}
