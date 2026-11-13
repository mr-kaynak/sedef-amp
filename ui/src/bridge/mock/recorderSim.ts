// SPDX-License-Identifier: AGPL-3.0-or-later
// The standalone app's recorder and re-amp for the browser mock (docs/recorder.md): takes that
// record while time passes, files that load (converted when their rate is not 48 kHz), play,
// loop and end, and drops that arrive in pieces. Time moves only with step(), so tests and
// screenshots see exactly what they ask for.
import type {
  ReampListItem,
  ReampState,
  ReampTelemetry,
  RecorderBridgeState,
  RecorderState,
  RecorderTelemetry,
} from '../protocol';

/** The recorder shown in a given state (the mock's ?recorder= flag). */
export type RecorderDemo =
  'ready' | 'recording' | 'saved' | 'dropouts' | 'error' | 'reamp' | 'converting' | 'reamp-empty';

const DEMOS: readonly RecorderDemo[] = [
  'ready',
  'recording',
  'saved',
  'dropouts',
  'error',
  'reamp',
  'converting',
  'reamp-empty',
];

export function isRecorderDemo(value: unknown): value is RecorderDemo {
  return typeof value === 'string' && (DEMOS as readonly string[]).includes(value);
}

/** Whether a demo opens the panel on its Re-amp tab. */
export function demoTab(demo: RecorderDemo): 'record' | 'reamp' {
  return demo.startsWith('reamp') || demo === 'converting' ? 'reamp' : 'record';
}

const FOLDER = '/Users/you/Music/Sedef Amp/Recordings';
const RATE = 48000;

interface MockFile {
  name: string;
  path: string;
  seconds: number;
  sampleRate: number;
  channels: number;
  kind: 'take' | 'imported';
}

function takeName(index: number): string {
  // Fixed times on one day, newest first: the mock never reads the clock.
  const minutes = 14 * 60 + 5 + index * 7;
  const hh = String(Math.floor(minutes / 60)).padStart(2, '0');
  const mm = String(minutes % 60).padStart(2, '0');
  return `2026-11-13 ${hh}-${mm}-33`;
}

const SEED_FILES: MockFile[] = [
  {
    name: takeName(1),
    path: `${FOLDER}/${takeName(1)} DI.wav`,
    seconds: 83.4,
    sampleRate: RATE,
    channels: 1,
    kind: 'take',
  },
  {
    name: takeName(0),
    path: `${FOLDER}/${takeName(0)} DI.wav`,
    seconds: 41.2,
    sampleRate: RATE,
    channels: 1,
    kind: 'take',
  },
  {
    name: 'Rhythm DI, 44.1 kHz',
    path: `${FOLDER}/Imported/Rhythm DI, 44.1 kHz.wav`,
    seconds: 128.0,
    sampleRate: 44100,
    channels: 2,
    kind: 'imported',
  },
];

export class RecorderSim {
  private status: RecorderState['status'] = 'idle';
  private seconds = 0;
  private savingLeft = 0;
  private take: RecorderState['take'] = null;
  private error: string | null = null;
  private dropouts = 0;
  private takes = 2;
  private files: MockFile[] = SEED_FILES.map((f) => ({ ...f }));

  private reamp: ReampState = {
    status: 'empty',
    file: null,
    progress: 0,
    loop: true,
    mix: false,
    playing: false,
    error: null,
  };
  private reampAt = 0;
  private pendingFile: MockFile | null = null;
  private importing: { token: number; name: string; size: number; received: number } | null = null;
  private nextToken = 1;
  /** Frozen screens (screenshots): states hold, time does not move. */
  private frozen = false;

  constructor(private readonly onChange: (state: RecorderBridgeState) => void) {}

  demo(which: RecorderDemo): void {
    this.frozen = true;
    switch (which) {
      case 'ready':
        break;
      case 'recording':
        this.status = 'recording';
        this.seconds = 42.3;
        this.take = this.newTake();
        break;
      case 'saved':
      case 'dropouts':
        this.dropouts = which === 'dropouts' ? 2 : 0;
        this.take = {
          ...this.newTake(),
          seconds: 83.4,
          dropouts: this.dropouts,
          droppedSeconds: this.dropouts ? 0.09 : 0,
          ended: 'stopped',
        };
        break;
      case 'error':
        this.error = `Could not write to ${FOLDER}. The disk may be full or gone; the take is kept up to where it stopped.`;
        this.take = { ...this.newTake(), seconds: 12.6, ended: 'writeError' };
        break;
      case 'reamp':
        this.load(this.files[1]!);
        this.finishLoad();
        this.reamp.playing = true;
        this.reampAt = 12.4;
        break;
      case 'converting':
        this.load(this.files[2]!);
        this.reamp.progress = 0.4;
        break;
      case 'reamp-empty':
        this.files = [];
        break;
    }
  }

  state(): RecorderBridgeState {
    return {
      recorder: {
        status: this.status,
        folder: FOLDER,
        take: this.take ? { ...this.take } : null,
        error: this.error,
      },
      reamp: { ...this.reamp, file: this.reamp.file ? { ...this.reamp.file } : null },
    };
  }

  /** What the telemetry carries; `inDb` and `outDb` are the simulated interface's peaks. */
  telemetry(inDb: number, outDb: number): { recorder: RecorderTelemetry; reamp: ReampTelemetry } {
    const active = this.status !== 'idle';
    const length = this.reamp.file ? this.reamp.file.seconds : 0;
    return {
      recorder: {
        status: this.status,
        seconds: active ? this.seconds : 0,
        diDb: active ? inDb : -120,
        outputDb: active ? outDb : -120,
        dropouts: active ? this.dropouts : 0,
      },
      reamp: { playing: this.reamp.playing, seconds: this.reampAt, length, gaps: 0 },
    };
  }

  step(dt: number): void {
    if (this.frozen) return;
    if (this.status === 'starting') {
      // The first block: the take starts now.
      this.status = 'recording';
      this.changed();
    } else if (this.status === 'recording') {
      this.seconds += dt;
      if (this.take) this.take.seconds = this.seconds;
    }
    if (this.status === 'saving') {
      this.savingLeft -= dt;
      if (this.savingLeft <= 0) this.finishTake();
    }
    if (this.reamp.status === 'converting' || this.reamp.status === 'loading') {
      this.reamp.progress = Math.min(1, this.reamp.progress + dt * 2);
      if (this.reamp.progress >= 1) this.finishLoad();
      this.changed();
    }
    if (this.reamp.playing && this.reamp.file) {
      this.reampAt += dt;
      if (this.reampAt >= this.reamp.file.seconds) {
        if (this.reamp.loop) this.reampAt -= this.reamp.file.seconds;
        else {
          this.reampAt = 0;
          this.reamp.playing = false;
          this.changed();
        }
      }
    }
  }

  // Native calls ----------------------------------------------------------------------------

  toggle(): RecorderBridgeState {
    if (this.status === 'idle') {
      this.status = 'starting';
      this.seconds = 0;
      this.dropouts = 0;
      this.error = null;
      this.take = this.newTake();
    } else if (this.status === 'starting' || this.status === 'recording') {
      this.status = 'saving';
      this.savingLeft = 0.05;
    }
    this.frozen = false;
    this.changed();
    return this.state();
  }

  list(): ReampListItem[] {
    return this.files.map(({ name, path, seconds, kind }) => ({ name, path, seconds, kind }));
  }

  loadPath(path: string): RecorderBridgeState {
    const file = this.files.find((f) => f.path === path);
    if (file) this.load(file);
    return this.state();
  }

  open(): RecorderBridgeState {
    // The chooser picks the imported 44.1 kHz file.
    const file = this.files.find((f) => f.kind === 'imported') ?? this.files[0];
    if (file) this.load(file);
    return this.state();
  }

  play(play: boolean): RecorderBridgeState {
    if (this.reamp.status === 'ready') {
      this.reamp.playing = play;
      this.reampAt = 0;
      this.frozen = false;
    }
    this.changed();
    return this.state();
  }

  setLoop(loop: boolean): RecorderBridgeState {
    this.reamp.loop = loop;
    this.changed();
    return this.state();
  }

  setMix(mix: boolean): RecorderBridgeState {
    this.reamp.mix = mix;
    this.changed();
    return this.state();
  }

  importBegin(fileName: string, size: number): number {
    if (!/\.(wav|wave|aif|aiff|flac)$/i.test(fileName)) {
      this.reampError('Drop a WAV, AIFF or FLAC file to re-amp it.');
      return 0;
    }
    if (size <= 0 || size > 2 * 1024 ** 3) {
      this.reampError(
        size <= 0
          ? 'That file is empty.'
          : 'That file is over 2 GB. Use Open file… for files this large.',
      );
      return 0;
    }
    this.importing = { token: this.nextToken++, name: fileName, size, received: 0 };
    this.reamp = { ...this.reamp, status: 'loading', error: null };
    this.changed();
    return this.importing.token;
  }

  importChunk(token: number, base64: string): boolean {
    if (!this.importing || token !== this.importing.token) return false;
    this.importing.received += Math.floor((base64.length * 3) / 4);
    return true;
  }

  importEnd(token: number): boolean {
    if (!this.importing || token !== this.importing.token) return false;
    const { name, size } = this.importing;
    this.importing = null;
    const base = name.replace(/\.[^.]+$/, '');
    const file: MockFile = {
      name: base,
      path: `${FOLDER}/Imported/${name}`,
      seconds: Math.max(1, size / (RATE * 4)),
      sampleRate: RATE,
      channels: 1,
      kind: 'imported',
    };
    this.files = [file, ...this.files.filter((f) => f.path !== file.path)];
    this.load(file);
    return true;
  }

  importCancel(token: number): void {
    if (this.importing?.token === token) this.importing = null;
  }

  // -----------------------------------------------------------------------------------------

  private newTake(): NonNullable<RecorderState['take']> {
    const name = takeName(this.takes);
    return {
      name,
      diPath: `${FOLDER}/${name} DI.wav`,
      outputPath: `${FOLDER}/${name} Output.wav`,
      seconds: 0,
      dropouts: 0,
      droppedSeconds: 0,
      ended: '',
    };
  }

  private finishTake(): void {
    this.status = 'idle';
    if (this.take) {
      this.take = { ...this.take, seconds: this.seconds, ended: 'stopped' };
      this.files = [
        {
          name: this.take.name,
          path: this.take.diPath,
          seconds: this.seconds,
          sampleRate: RATE,
          channels: 1,
          kind: 'take',
        },
        ...this.files,
      ];
      this.takes += 1;
    }
    this.changed();
  }

  private load(file: MockFile): void {
    this.pendingFile = file;
    this.reamp = {
      ...this.reamp,
      status: file.sampleRate === RATE ? 'loading' : 'converting',
      file: { ...this.fileInfo(file) },
      progress: 0,
      playing: false,
      error: null,
    };
    this.reampAt = 0;
    this.changed();
  }

  private finishLoad(): void {
    if (!this.pendingFile) return;
    this.reamp = {
      ...this.reamp,
      status: 'ready',
      file: this.fileInfo(this.pendingFile),
      progress: 1,
    };
    this.pendingFile = null;
  }

  private fileInfo(file: MockFile) {
    return {
      name: `${file.kind === 'take' ? `${file.name} DI` : file.name}.wav`,
      path: file.path,
      seconds: file.seconds,
      sampleRate: file.sampleRate,
      channels: file.channels,
      converted: file.sampleRate !== RATE,
    };
  }

  private reampError(message: string): void {
    this.reamp = { ...this.reamp, status: 'error', error: message, playing: false };
    this.changed();
  }

  private changed(): void {
    this.onChange(this.state());
  }
}
