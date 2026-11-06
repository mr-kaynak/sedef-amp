// SPDX-License-Identifier: AGPL-3.0-or-later
// The Updates panel: a modal sheet over the canvas with the versions, the automatic-check switch,
// the release notes and the update itself: download, install through the OS's installer, then a
// restart. Opened from the header pill or from Settings.
import { useEffect, useId, useRef } from 'react';
import type { CSSProperties, KeyboardEvent } from 'react';
import buttons from '../../controls/Button.module.css';
import nacre from '../../controls/NacreFill.module.css';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { ReleaseNotes } from './ReleaseNotes';
import {
  AFTER_DOWNLOAD,
  beforeInstall,
  errorMessage,
  formatBytes,
  formatDate,
  formatSince,
  installManuallyNote,
  installStep,
  manualOnly,
  restartText,
} from './text';
import {
  needsRestart,
  selectSupported,
  updateActions,
  useUpdateStore,
  type SupportedUpdateState,
} from './updateStore';
import styles from './UpdatesPanel.module.css';

const FOCUSABLE = 'a[href], button:not([disabled]), [tabindex="0"]';

/** Where focus goes when the element that opened the panel is gone (e.g. the settings popover). */
function fallbackFocus(): HTMLElement | null {
  return document.querySelector<HTMLElement>('header button[aria-label="Settings"]');
}

function isTransferring(state: SupportedUpdateState): boolean {
  return state.phase === 'downloading' || state.phase === 'verifying';
}

/** A labelled bar: determinate with `value` of `max`, else full and breathing. */
function Bar({
  labelId,
  value,
  max,
  text,
}: {
  labelId: string;
  value?: number;
  max?: number;
  text: string;
}) {
  const determinate = value !== undefined && max !== undefined;
  return (
    <div
      role="progressbar"
      aria-labelledby={labelId}
      aria-valuemin={determinate ? 0 : undefined}
      aria-valuemax={determinate ? max : undefined}
      aria-valuenow={determinate ? value : undefined}
      aria-valuetext={text}
      className={styles.bar}
      data-indeterminate={determinate ? undefined : true}
    >
      <span
        style={
          {
            '--p': `${determinate && max > 0 ? Math.min(100, (value / max) * 100) : 0}%`,
          } as CSSProperties
        }
      />
    </div>
  );
}

function Progress({ state }: { state: SupportedUpdateState }) {
  const labelId = useId();
  const verifying = state.phase === 'verifying';
  const { received = 0, total = 0 } = state.progress ?? {};
  const name = state.latest?.installer?.name ?? 'the installer';
  return (
    <div className={styles.progress}>
      <p id={labelId} className={styles.progressLabel}>
        {verifying ? `Checking ${name}…` : `Downloading ${name}`}
      </p>
      {verifying ? (
        <Bar labelId={labelId} text="Verifying" />
      ) : (
        <Bar
          labelId={labelId}
          value={received}
          max={total}
          text={`${formatBytes(received)} of ${formatBytes(total)}`}
        />
      )}
      <p className={styles.note}>
        {verifying
          ? 'Comparing its SHA-256 with the checksum published with the release.'
          : `${formatBytes(received)} of ${formatBytes(total)}`}
      </p>
    </div>
  );
}

/** The OS's installer runs: which step, and what (if anything) the user has to do. */
function Installing({ state }: { state: SupportedUpdateState }) {
  const labelId = useId();
  const step = installStep(
    state.install?.step ?? 'preparing',
    state.platform,
    state.latest?.version ?? '',
  );
  return (
    <div className={styles.progress}>
      <p id={labelId} className={styles.progressLabel}>
        {step.label}
      </p>
      <Bar labelId={labelId} text={step.label.replace('…', '')} />
      <p className={styles.note}>{step.note}</p>
    </div>
  );
}

/** Installed, or waiting for the host to exit: what's left is a restart. */
function Restart({ state }: { state: SupportedUpdateState }) {
  const text = restartText(state);
  return (
    <div className={styles.done}>
      <p className={styles.headline}>
        <Icon name="check" stroke={ICON_STROKE_LABEL} />
        <span className={styles.file}>{text.headline}</span>
      </p>
      <p className={styles.status}>{text.status}</p>
      <p className={styles.note}>{text.note}</p>
    </div>
  );
}

function Release({ state }: { state: SupportedUpdateState }) {
  const latest = state.latest;
  if (!latest) {
    return (
      <p className={styles.status}>
        {state.phase === 'checking'
          ? 'Checking for updates…'
          : state.lastChecked === null
            ? "Sedef hasn't checked for updates yet."
            : 'No releases are published yet.'}
      </p>
    );
  }
  const date = formatDate(latest.publishedAt);
  if (!state.updateAvailable) {
    return (
      <p className={styles.status}>
        Sedef Amp is up to date.{' '}
        <a
          href={latest.url}
          className={buttons.link}
          onClick={(e) => {
            e.preventDefault();
            void updateActions.openPage(latest.url);
          }}
        >
          What&rsquo;s new in {latest.version}
        </a>
      </p>
    );
  }
  const skipped = state.skippedVersion === latest.version;
  return (
    <>
      <p className={styles.headline}>
        <span className={styles.release}>{latest.name}</span>
        {date && <span className={styles.date}>{date}</span>}
        {skipped && <span className={styles.tag}>Skipped</span>}
      </p>
      <section className={styles.notes} aria-label="Release notes" tabIndex={0}>
        {latest.notes.trim() ? (
          <ReleaseNotes
            source={latest.notes}
            onOpenLink={(url) => void updateActions.openPage(url)}
          />
        ) : (
          <p className={styles.note}>This release has no notes.</p>
        )}
      </section>
    </>
  );
}

function Actions({ state }: { state: SupportedUpdateState }) {
  const latest = state.latest;
  const page = latest?.url ?? state.releasesUrl;
  const openPage = () => void updateActions.openPage(page);
  const close = (
    <button type="button" className={buttons.solid} onClick={updateActions.closePanel}>
      {state.phase === 'installing' ? 'Close' : 'Done'}
    </button>
  );

  if (isTransferring(state)) {
    return (
      <div className={styles.actions}>
        <button type="button" className={buttons.solid} onClick={() => void updateActions.cancel()}>
          Cancel
        </button>
      </div>
    );
  }
  // The OS's installer has its own prompt to cancel in; closing the panel doesn't stop it.
  if (state.phase === 'installing') return <div className={styles.actions}>{close}</div>;
  if (needsRestart(state)) {
    return (
      <div className={styles.actions}>
        {state.canRestart ? (
          <>
            <button type="button" className={buttons.ghost} onClick={updateActions.closePanel}>
              Later
            </button>
            <button
              type="button"
              className={buttons.nacre}
              onClick={() => void updateActions.restart()}
            >
              Restart Sedef
            </button>
          </>
        ) : (
          close
        )}
      </div>
    );
  }
  if (state.phase === 'done' && state.download) {
    return (
      <div className={styles.actions}>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.lead}`}
          onClick={() => void updateActions.reveal()}
        >
          <Icon name="folder" stroke={ICON_STROKE_LABEL} />
          Show in folder
        </button>
        {close}
      </div>
    );
  }

  const failed = state.phase === 'error';
  if (!latest || !state.updateAvailable) {
    return (
      <div className={styles.actions}>
        {failed && (
          <button type="button" className={buttons.ghost} onClick={openPage}>
            Open releases page
          </button>
        )}
        <button type="button" className={buttons.solid} onClick={updateActions.closePanel}>
          Close
        </button>
      </div>
    );
  }

  const skipped = state.skippedVersion === latest.version;
  const canInstall = !!latest.installer && latest.hasChecksums;
  const automatic = state.autoInstall.available;
  const busy = state.phase === 'checking';
  const install = () => void updateActions.install();
  const manually = () => void updateActions.installManually();
  return (
    <>
      <p className={styles.note}>
        {!canInstall
          ? errorMessage(latest.installer ? 'noChecksum' : 'noInstaller', state.platform)
          : !automatic
            ? manualOnly(state.autoInstall.reason, state)
            : failed
              ? installManuallyNote(state.platform)
              : beforeInstall(state)}
      </p>
      <div className={styles.actions}>
        <button type="button" className={buttons.ghost} onClick={updateActions.later}>
          Later
        </button>
        {/* After a failure the row holds the ways forward instead. */}
        {!(failed && canInstall) && (
          <button
            type="button"
            className={buttons.ghost}
            onClick={() =>
              void (skipped ? updateActions.unskip() : updateActions.skip(latest.version))
            }
          >
            {skipped ? 'Offer this version again' : 'Skip this version'}
          </button>
        )}
        <span className={styles.spacer} />
        {(failed || !canInstall) && (
          <button
            type="button"
            className={canInstall ? buttons.ghost : buttons.nacre}
            onClick={openPage}
          >
            Open releases page
          </button>
        )}
        {canInstall && automatic && failed && (
          <button type="button" className={buttons.ghost} disabled={busy} onClick={manually}>
            Install manually
          </button>
        )}
        {canInstall && (
          <button
            type="button"
            className={buttons.nacre}
            disabled={busy}
            onClick={automatic ? install : manually}
          >
            {!automatic ? 'Install manually' : failed ? 'Try again' : 'Update now'}
          </button>
        )}
      </div>
    </>
  );
}

/** One short sentence for screen readers when the phase changes. */
function announcement(state: SupportedUpdateState): string {
  switch (state.phase) {
    case 'checking':
      return 'Checking for updates';
    case 'downloading':
      return 'Downloading the update';
    case 'verifying':
      return 'Verifying the download';
    case 'installing':
      return installStep(
        state.install?.step ?? 'preparing',
        state.platform,
        state.latest?.version ?? '',
      ).label;
    case 'installed':
    case 'scheduled': {
      const text = restartText(state);
      return `${text.headline}. ${text.status}`;
    }
    case 'done':
      return 'Download verified. The installer is open.';
    case 'error':
      return 'Update failed';
    case 'idle':
      return state.updateAvailable && state.latest
        ? `Sedef Amp ${state.latest.version} is available`
        : 'Sedef Amp is up to date';
  }
}

export function UpdatesPanel() {
  const state = useUpdateStore(selectSupported);
  const titleId = useId();
  const dialogRef = useRef<HTMLDivElement>(null);
  const headingRef = useRef<HTMLHeadingElement>(null);
  const phase = state?.phase;

  // Focus moves into the panel and back to where it came from; Escape closes it. The listener
  // sits on window in the capture phase so it runs before the preset browser's (document,
  // capture), which then sees the key handled and stays open.
  useEffect(() => {
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    headingRef.current?.focus({ preventScroll: true });
    const onKeyDown = (e: globalThis.KeyboardEvent) => {
      if (e.key !== 'Escape') return;
      e.preventDefault();
      e.stopPropagation();
      updateActions.closePanel();
    };
    window.addEventListener('keydown', onKeyDown, true);
    return () => {
      window.removeEventListener('keydown', onKeyDown, true);
      const target = opener?.isConnected ? opener : fallbackFocus();
      target?.focus({ preventScroll: true });
    };
  }, []);

  // A phase swaps the buttons: when the focused one is gone, focus stays in the dialog (on its
  // heading) instead of falling out of it.
  useEffect(() => {
    const dialog = dialogRef.current;
    if (dialog && !dialog.contains(document.activeElement))
      headingRef.current?.focus({ preventScroll: true });
  }, [phase]);

  // Tab stays inside, and the app's shortcuts (1–5 switch sections) stay out while it is open.
  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    e.stopPropagation();
    if (e.key !== 'Tab') return;
    const items = [...(dialogRef.current?.querySelectorAll<HTMLElement>(FOCUSABLE) ?? [])];
    const first = items[0];
    const last = items[items.length - 1];
    if (!first || !last) return;
    if (
      e.shiftKey &&
      (document.activeElement === first || !items.includes(document.activeElement as HTMLElement))
    ) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && document.activeElement === last) {
      e.preventDefault();
      first.focus();
    }
  };

  if (!state) return null;
  const busy = state.phase === 'checking' || isTransferring(state) || state.phase === 'installing';

  return (
    <div
      className={styles.scrim}
      onPointerDown={(e) => {
        if (e.target === e.currentTarget) updateActions.closePanel();
      }}
    >
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        className={styles.dialog}
        onKeyDown={onKeyDown}
      >
        <div className={styles.top}>
          <h2 id={titleId} ref={headingRef} tabIndex={-1} className={styles.title}>
            Updates
          </h2>
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.icon}`}
            aria-label="Close"
            title="Close"
            onClick={updateActions.closePanel}
          >
            <Icon name="close" />
          </button>
        </div>

        <div className={styles.summary}>
          <dl className={styles.facts}>
            <div>
              <dt>Running</dt>
              <dd>{state.currentVersion}</dd>
            </div>
            <div>
              <dt>Latest</dt>
              <dd>{state.latest?.version ?? '—'}</dd>
            </div>
            <div>
              <dt>Last checked</dt>
              <dd>{state.phase === 'checking' ? 'Checking…' : formatSince(state.lastChecked)}</dd>
            </div>
          </dl>
          <button
            type="button"
            className={`${buttons.solid} ${buttons.small} ${buttons.lead}`}
            disabled={busy}
            onClick={() => void updateActions.checkNow()}
          >
            <Icon name="refresh" size={14} stroke={ICON_STROKE_LABEL} />
            Check now
          </button>
        </div>

        <div className={styles.auto}>
          <button
            type="button"
            role="switch"
            aria-checked={state.autoCheck && state.autoCheckAllowed}
            disabled={!state.autoCheckAllowed}
            className={styles.switch}
            onClick={() => void updateActions.setAutoCheck(!state.autoCheck)}
          >
            <span className={styles.track} aria-hidden="true">
              <span className={`${nacre.fill} ${styles.thumb}`} />
            </span>
            Check for updates automatically
          </button>
          <p className={styles.note}>
            {state.autoCheckAllowed
              ? 'Once a day, when you open Sedef. The request carries only the Sedef version and your OS.'
              : 'Turned off on this computer (SEDEF_DISABLE_UPDATE_CHECK).'}
          </p>
        </div>

        <div className={styles.body}>
          {state.phase === 'error' && state.error && (
            <p className={styles.error} role="alert">
              {errorMessage(state.error.code, state.platform)}
            </p>
          )}
          {isTransferring(state) ? (
            <Progress state={state} />
          ) : state.phase === 'installing' ? (
            <Installing state={state} />
          ) : needsRestart(state) ? (
            <Restart state={state} />
          ) : state.phase === 'done' && state.download ? (
            <div className={styles.done}>
              <p className={styles.headline}>
                <Icon name="check" stroke={ICON_STROKE_LABEL} />
                <span className={styles.file}>{state.download.fileName}</span>
              </p>
              <p className={styles.status}>Downloaded to your Downloads folder and verified.</p>
              <p className={styles.status}>{AFTER_DOWNLOAD[state.platform]}</p>
            </div>
          ) : (
            <Release state={state} />
          )}
        </div>

        <div className={styles.bottom}>
          <Actions state={state} />
        </div>

        <p className="visually-hidden" role="status">
          {announcement(state)}
        </p>
      </div>
    </div>
  );
}
