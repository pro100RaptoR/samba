/*
   Unix SMB/CIFS implementation.

   module to store/fetch session keys for the schannel server

   Copyright (C) Andrew Tridgell 2004
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2006-2009
   Copyright (C) Guenther Deschner 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "system/filesys.h"
#include "../lib/tdb/include/tdb.h"
#include "../lib/util/util_tdb.h"
#include "../lib/param/param.h"
#include "../libcli/auth/schannel.h"
#include "../librpc/gen_ndr/ndr_schannel.h"
#include "lib/dbwrap/dbwrap.h"

#define SECRETS_SCHANNEL_STATE "SECRETS/SCHANNEL"

/******************************************************************************
 Open or create the schannel session store tdb.  Non-static so it can
 be called from parent processes to corectly handle TDB_CLEAR_IF_FIRST
*******************************************************************************/

struct db_context *open_schannel_session_store(TALLOC_CTX *mem_ctx,
					       struct loadparm_context *lp_ctx)
{
	struct db_context *db_sc = NULL;
	char *fname = lpcfg_private_db_path(mem_ctx, lp_ctx, "schannel_store");

	if (!fname) {
		return NULL;
	}

	db_sc = dbwrap_local_open(mem_ctx, lp_ctx, fname, 0,
				  TDB_CLEAR_IF_FIRST|TDB_NOSYNC, O_RDWR|O_CREAT,
				  0600, 0);

	if (!db_sc) {
		DEBUG(0,("open_schannel_session_store: Failed to open %s - %s\n",
			 fname, strerror(errno)));
		TALLOC_FREE(fname);
		return NULL;
	}

	TALLOC_FREE(fname);

	return db_sc;
}

/********************************************************************
 ********************************************************************/

static
NTSTATUS schannel_store_session_key_tdb(struct db_context *db_sc,
					TALLOC_CTX *mem_ctx,
					struct netlogon_creds_CredentialState *creds)
{
	enum ndr_err_code ndr_err;
	DATA_BLOB blob;
	TDB_DATA value;
	char *keystr;
	char *name_upper;
	NTSTATUS status;

	name_upper = strupper_talloc(mem_ctx, creds->computer_name);
	if (!name_upper) {
		return NT_STATUS_NO_MEMORY;
	}

	keystr = talloc_asprintf(mem_ctx, "%s/%s",
				 SECRETS_SCHANNEL_STATE, name_upper);
	TALLOC_FREE(name_upper);
	if (!keystr) {
		return NT_STATUS_NO_MEMORY;
	}

	ndr_err = ndr_push_struct_blob(&blob, mem_ctx, creds,
			(ndr_push_flags_fn_t)ndr_push_netlogon_creds_CredentialState);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		talloc_free(keystr);
		return ndr_map_error2ntstatus(ndr_err);
	}

	value.dptr = blob.data;
	value.dsize = blob.length;

	status = dbwrap_store_bystring(db_sc, keystr, value, TDB_REPLACE);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("Unable to add %s to session key db - %s\n",
			 keystr, nt_errstr(status)));
		talloc_free(keystr);
		return status;
	}

	DEBUG(3,("schannel_store_session_key_tdb: stored schannel info with key %s\n",
		keystr));

	if (DEBUGLEVEL >= 10) {
		NDR_PRINT_DEBUG(netlogon_creds_CredentialState, creds);
	}

	talloc_free(keystr);

	return NT_STATUS_OK;
}

/********************************************************************
 ********************************************************************/

static
NTSTATUS schannel_fetch_session_key_tdb(struct db_context *db_sc,
					TALLOC_CTX *mem_ctx,
					const char *computer_name,
					struct netlogon_creds_CredentialState **pcreds)
{
	NTSTATUS status;
	TDB_DATA value;
	enum ndr_err_code ndr_err;
	DATA_BLOB blob;
	struct netlogon_creds_CredentialState *creds = NULL;
	char *keystr = NULL;
	char *name_upper;

	*pcreds = NULL;

	name_upper = strupper_talloc(mem_ctx, computer_name);
	if (!name_upper) {
		return NT_STATUS_NO_MEMORY;
	}

	keystr = talloc_asprintf(mem_ctx, "%s/%s",
				 SECRETS_SCHANNEL_STATE, name_upper);
	TALLOC_FREE(name_upper);
	if (!keystr) {
		return NT_STATUS_NO_MEMORY;
	}

	status = dbwrap_fetch_bystring(db_sc, keystr, keystr, &value);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(10,("schannel_fetch_session_key_tdb: Failed to find entry with key %s\n",
			keystr ));
		goto done;
	}

	creds = talloc_zero(mem_ctx, struct netlogon_creds_CredentialState);
	if (!creds) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	blob = data_blob_const(value.dptr, value.dsize);

	ndr_err = ndr_pull_struct_blob(&blob, creds, creds,
			(ndr_pull_flags_fn_t)ndr_pull_netlogon_creds_CredentialState);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		status = ndr_map_error2ntstatus(ndr_err);
		goto done;
	}

	if (DEBUGLEVEL >= 10) {
		NDR_PRINT_DEBUG(netlogon_creds_CredentialState, creds);
	}

	DEBUG(3,("schannel_fetch_session_key_tdb: restored schannel info key %s\n",
		keystr));

	status = NT_STATUS_OK;

 done:

	talloc_free(keystr);

	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(creds);
		return status;
	}

	*pcreds = creds;

	return NT_STATUS_OK;
}

/******************************************************************************
 Wrapper around schannel_fetch_session_key_tdb()
 Note we must be root here.
*******************************************************************************/

NTSTATUS schannel_get_creds_state(TALLOC_CTX *mem_ctx,
				  struct loadparm_context *lp_ctx,
				  const char *computer_name,
				  struct netlogon_creds_CredentialState **_creds)
{
	TALLOC_CTX *tmpctx;
	struct db_context *db_sc;
	struct netlogon_creds_CredentialState *creds;
	NTSTATUS status;

	tmpctx = talloc_named(mem_ctx, 0, "schannel_get_creds_state");
	if (!tmpctx) {
		return NT_STATUS_NO_MEMORY;
	}

	db_sc = open_schannel_session_store(tmpctx, lp_ctx);
	if (!db_sc) {
		return NT_STATUS_ACCESS_DENIED;
	}

	status = schannel_fetch_session_key_tdb(db_sc, tmpctx,
						computer_name, &creds);
	if (NT_STATUS_IS_OK(status)) {
		*_creds = talloc_steal(mem_ctx, creds);
		if (!*_creds) {
			status = NT_STATUS_NO_MEMORY;
		}
	}

	talloc_free(tmpctx);
	return status;
}

/******************************************************************************
 Wrapper around schannel_store_session_key_tdb()
 Note we must be root here.
*******************************************************************************/

NTSTATUS schannel_save_creds_state(TALLOC_CTX *mem_ctx,
				   struct loadparm_context *lp_ctx,
				   struct netlogon_creds_CredentialState *creds)
{
	TALLOC_CTX *tmpctx;
	struct db_context *db_sc;
	NTSTATUS status;

	tmpctx = talloc_named(mem_ctx, 0, "schannel_save_creds_state");
	if (!tmpctx) {
		return NT_STATUS_NO_MEMORY;
	}

	db_sc = open_schannel_session_store(tmpctx, lp_ctx);
	if (!db_sc) {
		return NT_STATUS_ACCESS_DENIED;
	}

	status = schannel_store_session_key_tdb(db_sc, tmpctx, creds);

	talloc_free(tmpctx);
	return status;
}

/********************************************************************
 Validate an incoming authenticator against the credentials for the
 remote machine stored in the schannel database.

 The credentials are (re)read and from the schannel database, and
 written back after the caclulations are performed.

 If the creds_out parameter is not NULL returns the credentials.
 ********************************************************************/

NTSTATUS schannel_check_creds_state(TALLOC_CTX *mem_ctx,
				    struct loadparm_context *lp_ctx,
				    const char *computer_name,
				    struct netr_Authenticator *received_authenticator,
				    struct netr_Authenticator *return_authenticator,
				    struct netlogon_creds_CredentialState **creds_out)
{
	TALLOC_CTX *tmpctx;
	struct db_context *db_sc;
	struct netlogon_creds_CredentialState *creds;
	NTSTATUS status;
	int ret;
	char *name_upper = NULL;
	char *keystr = NULL;
	struct db_record *record;
	TDB_DATA key;

	if (creds_out != NULL) {
		*creds_out = NULL;
	}

	tmpctx = talloc_named(mem_ctx, 0, "schannel_check_creds_state");
	if (!tmpctx) {
		return NT_STATUS_NO_MEMORY;
	}

	name_upper = strupper_talloc(tmpctx, computer_name);
	if (!name_upper) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	keystr = talloc_asprintf(tmpctx, "%s/%s",
				 SECRETS_SCHANNEL_STATE, name_upper);
	if (!keystr) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	key = string_term_tdb_data(keystr);

	db_sc = open_schannel_session_store(tmpctx, lp_ctx);
	if (!db_sc) {
		status = NT_STATUS_ACCESS_DENIED;
		goto done;
	}

	record = dbwrap_fetch_locked(db_sc, tmpctx, key);
	if (!record) {
		status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		goto done;
	}

	/* Because this is a shared structure (even across
	 * disconnects) we must update the database every time we
	 * update the structure */

	status = schannel_fetch_session_key_tdb(db_sc, tmpctx,
						computer_name, &creds);
	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	status = netlogon_creds_server_step_check(creds,
						  received_authenticator,
						  return_authenticator);
	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	status = schannel_store_session_key_tdb(db_sc, tmpctx, creds);
	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	if (creds_out) {
		*creds_out = talloc_steal(mem_ctx, creds);
		if (!*creds_out) {
			status = NT_STATUS_NO_MEMORY;
			goto done;
		}
	}

	status = NT_STATUS_OK;

done:
	talloc_free(tmpctx);
	return status;
}

