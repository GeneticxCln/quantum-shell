// This file lives in a directory the scanner must treat as build output and skip. It carries an
// unfinished marker on purpose: if the skip rule breaks, this case fails and reports the marker.
// TODO: this marker must never be reported by the scan.
int configure_steps() {
    return 2;
}
