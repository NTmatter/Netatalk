/*
  Copyright (c) 2012 Frank Lahm <franklahm@gmail.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#ifndef SPOTLIGHT_H
#define SPOTLIGHT_H

#include <stdint.h>
#include <stdbool.h>

#include <atalk/dalloc.h>
#include <atalk/globals.h>

/**************************************************************************************************
 * Spotlight module stuff
 **************************************************************************************************/

#define SL_MODULE_VERSION 1

struct sl_module_export {
    int sl_mod_version;
    int (*sl_mod_init)        (void *);
    int (*sl_mod_start_search)(void *);
    int (*sl_mod_fetch_result)(void *);
    int (*sl_mod_end_search)  (void *);
};

extern int sl_mod_load(const char *path);


/**************************************************************************************************
 * Spotlight RPC and marshalling stuff
 **************************************************************************************************/

/* FPSpotlightRPC subcommand codes */
#define SPOTLIGHT_CMD_OPEN    1
#define SPOTLIGHT_CMD_FLAGS   2
#define SPOTLIGHT_CMD_RPC     3
#define SPOTLIGHT_CMD_OPEN2   4

/* Can be ored and used as flags */
#define SL_ENC_LITTLE_ENDIAN 1
#define SL_ENC_BIG_ENDIAN    2
#define SL_ENC_UTF_16        4

typedef DALLOC_CTX     sl_array_t;    /* an array of elements                                           */
typedef DALLOC_CTX     sl_dict_t;     /* an array of key/value elements                                 */
typedef DALLOC_CTX     sl_filemeta_t; /* contains one sl_array_t                                        */
typedef int            sl_nil_t;      /* a nil element                                                  */
typedef bool           sl_bool_t;     /* a boolean, we avoid bool_t as it's a define for something else */
typedef struct timeval sl_time_t;     /* a boolean, we avoid bool_t as it's a define for something else */
typedef struct {
    char sl_uuid[16];
}                      sl_uuid_t;     /* a UUID                                                         */
typedef struct {
    uint16_t   ca_unkn1;
    uint32_t   ca_context;
    DALLOC_CTX *ca_cnids;
}                      sl_cnids_t;    /* an array of CNID                                               */

extern int afp_spotlight_rpc(AFPObj *obj, char *ibuf, size_t ibuflen _U_, char *rbuf, size_t *rbuflen);
extern int sl_pack(DALLOC_CTX *query, char *buf);
extern int sl_unpack(DALLOC_CTX *query, const char *buf);

#endif /* SPOTLIGHT_H */
