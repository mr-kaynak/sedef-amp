// SPDX-License-Identifier: AGPL-3.0-or-later
// The UI is designed on a fixed 1200x760 canvas and scaled uniformly to the window. The plugin
// resizes its window to 1200x760 times the chosen scale; in a browser the canvas simply fits.
import { useLayoutEffect, useState } from 'react';
import type { ReactNode } from 'react';
import styles from './Stage.module.css';

export const CANVAS_WIDTH = 1200;
export const CANVAS_HEIGHT = 760;

function fitScale(): number {
  if (typeof window === 'undefined') return 1;
  const s = Math.min(window.innerWidth / CANVAS_WIDTH, window.innerHeight / CANVAS_HEIGHT);
  return s > 0 && Number.isFinite(s) ? s : 1;
}

export function Stage({ children }: { children: ReactNode }) {
  const [scale, setScale] = useState(fitScale);

  useLayoutEffect(() => {
    const onResize = () => setScale(fitScale());
    window.addEventListener('resize', onResize);
    onResize();
    return () => window.removeEventListener('resize', onResize);
  }, []);

  return (
    <div className={styles.viewport}>
      <div
        className={styles.canvas}
        data-canvas
        style={{
          width: CANVAS_WIDTH,
          height: CANVAS_HEIGHT,
          transform: `scale(${scale})`,
        }}
      >
        {children}
      </div>
    </div>
  );
}
