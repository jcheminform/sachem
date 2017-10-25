#ifndef JAVA_H_
#define JAVA_H_

#include <stdbool.h>
#include <stdint.h>


typedef struct
{
    uint8_t *atoms;
    uint8_t *bonds;
    bool *restH;

    int atomLength;
    int bondLength;
} SubstructureQueryData;


typedef struct
{
    int16_t *counts;
    int16_t *fp;
    uint8_t *atoms;
    uint8_t *bonds;
    bool *restH;

    int fpLength;
    int atomLength;
    int bondLength;
} OrchemSubstructureQueryData;


int java_parse_substructure_query(SubstructureQueryData **data, char* query, size_t queryLength, char *type, bool tautomers);
int java_parse_orchem_substructure_query(OrchemSubstructureQueryData **data, char* query, size_t queryLength, char *type, bool tautomers);
int java_parse_orchem_similarity_query(uint64_t **data, char* query, size_t queryLength, char *type);
void java_module_init(void);
void java_module_finish(void);

#endif /* JAVA_H_ */
