/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include <string.h>
#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <assert.h>

#include "sqlite-pst.hh"

#define MAX_STEPS 10000

PreparedStatement::PreparedStatement(sqlite3 *d, const char *query) {
    db = d;
    if(sqlite3_prepare_v2(db, query, (int)strlen(query), &st, NULL)
       != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

PreparedStatement::~PreparedStatement() {
    sqlite3_finalize(st);
}

void PreparedStatement::bind(int pos, const char *s) {
    bind(pos, s, strlen(s));
}

void PreparedStatement::bind(int pos, const char *s, size_t nbytes) {
    sqlite3_bind_blob(st, pos, s, (int)nbytes, SQLITE_STATIC);
}

void PreparedStatement::bind(int pos, int v) {
    sqlite3_bind_int(st, pos, v);
}

void PreparedStatement::bind64(int pos, uint64_t v) {
    sqlite3_bind_int64(st, pos, v);
}

int PreparedStatement::execute() {
    int steps_run = 0, rc = 0;
    while ((rc = sqlite3_step(st)) != SQLITE_DONE) {
        if (++steps_run > MAX_STEPS) {
            return -1;
        }
        if (rc == SQLITE_ROW) {
            // This is rather normal
        } else if (rc == SQLITE_BUSY) {
            std::cerr << "SQLITE_BUSY (retrying)" << std::endl;
        } else {
            const char *msg = sqlite3_errmsg(db);
            std::cerr << "sqlite error:  " << msg << std::endl;
            return -1;
        }
    }
    return sqlite3_changes(db);
}

bool PreparedStatement::fetch() {
    bool rv = true;
    assert(st);
    switch(sqlite3_step(st)) {
    case SQLITE_BUSY:
        throw std::runtime_error("DB was busy.");
        break;
    case SQLITE_ROW:
        break;
    case SQLITE_DONE:
        rv = false;
        break;
    default:
        throw std::runtime_error("Unhandled case.");
    }
    return rv;
}

const char *PreparedStatement::column(int x) {
    return (char*)sqlite3_column_text(st, x);
}

const void *PreparedStatement::column_blob(int x) {
    return (char*)sqlite3_column_text(st, x);
}

int PreparedStatement::column_bytes(int x) {
    return sqlite3_column_bytes(st, x);
}

int PreparedStatement::column_int(int x) {
    return sqlite3_column_int(st, x);
}

uint64_t PreparedStatement::column_int64(int x) {
    return sqlite3_column_int64(st, x);
}

void PreparedStatement::reset() {
    // The result of this is ignored as it indicates the last error
    // returned from step calls, not whether reset works.
    // http://www.sqlite.org/c3ref/reset.html
    sqlite3_reset(st);
}
