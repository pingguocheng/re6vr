// mem_scan.h - locate the engine's camera storage by searching for its own view matrix.
//
// Enabled by re6vr_scan.txt in the marker directory. It captures the view matrix the
// engine uploads at two different camera poses, then intersects the addresses that hold
// both - which is what identifies the camera's own memory without an offset table.
#pragma once

namespace re6vr {

// Called with each matrix the vertex-shader probe classifies as a view matrix. Only used
// while scanning; harmless otherwise.
void mem_scan_note_matrix(const float *m);

// Called every frame with the head pose as a row-major 3x3 (columns = the head's
// right / up / backward axes, the same convention the panel and the view matrix use).
//
// This is the strongest identifier available, because the head pose is a value the plugin
// KNOWS rather than infers. The game camera's world basis is the head basis composed with
// whatever rotation the game camera is at, so once the player has turned their head far
// enough for the composition to be dominated by the head, the camera's basis is very
// nearly the head basis - a 3x3 that can simply be searched for, with far less chance of
// coincidence than a translation or a single scalar.
void mem_scan_note_head(const float *basis9);

// Reads the marker file and, if present, starts the scan thread. Safe to call more than
// once. Does nothing unless re6vr_scan.txt exists.
void mem_scan_start();

bool mem_scan_enabled();
bool mem_scan_done();

} // namespace re6vr
