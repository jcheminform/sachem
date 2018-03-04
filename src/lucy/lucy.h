#ifndef LUCY_H_
#define LUCY_H_

#include <stdbool.h>
#include <stdint.h>
#include "fingerprints/fingerprint.h"

#define NULL_RESULT_SET     ((LucyResultSet) { .hits = NULL })


typedef struct
{
    struct cfish_String *idF;
    struct cfish_String *fpF;
    struct cfish_String *folder;
    struct lucy_Schema *schema;

    struct lucy_QueryParser *qparser;
    struct lucy_IndexSearcher *searcher;

    struct lucy_Indexer *indexer;
} Lucy;


typedef struct
{
    struct lucy_Hits *hits;
} LucyResultSet;


inline bool lucy_is_open(Lucy *lucy, LucyResultSet *resultSet)
{
    return resultSet->hits != NULL;
}


void lucy_init(Lucy *lucy);
void lucy_set_folder(Lucy *lucy, const char *path);
void lucy_begin(Lucy *lucy);
void lucy_add(Lucy *lucy, int32_t id, StringFingerprint fp);
void lucy_add_index(Lucy *lucy, const char *path);
void lucy_delete(Lucy *lucy, int32_t id);
void lucy_optimize(Lucy *lucy);
void lucy_commit(Lucy *lucy);
void lucy_rollback(Lucy *lucy);
LucyResultSet lucy_search(Lucy *lucy, StringFingerprint fp, int max_results);
size_t lucy_get(Lucy *lucy, LucyResultSet *resultSet, int *buffer, size_t size);
void lucy_fail(Lucy *lucy, LucyResultSet *resultSet);
void lucy_link_directory(const char *oldPath, const char *newPath);
void lucy_delete_directory(const char *path);

#endif /* LUCY_H_ */