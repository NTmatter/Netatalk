
/*
 * $Id: cnid_cdb_open.c,v 1.1.4.1 2003-09-09 16:42:21 didg Exp $
 *
 * Copyright (c) 1999. Adrian Sun (asun@zoology.washington.edu)
 * All Rights Reserved. See COPYRIGHT.
 *
 * CNID database support. 
 *
 * here's the deal:
 *  1) afpd already caches did's. 
 *  2) the database stores cnid's as both did/name and dev/ino pairs. 
 *  3) RootInfo holds the value of the NextID.
 *  4) the cnid database gets called in the following manner --
 *     start a database:
 *     cnid = cnid_open(root_dir);
 *
 *     allocate a new id: 
 *     newid = cnid_add(cnid, dev, ino, parent did,
 *     name, id); id is a hint for a specific id. pass 0 if you don't
 *     care. if the id is already assigned, you won't get what you
 *     requested.
 *
 *     given an id, get a did/name and dev/ino pair.
 *     name = cnid_get(cnid, &id); given an id, return the corresponding
 *     info.
 *     return code = cnid_delete(cnid, id); delete an entry. 
 *
 * with AFP, CNIDs 0-2 have special meanings. here they are:
 * 0 -- invalid cnid
 * 1 -- parent of root directory (handled by afpd) 
 * 2 -- root directory (handled by afpd)
 *
 * CNIDs 4-16 are reserved according to page 31 of the AFP 3.0 spec so, 
 * CNID_START begins at 17.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifdef CNID_BACKEND_CDB

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif /* HAVE_FCNTL_H */
#include <sys/param.h>
#include <sys/stat.h>
#include <atalk/logger.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */

#include <db.h>

#include <atalk/adouble.h>
#include <atalk/cnid.h>
#include "cnid_cdb.h"
#include <atalk/util.h>

#include "cnid_cdb_private.h"

#ifndef MIN
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#endif /* ! MIN */

#define DBHOME        ".AppleDB"
#define DBCNID        "cnid2.db"
#define DBDEVINO      "devino.db"
#define DBDIDNAME     "didname.db"      /* did/full name mapping */
#define DBLOCKFILE    "cnid.lock"

#define DBHOMELEN    8
#define DBLEN        10

/* we version the did/name database so that we can change the format
 * if necessary. the key is in the form of a did/name pair. in this case,
 * we use 0/0. */
#define DBVERSION_KEY    "\0\0\0\0\0"
#define DBVERSION_KEYLEN 5
#define DBVERSION1       0x00000001U
#define DBVERSION        DBVERSION1

#define DBOPTIONS    (DB_CREATE | DB_INIT_CDB | DB_INIT_MPOOL)

#define MAXITER     0xFFFF      /* maximum number of simultaneously open CNID
                                 * databases. */

/* -----------------------
 * bandaid for LanTest performance pb. for now not used, cf. ifdef 0 below
*/
static int my_yield(void)
{
    struct timeval t;
    int ret;

    t.tv_sec = 0;
    t.tv_usec = 1000;
    ret = select(0, NULL, NULL, NULL, &t);
    return 0;
}

/* --------------- */
static int didname(dbp, pkey, pdata, skey)
DB *dbp;
const DBT *pkey, *pdata;
DBT *skey;
{
int len;
 
    memset(skey, 0, sizeof(DBT));
    skey->data = pdata->data + CNID_DID_OFS;
    len = strlen(skey->data + CNID_DID_LEN);
    skey->size = CNID_DID_LEN + len + 1;
    return (0);
}
 
/* --------------- */
static int devino(dbp, pkey, pdata, skey)
DB *dbp;
const DBT *pkey, *pdata;
DBT *skey;
{
    memset(skey, 0, sizeof(DBT));
    skey->data = pdata->data + CNID_DEVINO_OFS;
    skey->size = CNID_DEVINO_LEN;
    return (0);
}
 
/* --------------- */
static int  my_associate (DB *p, DB *s,
                   int (*callback)(DB *, const DBT *,const DBT *, DBT *),
                   u_int32_t flags)
{
#if DB_VERSION_MAJOR > 4 || (DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 1)
    return p->associate(p, NULL, s, callback, flags);
#else
    return p->associate(p,       s, callback, flags);
#endif
}

/* --------------- */
static int my_open(DB * p, const char *f, const char *d, DBTYPE t, u_int32_t flags, int mode)
{
#if DB_VERSION_MAJOR > 4 || (DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 1)
    return p->open(p, NULL, f, d, t, flags, mode);
#else
    return p->open(p, f, d, t, flags, mode);
#endif
}

/* --------------- */
static struct _cnid_db *cnid_cdb_new(const char *volpath)
{
    struct _cnid_db *cdb;

    if ((cdb = (struct _cnid_db *) calloc(1, sizeof(struct _cnid_db))) == NULL)
        return NULL;

    if ((cdb->volpath = strdup(volpath)) == NULL) {
        free(cdb);
        return NULL;
    }

    cdb->flags = CNID_FLAG_PERSISTENT;

    cdb->cnid_add = cnid_cdb_add;
    cdb->cnid_delete = cnid_cdb_delete;
    cdb->cnid_get = cnid_cdb_get;
    cdb->cnid_lookup = cnid_cdb_lookup;
    cdb->cnid_nextid = NULL;    /*cnid_cdb_nextid;*/
    cdb->cnid_resolve = cnid_cdb_resolve;
    cdb->cnid_update = cnid_cdb_update;
    cdb->cnid_close = cnid_cdb_close;

    return cdb;
}

/* --------------- */
struct _cnid_db *cnid_cdb_open(const char *dir, mode_t mask)
{
    struct stat st;
    char path[MAXPATHLEN + 1];
    CNID_private *db;
    struct _cnid_db *cdb;
    int open_flag, len;
    static int first = 0;
    int rc;

    if (!dir || *dir == 0) {
        return NULL;
    }

    /* this checks .AppleDB */
    if ((len = strlen(dir)) > (MAXPATHLEN - DBLEN - 1)) {
        LOG(log_error, logtype_default, "cnid_open: Pathname too large: %s", dir);
        return NULL;
    }

    if ((cdb = cnid_cdb_new(dir)) == NULL) {
        LOG(log_error, logtype_default, "cnid_open: Unable to allocate memory for database");
        return NULL;
    }

    if ((db = (CNID_private *) calloc(1, sizeof(CNID_private))) == NULL) {
        LOG(log_error, logtype_default, "cnid_open: Unable to allocate memory for database");
        goto fail_cdb;
    }

    cdb->_private = (void *) db;
    db->magic = CNID_DB_MAGIC;

    strcpy(path, dir);
    if (path[len - 1] != '/') {
        strcat(path, "/");
        len++;
    }

    strcpy(path + len, DBHOME);
    if ((stat(path, &st) < 0) && (ad_mkdir(path, 0777 & ~mask) < 0)) {
        LOG(log_error, logtype_default, "cnid_open: DBHOME mkdir failed for %s", path);
        goto fail_adouble;
    }

    open_flag = DB_CREATE;

    /* We need to be able to open the database environment with full
     * transaction, logging, and locking support if we ever hope to 
     * be a true multi-acess file server. */
    if ((rc = db_env_create(&db->dbenv, 0)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: db_env_create: %s", db_strerror(rc));
        goto fail_lock;
    }

    /* Open the database environment. */
    if ((rc = db->dbenv->open(db->dbenv, path, DBOPTIONS, 0666 & ~mask)) != 0) {
        if (rc == DB_RUNRECOVERY) {
            /* This is the mother of all errors.  We _must_ fail here. */
            LOG(log_error, logtype_default,
                "cnid_open: CATASTROPHIC ERROR opening database environment %s.  Run db_recovery -c immediately", path);
            goto fail_lock;
        }

        /* We can't get a full transactional environment, so multi-access
         * is out of the question.  Let's assume a read-only environment,
         * and try to at least get a shared memory pool. */
        if ((rc = db->dbenv->open(db->dbenv, path, DB_INIT_MPOOL, 0666 & ~mask)) != 0) {
            /* Nope, not a MPOOL, either.  Last-ditch effort: we'll try to
             * open the environment with no flags. */
            if ((rc = db->dbenv->open(db->dbenv, path, 0, 0666 & ~mask)) != 0) {
                LOG(log_error, logtype_default, "cnid_open: dbenv->open of %s failed: %s", path, db_strerror(rc));
                goto fail_lock;
            }
        }
        db->flags |= CNIDFLAG_DB_RO;
        open_flag = DB_RDONLY;
        LOG(log_info, logtype_default, "cnid_open: Obtained read-only database environment %s", path);
    }

    /* ---------------------- */
    /* Main CNID database.  Use a hash for this one. */

    if ((rc = db_create(&db->db_cnid, db->dbenv, 0)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to create cnid database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }

    if ((rc = my_open(db->db_cnid, DBCNID, DBCNID, DB_HASH, open_flag, 0666 & ~mask)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to open dev/ino database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }

    /* ---------------------- */
    /* did/name reverse mapping.  We use a BTree for this one. */

    if ((rc = db_create(&db->db_didname, db->dbenv, 0)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to create did/name database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }

    if ((rc = my_open(db->db_didname, DBCNID, DBDIDNAME,DB_HASH, open_flag, 0666 & ~mask))) {
        LOG(log_error, logtype_default, "cnid_open: Failed to open did/name database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }

    /* ---------------------- */
    /* dev/ino reverse mapping.  Use a hash for this one. */

    if ((rc = db_create(&db->db_devino, db->dbenv, 0)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to create dev/ino database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }

    if ((rc = my_open(db->db_devino, DBCNID, DBDEVINO, DB_HASH, open_flag, 0666 & ~mask)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to open devino database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }

    /* ---------------------- */
    /* Associate the secondary with the primary. */ 
    if ((rc = my_associate(db->db_cnid, db->db_didname, didname, 0)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to associate didname database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }
 
    if ((rc = my_associate(db->db_cnid, db->db_devino, devino, 0)) != 0) {
        LOG(log_error, logtype_default, "cnid_open: Failed to associate devino database: %s",
            db_strerror(rc));
        goto fail_appinit;
    }
 
#if 0
    DBT key, pkey, data;
    /* ---------------------- */
    /* Check for version.  This way we can update the database if we need
     * to change the format in any way. */
    memset(&key, 0, sizeof(key));
    memset(&pkey, 0, sizeof(DBT));
    memset(&data, 0, sizeof(data));
    key.data = DBVERSION_KEY;
    key.size = DBVERSION_KEYLEN;

    if ((rc = db->db_didname->pget(db->db_didname, NULL, &key, &pkey, &data, 0)) != 0) {
        int ret;
        {
            u_int32_t version = htonl(DBVERSION);

            data.data = &version;
            data.size = sizeof(version);
        }
        if ((ret = db->db_didname->put(db->db_cnid, NULL, &key, &data,
                                       DB_NOOVERWRITE))) {
            LOG(log_error, logtype_default, "cnid_open: Error putting new version: %s",
                db_strerror(ret));
            db->db_didname->close(db->db_didname, 0);
            goto fail_appinit;
        }
    }
#endif

    /* TODO In the future we might check for version number here. */
#if 0
    memcpy(&version, data.data, sizeof(version));
    if (version != ntohl(DBVERSION)) {
        /* Do stuff here. */
    }
#endif /* 0 */

    /* Print out the version of BDB we're linked against. */
    if (!first) {
        first = 1;
        LOG(log_info, logtype_default, "CNID DB initialized using %s", db_version(NULL, NULL, NULL));
    }

    db_env_set_func_yield(my_yield);
    return cdb;

  fail_appinit:
    if (db->db_didname)
        db->db_didname->close(db->db_didname, 0);
    if (db->db_devino)
        db->db_devino->close(db->db_devino, 0);
    if (db->db_cnid)
        db->db_cnid->close(db->db_cnid, 0);
    LOG(log_error, logtype_default, "cnid_open: Failed to setup CNID DB environment");
    db->dbenv->close(db->dbenv, 0);

  fail_lock:

  fail_adouble:

    free(db);

  fail_cdb:
    if (cdb->volpath != NULL)
        free(cdb->volpath);
    free(cdb);

    return NULL;
}

struct _cnid_module cnid_cdb_module = {
    "cdb",
    {NULL, NULL},
    cnid_cdb_open,
    CNID_FLAG_SETUID | CNID_FLAG_BLOCK
};


#endif /* CNID_BACKEND_CDB */