-- Synthetic headers exercise layout bounds without damaging live indexes.
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION pg_textsearch_index_info_test;

-- Fields absent from old headers remain NULL, not fabricated zeroes.
SELECT * FROM index_info_test_header(5, 1, true, 8, true);
SELECT * FROM index_info_test_header(6, 1, false, 108, true);
SELECT * FROM index_info_test_header(6, 1, true, 108, true);
SELECT * FROM index_info_test_header(7, 1, false, 116, true);
SELECT * FROM index_info_test_header(8, 1, true, 120, true);
-- Fresh and header-only upgraded version 9 have distinct flags.
SELECT * FROM index_info_test_header(9, 1, false, 128, true);
SELECT * FROM index_info_test_header(9, 0, true, 128, true);
SELECT * FROM index_info_test_header(9, 2, false, 128, true);
-- Unsigned fields must not overflow SQL integer types.
SELECT * FROM index_info_test_header(9, 4294967295, false, 128, true);
SELECT * FROM index_info_test_header(4294967295, 1, true, 8, true);
-- Unknown layouts expose the version only.
SELECT * FROM index_info_test_header(10, 1, true, 8, true);
-- Reject malformed pages before reading missing fields.
SELECT * FROM index_info_test_header(9, 1, false, 0, true);
SELECT * FROM index_info_test_header(9, 1, false, 128, false);
SELECT * FROM index_info_test_header(6, 1, false, 107, true);
SELECT * FROM index_info_test_header(7, 1, false, 115, true);
SELECT * FROM index_info_test_header(8, 1, false, 119, true);
SELECT * FROM index_info_test_header(9, 1, false, 120, true);
-- A subsequent successful call verifies error cleanup.
SELECT * FROM index_info_test_header(9, 1, false, 128, true);

-- The SQL wrapper leaves all bytes of a real metapage unchanged.
CREATE TABLE index_info_unchanged (body text);
INSERT INTO index_info_unchanged VALUES ('alpha'), ('');
CREATE INDEX index_info_unchanged_idx ON index_info_unchanged USING bm25(body)
    WITH (text_config = 'english');
CREATE TEMP TABLE index_info_before AS
SELECT index_info_test_raw_page('index_info_unchanged_idx') AS page;
SELECT * FROM bm25_index_info('index_info_unchanged_idx');
SELECT page = index_info_test_raw_page('index_info_unchanged_idx')
    AS header_unchanged FROM index_info_before;
DROP TABLE index_info_before, index_info_unchanged;
DROP EXTENSION pg_textsearch_index_info_test;
DROP EXTENSION pg_textsearch;
