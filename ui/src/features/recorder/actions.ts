// SPDX-License-Identifier: AGPL-3.0-or-later
// The recorder and re-amp's calls into the plug-in (docs/recorder.md).
import type { RecorderBridgeState } from '../../bridge/protocol';
import { invoke } from '../../state/backendRef';
import { applyRecorderState, recorderUi, useRecorderStore } from './store';

/** Bytes per piece of a dropped file (1 MB, sent as base64). */
export const IMPORT_CHUNK = 1 << 20;
/** The plug-in keeps dropped files up to 2 GB; bigger ones go through Open file…. */
export const IMPORT_MAX = 2 * 1024 ** 3;
const AUDIO_FILE = /\.(wav|wave|aif|aiff|flac)$/i;

function apply(state: RecorderBridgeState | null | undefined): void {
  if (state) applyRecorderState(state);
}

/** Starts a take, or stops the one recording (the R key, the big button). */
export async function toggleRecording(): Promise<void> {
  apply(await invoke('sedef_recorder_toggle'));
}

/** Shows a file of a take in the file manager; without one, the folder. */
export function reveal(path?: string): void {
  void invoke('sedef_recorder_reveal', path);
}

export async function refreshFiles(): Promise<void> {
  const files = await invoke('sedef_reamp_files');
  useRecorderStore.setState({ files: Array.isArray(files) ? files : [] });
}

export async function loadFile(path: string): Promise<void> {
  useRecorderStore.setState({ dropError: null });
  apply(await invoke('sedef_reamp_load', path));
}

export async function openFile(): Promise<void> {
  useRecorderStore.setState({ dropError: null });
  apply(await invoke('sedef_reamp_open'));
  void refreshFiles();
}

export async function playReamp(play: boolean): Promise<void> {
  apply(await invoke('sedef_reamp_play', play));
}

export async function setReampLoop(loop: boolean): Promise<void> {
  apply(await invoke('sedef_reamp_setLoop', loop));
}

export async function setReampMix(mix: boolean): Promise<void> {
  apply(await invoke('sedef_reamp_setMix', mix));
}

/** A take just recorded, played through the rig: the Re-amp tab with its DI loaded. */
export async function reampTake(diPath: string): Promise<void> {
  recorderUi.setTab('reamp');
  await loadFile(diPath);
  void refreshFiles();
}

async function bytesOf(blob: Blob): Promise<Uint8Array> {
  if (typeof blob.arrayBuffer === 'function') return new Uint8Array(await blob.arrayBuffer());
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error ?? new Error('read failed'));
    reader.readAsArrayBuffer(blob);
  });
}

function base64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000)
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(binary);
}

/**
 * A file dropped on the Re-amp tab: the page has its bytes but no path, so it sends them in
 * pieces and the plug-in keeps the copy under Recordings/Imported, then loads it.
 */
export async function importFile(file: File): Promise<void> {
  const fail = (message: string) =>
    useRecorderStore.setState({ dropError: message, importing: null });
  if (!AUDIO_FILE.test(file.name)) return fail('Drop a WAV, AIFF or FLAC file to re-amp it.');
  if (file.size === 0) return fail('That file is empty.');
  if (file.size > IMPORT_MAX)
    return fail('That file is over 2 GB. Use Open file… for files this large.');

  useRecorderStore.setState({ dropError: null, importing: { name: file.name, progress: 0 } });
  const token = await invoke('sedef_reamp_importBegin', file.name, file.size);
  if (!token) return useRecorderStore.setState({ importing: null }); // the plug-in says why
  try {
    for (let at = 0; at < file.size; at += IMPORT_CHUNK) {
      const piece = await bytesOf(file.slice(at, Math.min(file.size, at + IMPORT_CHUNK)));
      if (!(await invoke('sedef_reamp_importChunk', token, base64(piece))))
        throw new Error('refused');
      useRecorderStore.setState({
        importing: { name: file.name, progress: Math.min(1, (at + piece.length) / file.size) },
      });
    }
    if (!(await invoke('sedef_reamp_importEnd', token))) throw new Error('refused');
    useRecorderStore.setState({ importing: null });
    void refreshFiles();
  } catch {
    void invoke('sedef_reamp_importCancel', token);
    fail('The dropped file did not arrive whole. Try again, or use Open file….');
  }
}
