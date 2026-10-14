-- Header inspection requires what REINDEX requires: ownership of the index
-- or MAINTAIN on its table.  No superuser is involved.
CREATE EXTENSION pg_textsearch;
CREATE ROLE index_info_owner;
CREATE ROLE index_info_reader;
CREATE ROLE index_info_member INHERIT;
CREATE ROLE index_info_maintainer;
CREATE ROLE index_info_pg_maintain;
GRANT index_info_owner TO index_info_member;

CREATE TABLE index_info_docs (id integer PRIMARY KEY, body text);
INSERT INTO index_info_docs VALUES (1, 'alpha'), (2, ''), (3, 'the');
CREATE INDEX index_info_bm25 ON index_info_docs USING bm25(body)
    WITH (text_config = 'english');
ALTER TABLE index_info_docs OWNER TO index_info_owner;
GRANT SELECT ON index_info_docs TO index_info_reader;

SET ROLE index_info_owner;
SELECT rolsuper FROM pg_roles WHERE rolname = CURRENT_USER;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
-- Inspection also works in a read-only transaction.
BEGIN READ ONLY;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
COMMIT;
-- STRICT, with no attempt to open an index.
SELECT bm25_index_info(NULL::regclass) IS NULL AS strict_null;
-- Reject a table and another index access method.
SELECT * FROM bm25_index_info('index_info_docs'::regclass);
SELECT * FROM bm25_index_info('index_info_docs_pkey'::regclass);
SELECT * FROM bm25_index_info(0::oid::regclass);
SELECT bm25_compact(0::oid::regclass);
RESET ROLE;

-- A missing OID is a user-facing error for superusers too, and for every
-- caller of the shared bm25 open helper.
SELECT * FROM bm25_index_info(0::oid::regclass);
SELECT bm25_level_counts(0::oid::regclass);
SELECT bm25_compact(0::oid::regclass);

-- Ordinary ownership inheritance works; no inspection role is needed.
SET ROLE index_info_member;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
RESET ROLE;

-- SELECT privilege alone does not suffice.
SET ROLE index_info_reader;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
RESET ROLE;

-- MAINTAIN on the table suffices, as for REINDEX.  It does not extend to
-- compaction, which remains owner-only.
GRANT MAINTAIN ON index_info_docs TO index_info_maintainer;
SET ROLE index_info_maintainer;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
SELECT bm25_compact('index_info_bm25'::regclass);
RESET ROLE;

-- Revoking MAINTAIN takes effect.
REVOKE MAINTAIN ON index_info_docs FROM index_info_maintainer;
SET ROLE index_info_maintainer;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
RESET ROLE;

-- Members of pg_maintain have MAINTAIN on every table.
GRANT pg_maintain TO index_info_pg_maintain;
SET ROLE index_info_pg_maintain;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
RESET ROLE;

-- The owner may inspect even after revoking its own MAINTAIN.
SET ROLE index_info_owner;
REVOKE MAINTAIN ON index_info_docs FROM index_info_owner;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
GRANT MAINTAIN ON index_info_docs TO index_info_owner;
RESET ROLE;

-- Names resolved through regclass support schemas and quoted identifiers.
CREATE SCHEMA index_info_schema AUTHORIZATION index_info_owner;
SET ROLE index_info_owner;
CREATE TABLE index_info_schema.docs (body text);
CREATE INDEX "Header Facts" ON index_info_schema.docs USING bm25(body)
    WITH (text_config = 'english');
SELECT * FROM bm25_index_info('index_info_schema."Header Facts"'::regclass);
RESET ROLE;

-- Partitioned parents have no physical header; inspect a leaf instead.
CREATE TABLE index_info_parts (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE index_info_leaf PARTITION OF index_info_parts
    FOR VALUES FROM (0) TO (10);
CREATE INDEX index_info_parent ON index_info_parts USING bm25(body)
    WITH (text_config = 'english');
ALTER TABLE index_info_parts OWNER TO index_info_owner;
ALTER TABLE index_info_leaf OWNER TO index_info_owner;
SET ROLE index_info_owner;
SELECT * FROM bm25_index_info('index_info_parent'::regclass);
SELECT h.* FROM pg_inherits AS p
CROSS JOIN LATERAL bm25_index_info(p.inhrelid::regclass) AS h
WHERE p.inhparent = 'index_info_parent'::regclass;
RESET ROLE;

-- The relation lock is released when inspection returns, not held to the end
-- of the transaction, so inspecting inside a long transaction does not block
-- REINDEX or DROP INDEX.
SET ROLE index_info_owner;
BEGIN;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
SELECT count(*) AS locks_held FROM pg_locks
WHERE pid = pg_backend_pid() AND relation = 'index_info_bm25'::regclass;
COMMIT;
RESET ROLE;

-- A rebuilt index reports a fresh header.
REINDEX INDEX index_info_bm25;
SET ROLE index_info_owner;
SELECT * FROM bm25_index_info('index_info_bm25'::regclass);
RESET ROLE;

DROP TABLE index_info_docs, index_info_parts;
DROP SCHEMA index_info_schema CASCADE;
DROP ROLE index_info_member;
DROP ROLE index_info_maintainer;
DROP ROLE index_info_pg_maintain;
DROP ROLE index_info_reader;
DROP ROLE index_info_owner;
DROP EXTENSION pg_textsearch;
