/*
 * Copyright (c) 2004 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "updatedb.h"

static void usage(void)
{
	fprintf(stderr, "Usage: nss_updatedb [nameservice] [passwd|group]\n");
	exit(NSS_STATUS_UNAVAIL);
}

static const char *nss_err2string(enum nss_status status)
{
	const char *serr;

	switch (status) {
	case NSS_STATUS_SUCCESS:
		serr = "success";
		break;
	case NSS_STATUS_TRYAGAIN:
		serr = "out of memory";
		break;
	case NSS_STATUS_UNAVAIL:
		serr = "nameservice unavailable";
		break;
	case NSS_STATUS_NOTFOUND:
		serr = "not found";
		break;
	default:
		serr = "unknown error";
		break;
	}

	return serr;
}

int main(int argc, char *argv[])
{
	nss_backend_handle_t *handle;
	unsigned maps = 0;
	char *dbname;
	enum nss_status status;

	if (argc < 2 || argc > 3) {
		usage();
	}

	dbname = argv[1];
	if (strcmp(dbname, "db") == 0) {
		fprintf(stderr, "Cannot run nss_updatedb against nss_db.\n");
		exit(NSS_STATUS_UNAVAIL);
	}

	if (argc == 3) {
		char *mapname;

		mapname = argv[2];

		if (strcmp(mapname, "passwd") == 0)
			maps = MAP_PASSWD;
		else if (strcmp(mapname, "group") == 0)
			maps = MAP_GROUP;
		else
			usage();
	} else {
		maps = MAP_ALL;
	}

	status = nss_backend_open(dbname, &handle);
	if (status != NSS_STATUS_SUCCESS) {
		exit(status);
	}

	if (maps & MAP_PASSWD) {
		printf("passwd... ");
		status = nss_update_db(handle, MAP_PASSWD, DB_PASSWD);
		if (status != NSS_STATUS_SUCCESS) {
			printf("%s.\n", nss_err2string(status));
			exit(status);
		}
		printf("done.\n");
	}

	if (maps & MAP_GROUP) {
		printf("group... ");
		status = nss_update_db(handle, MAP_GROUP, DB_GROUP);
		if (status != NSS_STATUS_SUCCESS) {
			printf("%s.\n", nss_err2string(status));
			exit(status);
		}
		printf("done.\n");
	}

	exit(NSS_STATUS_SUCCESS);
}
