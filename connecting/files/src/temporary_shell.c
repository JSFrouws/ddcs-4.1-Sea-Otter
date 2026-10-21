/*
 * temporary_shell.c - a temporary stand-in for the DDCS V4.1's ddcsv4.out.
 *
 * run_me_first.bat puts this in place of the vendor app (renamed to ddcsv4.out.original).
 * After the next power-on the controller's boot script starts it instead of the vendor
 * app. It:
 *   1. reads /ddcsv4/connecting.conf  (IP=, MASK=, GW=, TOKEN= lines, written by
 *      run_me_first.bat) and brings up eth0 with that address,
 *   2. listens on TCP 7777; a client must first send the 32-character TOKEN, then gets
 *      /bin/sh (as root) on the socket,
 *   3. keeps accepting, so a dropped connection doesn't need another reboot.
 * run_me_after_reboot.bat uses that shell once to install the SSH server and put the
 * vendor app back. Static ARMv5 build: see build.sh.
 *
 * SPDX-License-Identifier: MIT
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONF "/ddcsv4/connecting.conf"
#define PORT 7777
#define TOKEN_LEN 32

static char ip[32] = "192.168.1.200", mask[32] = "255.255.255.0", gw[32] = "", token[TOKEN_LEN + 1];

static void read_conf(void)
{
	FILE *f = fopen(CONF, "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "IP=", 3)) snprintf(ip, sizeof ip, "%s", line + 3);
		else if (!strncmp(line, "MASK=", 5)) snprintf(mask, sizeof mask, "%s", line + 5);
		else if (!strncmp(line, "GW=", 3)) snprintf(gw, sizeof gw, "%s", line + 3);
		else if (!strncmp(line, "TOKEN=", 6)) snprintf(token, sizeof token, "%s", line + 6);
	}
	fclose(f);
}

static void set_addr(struct sockaddr *sa, const char *a)
{
	struct sockaddr_in *s = (struct sockaddr_in *)sa;
	s->sin_family = AF_INET;
	s->sin_addr.s_addr = inet_addr(a);
}

static void network_up(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return;
	struct ifreq r;
	memset(&r, 0, sizeof r);
	strncpy(r.ifr_name, "eth0", IFNAMSIZ - 1);
	set_addr(&r.ifr_addr, ip);
	ioctl(s, SIOCSIFADDR, &r);
	set_addr(&r.ifr_netmask, mask);
	ioctl(s, SIOCSIFNETMASK, &r);
	if (ioctl(s, SIOCGIFFLAGS, &r) == 0) { r.ifr_flags |= IFF_UP | IFF_RUNNING; ioctl(s, SIOCSIFFLAGS, &r); }
	if (gw[0]) {
		struct rtentry rt;
		memset(&rt, 0, sizeof rt);
		set_addr((struct sockaddr *)&rt.rt_gateway, gw);
		rt.rt_dst.sa_family = AF_INET;
		rt.rt_genmask.sa_family = AF_INET;
		rt.rt_flags = RTF_UP | RTF_GATEWAY;
		rt.rt_dev = "eth0";
		ioctl(s, SIOCADDRT, &rt);
	}
	close(s);
}

static int read_full(int fd, char *b, size_t n)
{
	for (size_t got = 0; got < n;) {
		ssize_t r = read(fd, b + got, n - got);
		if (r <= 0) return -1;
		got += (size_t)r;
	}
	return 0;
}

int main(void)
{
	read_conf();
	network_up();
	if (strlen(token) != TOKEN_LEN) return 1;            /* no token: never open a shell */
	int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(PORT);
	if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 1) < 0) return 1;
	for (;;) {
		int fd = accept(srv, NULL, NULL);
		if (fd < 0) continue;
		char t[TOKEN_LEN];
		if (read_full(fd, t, TOKEN_LEN) < 0 || memcmp(t, token, TOKEN_LEN)) { close(fd); continue; }
		if (fork() == 0) {
			dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
			close(fd); close(srv);
			char *argv[] = { "/bin/sh", NULL };          /* not -i: no controlling tty here */
			execv("/bin/sh", argv);
			_exit(127);
		}
		close(fd);
		while (waitpid(-1, NULL, WNOHANG) > 0) {}
	}
}
