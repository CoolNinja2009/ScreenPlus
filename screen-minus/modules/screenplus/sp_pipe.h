// Byte stream to the Screen+ engine: a named pipe on Windows, a Unix socket elsewhere.
// No Godot types here, so <windows.h> stays out of the engine's headers.
// One thread may read while another writes; sp_pipe_close unblocks a pending read.

#pragma once

#include <cstddef>

struct SPPipe;

SPPipe *sp_pipe_open(const char *p_name); // UTF-8 name from --sp-pipe, nullptr on failure
bool sp_pipe_read(SPPipe *p_pipe, void *r_buf, size_t p_len); // exactly p_len bytes; false once closed
bool sp_pipe_write(SPPipe *p_pipe, const void *p_buf, size_t p_len);
void sp_pipe_close(SPPipe *p_pipe); // safe while another thread reads
void sp_pipe_free(SPPipe *p_pipe); // after the reader has finished
