// SPDX-License-Identifier: AGPL-3.0-or-later
// A dropped file's bytes as they are, for the bridge. Reading a file as text decodes it as UTF-8,
// which drops a byte-order mark and replaces invalid bytes (docs/robustness.md, section 6).

/** The blob's bytes (FileReader where Blob.arrayBuffer is missing, as in older WebViews). */
export async function bytesOf(blob: Blob): Promise<Uint8Array> {
  if (typeof blob.arrayBuffer === 'function') return new Uint8Array(await blob.arrayBuffer());
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error ?? new Error('read failed'));
    reader.readAsArrayBuffer(blob);
  });
}

/** Base64 of `bytes`, in 32 KB steps so no call gets too many arguments. */
export function base64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000)
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(binary);
}
