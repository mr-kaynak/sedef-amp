# ADR-0003: The UI is a web app (React + TypeScript) inside JUCE's WebView

Status: Accepted

## Context

A plugin UI must live inside the host's window, which rules out Tauri and
Electron: they own the process and window. Classic JUCE `Component` UIs are
robust but slow to iterate on and hard to make look modern. JUCE 8+ ships a
first-class `WebBrowserComponent` with native-function and event bridges.

## Decision

Build the UI with Vite, React and TypeScript, embed the production bundle in
the binary, and talk to the processor through a small, versioned bridge
protocol. The UI ships a mock backend so it runs in a normal browser.

## Consequences

- Hot reload and the whole web tool chain for UI work; designers can work in a
  browser with no audio stack.
- WebView adds roughly 30–60 MB of resident memory per open editor and needs
  WebView2 on Windows and WebKitGTK on Linux.
- All DSP stays in C++. The UI never runs on the audio thread and never
  computes audio.
