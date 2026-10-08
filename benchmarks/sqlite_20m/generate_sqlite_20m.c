#include "sqlite3.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    WORD_COUNT = 100000,
    DOC_COUNT = 20000,
    POSTINGS_PER_WORD = 200
};

static const int64_t EXPECTED_POSTINGS =
    (int64_t)WORD_COUNT * POSTINGS_PER_WORD;
static const uint64_t EXPECTED_CNT_SUM = 3009999100ULL;

static void fail(sqlite3* db, const char* where, int rc)
{
    fprintf(stderr,
            "ERROR: %s rc=%d err=%s\n",
            where,
            rc,
            db ? sqlite3_errmsg(db) : "no database");
    exit(1);
}

static void exec_sql(sqlite3* db, const char* sql)
{
    char* error = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr,
                "ERROR: sql=%s err=%s\n",
                sql,
                error ? error : "");
        sqlite3_free(error);
        exit(1);
    }
}

static sqlite3_stmt* prepare(sqlite3* db, const char* sql)
{
    sqlite3_stmt* statement = NULL;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc != SQLITE_OK)
        fail(db, "prepare", rc);
    return statement;
}

static int file_exists(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    fclose(file);
    return 1;
}

static double elapsed_seconds(clock_t start)
{
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

static int verify_database(const char* path)
{
    sqlite3* db = NULL;
    sqlite3_stmt* statement = NULL;
    int64_t words = 0;
    int64_t docs = 0;
    int64_t postings = 0;
    uint64_t cnt_sum = 0;
    int is_without_rowid = 0;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        fail(db, "open read-only", sqlite3_errcode(db));

    statement = prepare(db, "PRAGMA quick_check;");
    if (sqlite3_step(statement) != SQLITE_ROW ||
        strcmp((const char*)sqlite3_column_text(statement, 0), "ok") != 0)
    {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        fprintf(stderr, "ERROR: PRAGMA quick_check failed\n");
        return 1;
    }
    sqlite3_finalize(statement);

    statement = prepare(
        db,
        "SELECT "
        "(SELECT COUNT(*) FROM words),"
        "(SELECT COUNT(*) FROM docs),"
        "(SELECT COUNT(*) FROM postings),"
        "(SELECT SUM(cnt) FROM postings);");
    if (sqlite3_step(statement) != SQLITE_ROW)
        fail(db, "read counts", sqlite3_errcode(db));
    words = sqlite3_column_int64(statement, 0);
    docs = sqlite3_column_int64(statement, 1);
    postings = sqlite3_column_int64(statement, 2);
    cnt_sum = (uint64_t)sqlite3_column_int64(statement, 3);
    sqlite3_finalize(statement);

    statement = prepare(
        db,
        "SELECT instr(upper(sql),'WITHOUT ROWID') "
        "FROM sqlite_master "
        "WHERE type='table' AND name='postings';");
    if (sqlite3_step(statement) != SQLITE_ROW)
        fail(db, "read postings schema", sqlite3_errcode(db));
    is_without_rowid = sqlite3_column_int(statement, 0) != 0;
    sqlite3_finalize(statement);

    printf("quick_check=ok\n");
    printf("words=%lld\n", (long long)words);
    printf("docs=%lld\n", (long long)docs);
    printf("postings=%lld\n", (long long)postings);
    printf("cnt_sum=%llu\n", (unsigned long long)cnt_sum);
    printf("postings_table=%s\n",
           is_without_rowid ? "WITHOUT ROWID" : "ordinary rowid");

    sqlite3_close(db);

    if (words != WORD_COUNT ||
        docs != DOC_COUNT ||
        postings != EXPECTED_POSTINGS ||
        cnt_sum != EXPECTED_CNT_SUM ||
        is_without_rowid)
    {
        fprintf(stderr, "ERROR: fixture validation failed\n");
        return 1;
    }

    printf("validation=ok\n");
    return 0;
}

int main(int argc, char** argv)
{
    sqlite3* db = NULL;
    sqlite3_stmt* statement = NULL;
    uint64_t expected_cnt_sum = 0;
    const clock_t started = clock();

    if (argc == 3 && strcmp(argv[1], "--verify") == 0)
        return verify_database(argv[2]);

    if (argc != 2)
    {
        fprintf(stderr,
                "Usage:\n"
                "  generate_sqlite_20m.exe OUTPUT_DB\n"
                "  generate_sqlite_20m.exe --verify DB\n");
        return 2;
    }
    if (file_exists(argv[1]))
    {
        fprintf(stderr,
                "ERROR: output already exists, remove it explicitly: %s\n",
                argv[1]);
        return 2;
    }

    printf("Creating %s\n", argv[1]);
    printf("words=%d docs=%d postings=%lld\n",
           WORD_COUNT,
           DOC_COUNT,
           (long long)EXPECTED_POSTINGS);
    fflush(stdout);

    if (sqlite3_open(argv[1], &db) != SQLITE_OK)
        fail(db, "open", sqlite3_errcode(db));

    exec_sql(db, "PRAGMA page_size=4096;");
    exec_sql(db, "PRAGMA journal_mode=OFF;");
    exec_sql(db, "PRAGMA synchronous=OFF;");
    exec_sql(db, "PRAGMA locking_mode=EXCLUSIVE;");
    exec_sql(db, "PRAGMA temp_store=MEMORY;");
    exec_sql(db, "PRAGMA cache_size=-262144;");

    exec_sql(db, "BEGIN;");
    exec_sql(db,
             "CREATE TABLE meta("
             "key TEXT PRIMARY KEY, "
             "value TEXT NOT NULL);");
    exec_sql(db,
             "INSERT INTO meta VALUES"
             "('schema_version','2'),"
             "('fixture_words','100000'),"
             "('fixture_docs','20000'),"
             "('fixture_postings','20000000');");
    exec_sql(db,
             "CREATE TABLE words("
             "word_id INTEGER PRIMARY KEY, "
             "word TEXT NOT NULL);");
    exec_sql(db,
             "CREATE TABLE docs("
             "doc_id INTEGER PRIMARY KEY, "
             "path TEXT NOT NULL, "
             "mtime_ticks INTEGER NOT NULL, "
             "size_int64 INTEGER NOT NULL, "
             "deleted INTEGER NOT NULL DEFAULT 0);");
    exec_sql(db,
             "CREATE TABLE postings("
             "word_id INTEGER NOT NULL, "
             "doc_id INTEGER NOT NULL, "
             "cnt INTEGER NOT NULL, "
             "PRIMARY KEY(word_id, doc_id));");

    statement = prepare(db, "INSERT INTO words VALUES(?,?);");
    for (int word_id = 0; word_id < WORD_COUNT; ++word_id)
    {
        char word[32];
        snprintf(word, sizeof(word), "word_%06d", word_id);
        sqlite3_bind_int(statement, 1, word_id);
        sqlite3_bind_text(statement, 2, word, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) != SQLITE_DONE)
            fail(db, "insert word", sqlite3_errcode(db));
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);
    statement = NULL;

    statement = prepare(db, "INSERT INTO docs VALUES(?,?,?,?,?);");
    for (int doc_id = 0; doc_id < DOC_COUNT; ++doc_id)
    {
        char path[80];
        snprintf(path,
                 sizeof(path),
                 "C:/sqlite_20m_fixture/doc_%05d.txt",
                 doc_id);
        sqlite3_bind_int(statement, 1, doc_id);
        sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, 1000000000LL + doc_id);
        sqlite3_bind_int64(statement, 4, 4096LL + doc_id * 17LL);
        sqlite3_bind_int(statement, 5, 0);
        if (sqlite3_step(statement) != SQLITE_DONE)
            fail(db, "insert doc", sqlite3_errcode(db));
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);
    statement = NULL;

    statement = prepare(db, "INSERT INTO postings VALUES(?,?,?);");
    for (int word_id = 0; word_id < WORD_COUNT; ++word_id)
    {
        const int max_base = DOC_COUNT - POSTINGS_PER_WORD + 1;
        const int base_doc = (int)(((int64_t)word_id * 9973) % max_base);

        for (int offset = 0; offset < POSTINGS_PER_WORD; ++offset)
        {
            const int doc_id = base_doc + offset;
            const int cnt =
                (int)(((int64_t)word_id * 17 +
                       (int64_t)doc_id * 13 +
                       offset) % 300) + 1;

            sqlite3_bind_int(statement, 1, word_id);
            sqlite3_bind_int(statement, 2, doc_id);
            sqlite3_bind_int(statement, 3, cnt);
            if (sqlite3_step(statement) != SQLITE_DONE)
                fail(db, "insert posting", sqlite3_errcode(db));
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            expected_cnt_sum += (uint64_t)cnt;
        }

        if ((word_id + 1) % 5000 == 0)
        {
            const int64_t rows =
                (int64_t)(word_id + 1) * POSTINGS_PER_WORD;
            printf("postings=%lld/%lld (%.0f%%), cpu_sec=%.1f\n",
                   (long long)rows,
                   (long long)EXPECTED_POSTINGS,
                   100.0 * (double)rows / (double)EXPECTED_POSTINGS,
                   elapsed_seconds(started));
            fflush(stdout);
        }
    }
    sqlite3_finalize(statement);
    statement = NULL;
    exec_sql(db, "COMMIT;");

    printf("Building secondary indexes...\n");
    fflush(stdout);
    exec_sql(db, "CREATE INDEX idx_postings_doc ON postings(doc_id);");
    exec_sql(db, "CREATE INDEX idx_docs_deleted ON docs(deleted);");
    exec_sql(db, "PRAGMA journal_mode=DELETE;");
    sqlite3_close(db);

    printf("Done: postings=%lld cnt_sum=%llu cpu_sec=%.1f\n",
           (long long)EXPECTED_POSTINGS,
           (unsigned long long)expected_cnt_sum,
           elapsed_seconds(started));
    return 0;
}
