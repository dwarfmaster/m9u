/* © 2008 sqweek <sqweek@gmail.com>
 * See COPYING for details.
 */
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ixp.h>

#include "m9u.h"

extern Ixp9Srv p9srv;

int chldpipe[2];

void
chld()
{
	char c = '\a';
	write(chldpipe[1], &c, 1);
	wait(NULL);
}

void
playerdeath(IxpConn *conn)
{
	char c;
	read(chldpipe[0], &c, 1);
	songends();
}

int
main(int argc, char **arv)
{
	IxpServer srv = {0};
	char *address, buf[512];
	sigset_t sigs;
	int fd, i;

	address = getenv("IXP_ADDRESS");
	if(!address) {
		char *nsdir = ixp_namespace();
		if(mkdir(nsdir, 0700) == -1 && errno != EEXIST) {
			err(1, "mkdir: %s", nsdir);
		}
		snprintf(buf, sizeof(buf), "unix!%s/m9u", nsdir);
		address = buf;
	}

	if(pipe(chldpipe) != 0) {
		err(1, "pipe");
	}
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGCHLD);
	sigprocmask(SIG_UNBLOCK, &sigs, NULL);
	signal(SIGCHLD, chld);

	fd = ixp_announce(address);
	if(fd < 0) {
		err(1, "ixp_announce");
	}

	ixp_listen(&srv, fd, &p9srv, serve_9pcon, NULL);
	ixp_listen(&srv, chldpipe[0], NULL, playerdeath, NULL);

	if(init() != 0) {
		errx(1, "initialisation failed");
	}

	i = ixp_serverloop(&srv);

	return i;
}
