\echo Use "CREATE EXTENSION pg_textsearch_index_info_test" to load this file. \quit

CREATE FUNCTION index_info_test_header(
    version bigint, bits bigint, legacy_pointer boolean,
    used_bytes integer, valid_magic boolean,
    OUT metapage_version bigint, OUT capabilities bigint,
    OUT all_documents_indexed boolean,
    OUT legacy_docid_pointer_present boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'index_info_test_header'
LANGUAGE C STRICT;

CREATE FUNCTION index_info_test_raw_page(regclass) RETURNS bytea
AS 'MODULE_PATHNAME', 'index_info_test_raw_page'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION
    index_info_test_header(bigint, bigint, boolean, integer, boolean)
    FROM PUBLIC;
REVOKE ALL ON FUNCTION index_info_test_raw_page(regclass) FROM PUBLIC;
