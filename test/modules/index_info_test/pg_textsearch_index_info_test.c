/* Test-only synthetic headers and raw-page comparisons; never shipped. */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <utils/rel.h>

#include "index/metapage.h"

PG_MODULE_MAGIC;
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
	TupleDesc		  desc;
	Datum			  values[4] = {0};
	bool			  nulls[4]	= {false, true, true, true};
	HeapTuple		  tuple;

	if (!superuser())
		elog(ERROR, "must be superuser to construct test headers");
	if (version < 0 || version > PG_UINT32_MAX || bits < 0 ||
		bits > PG_UINT32_MAX || used < 0 ||
		(Size)used > BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
		elog(ERROR, "invalid test header arguments");
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "expected a composite result");

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
	/* PostgreSQL loads extension libraries locally, not into the global
	 * symbol namespace. Resolve the parser from its own library handle. */
	{
		typedef void (*InspectHeader)(Page, TpIndexHeaderInfo *);
		InspectHeader inspect = (InspectHeader)load_external_function(
				"$libdir/pg_textsearch",
				"tp_inspect_metapage_page",
				true,
				NULL);

		inspect(page, &info);
	}
	if (memcmp(before, page, BLCKSZ) != 0)
		elog(ERROR, "inspection changed a synthetic page");

	values[0] = Int64GetDatum((int64)info.version);
	if (info.capabilities_present)
	{
		values[1] = Int64GetDatum((int64)info.capabilities);
		values[2] = BoolGetDatum(
				(info.capabilities & TP_METAPAGE_ALL_DOCUMENTS_INDEXED) != 0);
		nulls[1] = nulls[2] = false;
	}
	if (info.legacy_docid_pointer_known)
	{
		values[3] = BoolGetDatum(info.legacy_docid_pointer_present);
		nulls[3]  = false;
	}
	tuple = heap_form_tuple(BlessTupleDesc(desc), values, nulls);
	pfree(page);
	pfree(before);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
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
