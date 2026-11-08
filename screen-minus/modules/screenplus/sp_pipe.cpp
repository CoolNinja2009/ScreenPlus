#include "sp_pipe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Overlapped, because a synchronous handle serialises I/O: a blocked read would block every write.
struct SPPipe {
	HANDLE h = INVALID_HANDLE_VALUE;
	HANDLE read_ev = nullptr;
	HANDLE write_ev = nullptr;
	volatile LONG closed = 0;
};

SPPipe *sp_pipe_open(const char *p_name) {
	char path[512];
	snprintf(path, sizeof(path), "\\.\pipe\%s", p_name);
	wchar_t wpath[512];
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512)) {
		return nullptr;
	}
	HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return nullptr;
	}
	SPPipe *p = new SPPipe;
	p->h = h;
	p->read_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	p->write_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	return p;
}

static bool io(SPPipe *p, HANDLE ev, bool p_write, void *buf, size_t len) {
	char *c = static_cast<char *>(buf);
	while (len > 0) {
		if (p->closed) {
			return false;
		}
		OVERLAPPED ov = {};
		ov.hEvent = ev;
		ResetEvent(ev);
		DWORD chunk = len > 0x10000000 ? 0x10000000 : (DWORD)len;
		DWORD done = 0;
		BOOL ok = p_write ? WriteFile(p->h, c, chunk, nullptr, &ov) : ReadFile(p->h, c, chunk, nullptr, &ov);
		if (!ok && GetLastError() != ERROR_IO_PENDING) {
			return false;
		}
		if (!GetOverlappedResult(p->h, &ov, &done, TRUE) || done == 0) {
			return false;
		}
		c += done;
		len -= done;
	}
	return true;
}

bool sp_pipe_read(SPPipe *p, void *r_buf, size_t p_len) {
	return io(p, p->read_ev, false, r_buf, p_len);
}

bool sp_pipe_write(SPPipe *p, const void *p_buf, size_t p_len) {
	return io(p, p->write_ev, true, const_cast<void *>(p_buf), p_len);
}

void sp_pipe_close(SPPipe *p) {
	InterlockedExchange(&p->closed, 1);
	CancelIoEx(p->h, nullptr);
}

void sp_pipe_free(SPPipe *p) {
	CloseHandle(p->h);
	CloseHandle(p->read_ev);
	CloseHandle(p->write_ev);
	delete p;
}

#else

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct SPPipe {
	int fd = -1;
};

SPPipe *sp_pipe_open(const char *p_name) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !*dir) {
		dir = "/tmp";
	}
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s.sock", dir, p_name) >= (int)sizeof(addr.sun_path)) {
		return nullptr;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return nullptr;
	}
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		close(fd);
		return nullptr;
	}
	SPPipe *p = new SPPipe;
	p->fd = fd;
	return p;
}

bool sp_pipe_read(SPPipe *p, void *r_buf, size_t p_len) {
	char *c = static_cast<char *>(r_buf);
	while (p_len > 0) {
		ssize_t n = recv(p->fd, c, p_len, 0);
		if (n <= 0) {
			return false;
		}
		c += n;
		p_len -= (size_t)n;
	}
	return true;
}

bool sp_pipe_write(SPPipe *p, const void *p_buf, size_t p_len) {
	const char *c = static_cast<const char *>(p_buf);
	while (p_len > 0) {
		ssize_t n = send(p->fd, c, p_len, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		c += n;
		p_len -= (size_t)n;
	}
	return true;
}

void sp_pipe_close(SPPipe *p) {
	shutdown(p->fd, SHUT_RDWR);
}

void sp_pipe_free(SPPipe *p) {
	close(p->fd);
	delete p;
}

#endif
