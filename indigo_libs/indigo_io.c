// Copyright (c) 2016 CloudMakers, s. r. o.
// Copyright (c) 2016 Rumen G.Bogdanovski
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO Bus
 \file indigo_io.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#if defined(INDIGO_LINUX) || defined(INDIGO_MACOS)
#include <unistd.h>
#include <termios.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#if defined(INDIGO_WINDOWS)
#include <io.h>
#include <winsock2.h>
#define close closesocket
#pragma warning(disable:4996)
#endif

#include <indigo/indigo_bus.h>
#include <indigo/indigo_io.h>

#if defined(INDIGO_LINUX) || defined(INDIGO_MACOS)

typedef struct {
	int value;
	size_t len;
	char *str;
} sbaud_rate;
#define BR(str,val) { val, sizeof(str), str }

static sbaud_rate br[] = {
	BR(     "50", B50),
	BR(     "75", B75),
	BR(    "110", B110),
	BR(    "134", B134),
	BR(    "150", B150),
	BR(    "200", B200),
	BR(    "300", B300),
	BR(    "600", B600),
	BR(   "1200", B1200),
	BR(   "1800", B1800),
	BR(   "2400", B2400),
	BR(   "4800", B4800),
	BR(   "9600", B9600),
	BR(  "19200", B19200),
	BR(  "38400", B38400),
	BR(  "57600", B57600),
	BR( "115200", B115200),
	BR( "230400", B230400),
#if !defined(__APPLE__) && !defined(__MACH__)
	BR( "460800", B460800),
	BR( "500000", B500000),
	BR( "576000", B576000),
	BR( "921600", B921600),
	BR("1000000", B1000000),
	BR("1152000", B1152000),
	BR("1500000", B1500000),
	BR("2000000", B2000000),
	BR("2500000", B2500000),
	BR("3000000", B3000000),
	BR("3500000", B3500000),
	BR("4000000", B4000000),
#endif /* not OSX */
	BR(       "", 0),
};

/* map string to actual baudrate value */
static int map_str_baudrate(const char *baudrate) {
	sbaud_rate *brp = br;
	while (strncmp(brp->str, baudrate, brp->len)) {
		if (brp->str[0]=='\0') return -1;
		brp++;
	}
	return brp->value;
}

static int configure_tty_options(struct termios *options, const char *baudrate) {
	int cbits = CS8, cpar = 0, ipar = IGNPAR, bstop = 0;
	int baudr = 0;
	char *mode;
	char copy[32];
	strncpy(copy, baudrate, sizeof(copy));

	/* firmat is 9600-8N1, so split baudrate from the rest */
	mode = strchr(copy, '-');
	if (mode == NULL) {
		errno = EINVAL;
		return -1;
	}
	*mode = '\0';
	++mode;

	baudr = map_str_baudrate(copy);
	if (baudr == -1) {
		errno = EINVAL;
		return -1;
	}

	if (strlen(mode) != 3) {
		errno = EINVAL;
		return -1;
	}

	switch (mode[0]) {
		case '8': cbits = CS8; break;
		case '7': cbits = CS7; break;
		case '6': cbits = CS6; break;
		case '5': cbits = CS5; break;
		default :
			errno = EINVAL;
			return -1;
			break;
	}

	switch (mode[1]) {
		case 'N':
		case 'n':
			cpar = 0;
			ipar = IGNPAR;
			break;
		case 'E':
		case 'e':
			cpar = PARENB;
			ipar = INPCK;
			break;
		case 'O':
		case 'o':
			cpar = (PARENB | PARODD);
			ipar = INPCK;
			break;
		default :
			errno = EINVAL;
			return -1;
			break;
	}

	switch (mode[2]) {
		case '1': bstop = 0; break;
		case '2': bstop = CSTOPB; break;
		default :
			errno = EINVAL;
			return -1;
			break;
	}

	memset(options, 0, sizeof(*options));  /* clear options struct */

	options->c_cflag = cbits | cpar | bstop | CLOCAL | CREAD;
	options->c_iflag = ipar;
	options->c_oflag = 0;
	options->c_lflag = 0;
	options->c_cc[VMIN] = 0;       /* block untill n bytes are received */
	options->c_cc[VTIME] = 50;     /* block untill a timer expires (n * 100 mSec.) */

	cfsetispeed(options, baudr);
	cfsetospeed(options, baudr);

	return 0;
}


static int open_tty(const char *tty_name, const struct termios *options, struct termios *old_options) {
	int tty_fd;

	tty_fd = open(tty_name, O_RDWR | O_NOCTTY | O_SYNC);
	if (tty_fd == -1) {
		return -1;
	}

	if (old_options) {
		if (tcgetattr(tty_fd, old_options) == -1) {
			close(tty_fd);
			return -1;
		}
	}

	if (tcsetattr(tty_fd, TCSANOW, options) == -1) {
		close(tty_fd);
		return -1;
	}

	return tty_fd;
}

int indigo_open_serial(const char *dev_file) {
	return indigo_open_serial_with_speed(dev_file, 9600);
}

int indigo_open_serial_with_speed(const char *dev_file, int speed) {
	char baud_str[32];

	snprintf(baud_str, sizeof(baud_str), "%d-8N1", speed);
	return indigo_open_serial_with_config(dev_file, baud_str);
}

/* baudconfig is in form "9600-8N1" */
int indigo_open_serial_with_config(const char *dev_file, const char *baudconfig) {
	struct termios to;

	int res = configure_tty_options(&to, baudconfig);
	if (res == -1)
		return res;

	return open_tty(dev_file, &to, NULL);
}

#endif /* Linux and Mac */

int indigo_open_tcp(const char *host, int port) {
	struct sockaddr_in srv_info;
	struct hostent *he;
	int sock;
	struct timeval timeout;
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	if ((he = gethostbyname(host)) == NULL) {
		return -1;
	}
	if ((sock = socket(AF_INET, SOCK_STREAM, 0))== -1) {
		return -1;
	}
	memset(&srv_info, 0, sizeof(srv_info));
	srv_info.sin_family = AF_INET;
	srv_info.sin_port = htons(port);
	srv_info.sin_addr = *((struct in_addr *)he->h_addr);
	if (connect(sock, (struct sockaddr *)&srv_info, sizeof(struct sockaddr))<0) {
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
		close(sock);
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
		close(sock);
		return -1;
	}
	return sock;
}

int indigo_open_udp(const char *host, int port) {
	struct sockaddr_in srv_info;
	struct hostent *he;
	int sock;
	struct timeval timeout;
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	if ((he = gethostbyname(host)) == NULL) {
		return -1;
	}
	if ((sock = socket(AF_INET, SOCK_DGRAM, 0))== -1) {
		return -1;
	}
	memset(&srv_info, 0, sizeof(srv_info));
	srv_info.sin_family = AF_INET;
	srv_info.sin_port = htons(port);
	srv_info.sin_addr = *((struct in_addr *)he->h_addr);
	if (connect(sock, (struct sockaddr *)&srv_info, sizeof(struct sockaddr))<0) {
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
		close(sock);
		return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
		close(sock);
		return -1;
	}
	return sock;
}

int indigo_read(int handle, char *buffer, long length) {
	long remains = length;
	long total_bytes = 0;
	while (true) {
#if defined(INDIGO_WINDOWS)
		long bytes_read = recv(handle, buffer, remains, 0);
		if (bytes_read == -1 && WSAGetLastError() == WSAETIMEDOUT) {
			Sleep(500);
			continue;
		}
#else
		long bytes_read = read(handle, buffer, remains);
#endif
		if (bytes_read <= 0) {
			return (int)bytes_read;
		}
		total_bytes += bytes_read;
		if (bytes_read == remains) {
			return (int)total_bytes;
		}
		buffer += bytes_read;
		remains -= bytes_read;
	}
}

#if defined(INDIGO_WINDOWS)
int indigo_recv(int handle, char *buffer, long length) {
	while (true) {
		long bytes_read = recv(handle, buffer, length, 0);
		if (bytes_read == -1 && WSAGetLastError() == WSAETIMEDOUT) {
			Sleep(500);
			continue;
		}
		return (int)bytes_read;
	}
}

int indigo_close(int handle) {
	return closesocket(handle);
}
#endif

int indigo_read_line(int handle, char *buffer, int length) {
	char c = '\0';
	long total_bytes = 0;
	while (total_bytes < length) {
#if defined(INDIGO_WINDOWS)
		long bytes_read = recv(handle, &c, 1, 0);
		if (bytes_read == -1 && WSAGetLastError() == WSAETIMEDOUT) {
			Sleep(500);
			continue;
		}
#else
		long bytes_read = read(handle, &c, 1);
#endif
		if (bytes_read > 0) {
			if (c == '\r')
				;
			else if (c != '\n')
				buffer[total_bytes++] = c;
			else
				break;
		} else {
			errno = ECONNRESET;
			INDIGO_TRACE_PROTOCOL(indigo_trace("%d → ERROR", handle));
			return -1;
		}
	}
	buffer[total_bytes] = '\0';
	INDIGO_TRACE_PROTOCOL(indigo_trace("%d → %s", handle, buffer));
	return (int)total_bytes;
}

bool indigo_write(int handle, const char *buffer, long length) {
	long remains = length;
	while (true) {

#if defined(INDIGO_WINDOWS)
		long bytes_written = send(handle, buffer, remains, 0);
#else
		long bytes_written = write(handle, buffer, remains);
#endif
		if (bytes_written < 0)
			return false;
		if (bytes_written == remains)
			return true;
		buffer += bytes_written;
		remains -= bytes_written;
	}
}

bool indigo_printf(int handle, const char *format, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	INDIGO_TRACE_PROTOCOL(indigo_trace("%d ← %s", handle, buffer));
	return indigo_write(handle, buffer, length);
}

int indigo_scanf(int handle, const char *format, ...) {
	char buffer[1024];
	if (indigo_read_line(handle, buffer, sizeof(buffer)) <= 0)
		return 0;
	va_list args;
	va_start(args, format);
	int count = vsscanf(buffer, format, args);
	va_end(args);
	return count;
}
