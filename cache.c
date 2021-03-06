/*
 * Copyright (c) 2004-2008 PADL Software Pty Ltd.
 * All rights reserved.
 * Use is subject to license.
 */

#define _GNU_SOURCE /* stpcpy() */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include <db.h>

#include "cache.h"
#include "updatedb.h"

/*
 * Make name service data persistent
 */

struct nss_cache {
	char *filename;
	char *tmpfilename;
	DB *db;
	int index;
};

static void nss_cache_error(const DB_ENV *dbenv, const char *errpfx, const char *msg)
{
	fprintf(stderr, "ERROR: nss_cache_error: %s\n", msg);
}

enum nss_status nss_cache_init(const char *filename,
			       nss_cache_t **cache_p)
{
	nss_cache_t *cache;
	int rc = -1, mode = 0644;

	cache = (nss_cache_t *)calloc(1, sizeof(*cache));
	if (cache == NULL) {
		return NSS_STATUS_TRYAGAIN;
	}

	cache->filename = strdup(filename);
	if (cache->filename == NULL) {
		nss_cache_close(&cache);
		return NSS_STATUS_TRYAGAIN;
	}

	/* Create a temporary database file */
	cache->tmpfilename = strdup(DB_DIR "/tmpdbXXXXXX");
	if (cache->tmpfilename == NULL) {
		nss_cache_close(&cache);
		return NSS_STATUS_TRYAGAIN;
	}
	if (mktemp(cache->tmpfilename) == NULL) {
		nss_cache_close(&cache);
		return NSS_STATUS_TRYAGAIN;
	}

#if DB_VERSION_MAJOR >= 4
	rc = db_create(&cache->db, NULL, 0);
	if (rc != 0) {
		nss_cache_close(&cache);
		errno = rc;
		return NSS_STATUS_UNAVAIL;
	}

	cache->db->set_errcall(cache->db, &nss_cache_error);

	rc = cache->db->open(cache->db, NULL,
			     cache->tmpfilename, NULL, 
			     DB_BTREE, DB_CREATE | DB_EXCL, mode);
	if (rc != 0) {
		nss_cache_close(&cache);
		errno = rc;
		return NSS_STATUS_UNAVAIL;
	}

#elif DB_VERSION_MAJOR == 3
	rc = db_create(&cache->db, NULL, 0);
	if (rc != 0) {
		nss_cache_close(&cache);
		errno = rc;
		return NSS_STATUS_UNAVAIL;
	}
	rc = cache->db->open(cache->db, cache->tmpfilename, NULL,
			     DB_BTREE, DB_CREATE | DB_TRUNCATE, mode);
	if (rc != 0) {
		nss_cache_close(&cache);
		errno = rc;
		return NSS_STATUS_UNAVAIL;
	}
#elif DB_VERSION_MAJOR == 2
	rc = db_open(cache->tmpfilename, DB_BTREE, DB_CREATE | DB_TRUNCATE,
		     mode, NULL, NULL, &cache->db);
	if (rc != 0) {
		nss_cache_close(&cache);
		errno = rc;
		return NSS_STATUS_UNAVAIL;
	}
#else
	cache->db = dbopen(cache->tmpfilename, O_CREAT | O_TRUNC,
			   mode, DB_BTREE, NULL);
	if (cache->db == NULL) {
		nss_cache_close(&cache);
		return NSS_STATUS_UNAVAIL;
	}
#endif

	*cache_p = cache;

	return NSS_STATUS_SUCCESS;
}

enum nss_status nss_cache_put(nss_cache_t *cache,
			      const char *key,
			      const char *value)
{
	DBT db_key;
	DBT db_val;
	int rc;

	memset(&db_key, 0, sizeof(db_key));
	db_key.data = (char *)key;
	db_key.size = strlen(key);

	memset(&db_val, 0, sizeof(db_val));
	db_val.data = (char *)value;
	db_val.size = strlen(value) + 1;

	rc = (cache->db->put)(cache->db,
#if DB_VERSION_MAJOR >= 4
			      NULL,
#elif DB_VERSION_MAJOR >= 2
			      NULL,
#endif
			      &db_key,
			      &db_val,
#if DB_VERSION_MAJOR >= 2
			      DB_NOOVERWRITE
#else
			      R_NOOVERWRITE
#endif
			      );

#if DB_VERSION_MAJOR < 2
#define DB_KEYEXIST 1
#endif

	if (rc == DB_KEYEXIST) {
		/*fprintf(stderr, "Ignoring duplicate key %s\n", key);*/
	} else if (rc != 0) {
#if DB_VERSION_MAJOR >= 2
		errno = rc;
#endif
		fprintf(stderr, "Error: puting into DB: %s: key '%s' value '%s'\n", 
			db_strerror(rc), (char *)db_key.data, (char *)db_val.data);
		return NSS_STATUS_UNAVAIL;
	}

	return NSS_STATUS_SUCCESS;
}

static enum nss_status _nss_cache_put_name(nss_cache_t *cache,
					   const char *name,
					   const char *value)
{
	char *s = NULL;
	size_t len;
	char buf[1024], *buf2 = NULL;
	enum nss_status status;

	len = strlen(name);
	if (len >= sizeof(buf)) {
		buf2 = malloc(1 + len + 1);
		if (buf2 == NULL)
			return NSS_STATUS_TRYAGAIN;
		s = buf2;
	} else {
		s = buf;
	}

	s[0] = '.';
	memcpy(&s[1], name, len);
	s[1 + len] = '\0';

	status = nss_cache_put(cache, s, value);

	if (buf2 != NULL)
		free(buf2);

	return status;
}

static enum nss_status _nss_cache_put_id(nss_cache_t *cache,
					 int id,
					 const char *value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "=%d", id);

	return nss_cache_put(cache, buf, value);
}

static enum nss_status _nss_cache_put_index(nss_cache_t *cache,
					    int index,
					    const char *value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "0%d", index);

	return nss_cache_put(cache, buf, value);
}

enum nss_status nss_cache_putpwent(nss_cache_t *cache,
				   struct passwd *pw)
{
	/*
	 * We need to put the following keys:
	 *
	 *	=<UID>
	 *	0<SEQUENCE>
	 *	.<USERNAME>
	 */
	char pwbuf[1024];
	enum nss_status status;

	if (pw->pw_name == NULL) {
		return NSS_STATUS_NOTFOUND;
	}

	snprintf(pwbuf, sizeof(pwbuf), "%s:%s:%d:%d:%s:%s:%s",
		 pw->pw_name,
		 (pw->pw_passwd != NULL) ? pw->pw_passwd : "x",
		 pw->pw_uid,
		 pw->pw_gid,
		 (pw->pw_gecos != NULL) ? pw->pw_gecos : "",
		 (pw->pw_dir != NULL) ? pw->pw_dir : "",
		 (pw->pw_shell != NULL) ? pw->pw_shell : "");

	status = _nss_cache_put_name(cache, pw->pw_name, pwbuf);
	if (status != NSS_STATUS_SUCCESS)
		return status;

	status = _nss_cache_put_id(cache, pw->pw_uid, pwbuf);
	if (status != NSS_STATUS_SUCCESS)
		return status;

	status = _nss_cache_put_index(cache, cache->index, pwbuf);
	if (status != NSS_STATUS_SUCCESS)
		return status;

	cache->index++;

	return NSS_STATUS_SUCCESS;
}

enum nss_status nss_cache_putgrent(nss_cache_t *cache,
				   struct group *gr)
{
	/*
	 * We need to put the following keys:
	 *
	 *	=<GID>
	 *	0<SEQUENCE>
	 *	.<GROUPNAME>
	 */
	char grbuf[1024], *grbuf2 = NULL;
	char *grent, *p;
	enum nss_status status;
	char **grmem_p;
	size_t len;
	char gidbuf[32];

	if (gr->gr_name == NULL) {
		return NSS_STATUS_NOTFOUND;
	}

	snprintf(gidbuf, sizeof(gidbuf), "%d", gr->gr_gid);

	len = strlen(gr->gr_name) + 1 /* : */;
	len += ((gr->gr_passwd != NULL) ? strlen(gr->gr_passwd) : 1) + 1 /* : */;
	len += strlen(gidbuf) + 1 /* : */;

	if (gr->gr_mem != NULL) {
		for (grmem_p = gr->gr_mem; *grmem_p != NULL; grmem_p++) {
			if (grmem_p != gr->gr_mem)
				len++; /* , */
			len += strlen(*grmem_p);
		}
	}
	len++; /* \0 terminator */

	if (len > sizeof(grbuf)) {
		grbuf2 = malloc(len);
		if (grbuf2 == NULL)
			return NSS_STATUS_TRYAGAIN;
		grent = grbuf2;
	} else {
		grent = grbuf;
	}

	p = grent;
	p = stpcpy(p, gr->gr_name);
	p = stpcpy(p, ":");
	p = stpcpy(p, (gr->gr_passwd != NULL) ? gr->gr_passwd : "x");
	p = stpcpy(p, ":");
	p = stpcpy(p, gidbuf);
	p = stpcpy(p, ":");

	if (gr->gr_mem != NULL) {
		for (grmem_p = gr->gr_mem; *grmem_p != NULL; grmem_p++) {
			if (grmem_p != gr->gr_mem)
				p = stpcpy(p, ",");
			p = stpcpy(p, *grmem_p);
		}
	}

	status = _nss_cache_put_name(cache, gr->gr_name, grent);
	if (status != NSS_STATUS_SUCCESS) {
		if (grbuf2 != NULL)
			free(grbuf2);
		return status;
	}

	status = _nss_cache_put_id(cache, gr->gr_gid, grent);
	if (status != NSS_STATUS_SUCCESS) {
		if (grbuf2 != NULL)
			free(grbuf2);
		return status;
	}

	status = _nss_cache_put_index(cache, cache->index, grent);
	if (status != NSS_STATUS_SUCCESS) {
		if (grbuf2 != NULL)
			free(grbuf2);
		return status;
	}

	cache->index++;

	if (grbuf2 != NULL)
		free(grbuf2);

	return NSS_STATUS_SUCCESS;
}

enum nss_status nss_cache_commit(nss_cache_t *cache)
{
	int rc;

#if 0
	if (cache->index == 0) {
		fprintf(stderr, "Warning: no information was retrieved from the "
			"name service, so the cache will not be replaced\n");
		return NSS_STATUS_UNAVAIL;
	}
#endif

	rc = (cache->db->sync)(cache->db, 0);
	if (rc != 0) {
#if DB_VERSION_MAJOR > 2
		errno = rc;
#endif
		return NSS_STATUS_UNAVAIL;
	}

	/* Link temporary file to final filename.
	   Use rename and link to avoid cache being unavailable. */
	rc = rename(cache->tmpfilename, cache->filename);
	if (rc != 0) {
		perror("rename");
		return NSS_STATUS_UNAVAIL;
	}
	rc = link(cache->filename, cache->tmpfilename);
	if (rc != 0) {
		perror("link");
		return NSS_STATUS_UNAVAIL;
	}

	return NSS_STATUS_SUCCESS;
}

enum nss_status nss_cache_abort(nss_cache_t *cache)
{
	return NSS_STATUS_UNAVAIL;
}

enum nss_status nss_cache_close(nss_cache_t **cache_p)
{
	nss_cache_t *cache;

	cache = *cache_p;

	if (cache != NULL) {
#if DB_VERSION_MAJOR >= 4
		if (cache->db != NULL)
			cache->db->close(cache->db, 0);
#endif
		if (cache->filename != NULL)
			free(cache->filename);
		if (cache->tmpfilename != NULL) {
			(void)unlink(cache->tmpfilename);
			free(cache->tmpfilename);
		}
		free(cache);
		*cache_p = NULL;
	}

	return NSS_STATUS_SUCCESS;
}
