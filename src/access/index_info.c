/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * index_info.c - Owner-only inspection of BM25 index header facts
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <access/relation.h>
#include <catalog/pg_class.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/lsyscache.h>

#include "access/am.h"
#include "index/metapage.h"

static void
check_index_info_owner(Oid index_oid)
{
	char *name = get_rel_name(index_oid);

	if (name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("relation with OID %u does not exist", index_oid)));
	if (!object_ownercheck(RelationRelationId, index_oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, name);
}

PG_FUNCTION_INFO_V1(tp_index_info);

Datum
tp_index_info(PG_FUNCTION_ARGS)
{
	Oid				  index_oid = PG_GETARG_OID(0);
	Relation		  index_rel;
	TpIndexHeaderInfo info;
	TupleDesc		  tupdesc;
	HeapTuple		  tuple;
	Datum			  values[4] = {0};
	bool			  nulls[4]	= {false, true, true, true};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bm25_index_info requires a composite result type")));
	tupdesc = BlessTupleDesc(tupdesc);

	/* Ownership can change while we wait for the relation lock. */
	check_index_info_owner(index_oid);
	index_rel = relation_open(index_oid, AccessShareLock);
	check_index_info_owner(index_oid);

	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a bm25 index",
						RelationGetRelationName(index_rel))));
	if (index_rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a partitioned bm25 index",
						RelationGetRelationName(index_rel)),
				 errhint("Use a physical partition index instead.")));

	info = tp_inspect_metapage(index_rel);
	relation_close(index_rel, AccessShareLock);

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

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
