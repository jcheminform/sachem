#ifndef FINGERPRINT_H__
#define FINGERPRINT_H__

#include <postgres.h>
#include <stdbool.h>
#include <stdlib.h>


#define GRAPH_SIZE              7
#define MAX_FEAT_LOGCOUNT       5
#define CIRC_SIZE               3


typedef struct Molecule Molecule;


typedef struct
{
    size_t size;
    char *data;
} StringFingerprint;


typedef struct
{
    size_t size;
    int32_t *data;
} IntegerFingerprint;


StringFingerprint string_substructure_fingerprint_get(const Molecule *molecule);
StringFingerprint string_substructure_fingerprint_get_query(const Molecule *molecule);

IntegerFingerprint integer_substructure_fingerprint_get(const Molecule *molecule);
IntegerFingerprint integer_substructure_fingerprint_get_query(const Molecule *molecule);
IntegerFingerprint integer_similarity_fingerprint_get(const Molecule *molecule);
IntegerFingerprint integer_similarity_fingerprint_get_query(const Molecule *molecule);


static inline void string_fingerprint_free(StringFingerprint fingerprint)
{
    if(fingerprint.data)
        pfree(fingerprint.data);
}


static inline void integer_fingerprint_free(IntegerFingerprint fingerprint)
{
    if(fingerprint.data)
        pfree(fingerprint.data);
}

#endif /* FINGERPRINT_H__ */
