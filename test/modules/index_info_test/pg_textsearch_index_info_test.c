/* Test-only synthetic headers and raw-page comparisons; never shipped. */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/rel.h>

#include "access/index_info.h"
#include "index/metapage.h"

PG_MODULE_MAGIC;

/*
 * PostgreSQL loads extension libraries locally, not into the global symbol
 * namespace. Resolve pg_textsearch functions from its own library handle.
 */
typedef void (*index_info_test_fn)(void);

static index_info_test_fn
index_info_test_lookup(const char *name)
{
	return (index_info_test_fn)
			load_external_function("$libdir/pg_textsearch", name, true, NULL);
}

PG_FUNCTION_INFO_V1(index_info_test_header);
PG_FUNCTION_INFO_V1(index_info_test_raw_page);

Datum
index_info_test_header(PG_FUNCTION_ARGS)
{
	int64			  version = PG_GETARG_INT64(0);
	int64			  bits	  = PG_GETARG_INT64(1);
	int32			  used	  = PG_GETARG_INT32(3);
	Page			  page;
	Page			  before;
	TpIndexMetaPage	  meta;
	TpIndexHeaderInfo info;
	Datum			  result;
	void (*inspect)(Page, TpIndexHeaderInfo *);
	Datum (*to_datum)(FunctionCallInfo, const TpIndexHeaderInfo *);

	if (!superuser())
		elog(ERROR, "must be superuser to construct test headers");
	if (version < 0 || version > PG_UINT32_MAX || bits < 0 ||
		bits > PG_UINT32_MAX || used < 0 ||
		(Size)used > BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
		elog(ERROR, "invalid test header arguments");

	page   = palloc0(BLCKSZ);
	before = palloc(BLCKSZ);
	PageInit(page, BLCKSZ, 0);
	meta					 = (TpIndexMetaPage)PageGetContents(page);
	meta->magic				 = PG_GETARG_BOOL(4) ? TP_METAPAGE_MAGIC : 0;
	meta->version			 = (uint32)version;
	meta->capabilities		 = (uint32)bits;
	meta->_unused_docid_page = PG_GETARG_BOOL(2) ? 123 : InvalidBlockNumber;
	((PageHeader)page)->pd_lower = MAXALIGN(SizeOfPageHeaderData) + used;
	memcpy(before, page, BLCKSZ);
	inspect = (void (*)(Page, TpIndexHeaderInfo *))index_info_test_lookup(
			"tp_inspect_metapage_page");
	to_datum = (Datum (*)(FunctionCallInfo, const TpIndexHeaderInfo *))
			index_info_test_lookup("tp_index_header_info_datum");

	inspect(page, &info);
	if (memcmp(before, page, BLCKSZ) != 0)
		elog(ERROR, "inspection changed a synthetic page");

	/* Same row construction as bm25_index_info(). */
	result = to_datum(fcinfo, &info);
	pfree(page);
	pfree(before);
	PG_RETURN_DATUM(result);
}

Datum
index_info_test_raw_page(PG_FUNCTION_ARGS)
{
	Relation index;
	Buffer	 buf;
	bytea	*result;

	if (!superuser())
		elog(ERROR, "must be superuser to inspect raw test pages");
	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	buf	  = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	result = palloc(VARHDRSZ + BLCKSZ);
	SET_VARSIZE(result, VARHDRSZ + BLCKSZ);
	memcpy(VARDATA(result), BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	index_close(index, AccessShareLock);
	PG_RETURN_BYTEA_P(result);
}
