#include <postgres.h>
#include <catalog/pg_type.h>
#include <executor/spi.h>
#include <funcapi.h>
#include <utils/memutils.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include "bitset.h"
#include "common.h"
#include "isomorphism.h"
#include "molecule.h"
#include "molindex.h"
#include "sachem.h"
#include "subsearch.h"
#include "lucy.h"
#include "measurement.h"
#include "java/parse.h"
#include "fingerprints/fingerprint.h"


#define SHOW_STATS              0
#define FETCH_SIZE              10000


typedef struct
{
    int32_t topN;
    bool extended;
    GraphMode graphMode;
    ChargeMode chargeMode;
    IsotopeMode isotopeMode;
    StereoMode stereoMode;
    int32_t vf2_timeout;

    BitSet resultMask;
    int32_t foundResults;

    SubstructureQueryData *queryData;
    int queryDataCount;
    int queryDataPosition;

    LucyResultSet resultSet;

#if USE_MOLECULE_INDEX == 0
    SPITupleTable *table;
#endif
    int tableRowCount;
    int tableRowPosition;

    Molecule queryMolecule;
    VF2State vf2state;

#if USE_MOLECULE_INDEX
    int32_t *arrayBuffer;
#else
    ArrayType *arrayBuffer;
#endif
    MemoryContext isomorphismContext;
    MemoryContext targetContext;

#if SHOW_STATS
    int candidateCount;
    struct timeval begin;
    int64_t prepareTime;
    int64_t indexTime;
    int64_t matchTime;
#endif

} SubstructureSearchData;


static bool initialized = false;
static bool javaInitialized = false;
static bool lucyInitialised = false;
static int indexId = -1;
static SPIPlanPtr mainQueryPlan;
static SPIPlanPtr snapshotQueryPlan;
static Lucy lucy;
static int moleculeCount;

#if USE_MOLECULE_INDEX
static void *molIndexAddress = MAP_FAILED;
static size_t molIndexSize;
static uint8_t *moleculeData;
static uint64_t *offsetData;
#endif

#if SHOW_STATS
static int dbSize;
#endif


void lucy_subsearch_init(void)
{
    if(unlikely(SPI_connect() != SPI_OK_CONNECT))
        elog(ERROR, "%s: SPI_connect() failed", __func__);


    if(unlikely(initialized == false))
    {
        if(unlikely(javaInitialized == false))
        {
            java_parse_init();
            javaInitialized = true;
        }


        /* prepare lucy */
        if(unlikely(lucyInitialised == false))
        {
            lucy_init(&lucy);
            lucyInitialised = true;
        }


        /* prepare snapshot query plan */
        if(unlikely(snapshotQueryPlan == NULL))
        {
            SPIPlanPtr plan = SPI_prepare("select id from " INDEX_TABLE, 0, NULL);

            if(unlikely(SPI_keepplan(plan) == SPI_ERROR_ARGUMENT))
                elog(ERROR, "%s: SPI_keepplan() failed", __func__);

            snapshotQueryPlan = plan;
        }


        /* prepare query plan */
        if(unlikely(mainQueryPlan == NULL))
        {
            SPIPlanPtr plan = SPI_prepare("select id, molecule from " MOLECULES_TABLE " where id = any($1)", 1, (Oid[]) { INT4ARRAYOID });

            if(unlikely(SPI_keepplan(plan) == SPI_ERROR_ARGUMENT))
                elog(ERROR, "%s: SPI_keepplan() failed", __func__);

            mainQueryPlan = plan;
        }


        initialized = true;
    }


    /* get snapshot information */
    if(unlikely(SPI_execute_plan(snapshotQueryPlan, NULL, NULL, true, 0) != SPI_OK_SELECT))
        elog(ERROR, "%s: SPI_execute_plan() failed", __func__);

    if(unlikely(SPI_processed != 1 || SPI_tuptable == NULL || SPI_tuptable->tupdesc->natts != 1))
        elog(ERROR, "%s: SPI_execute_plan() failed", __func__);

    char isNullFlag;
    int32_t dbIndexNumber = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isNullFlag);

    if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
        elog(ERROR, "%s: SPI_getbinval() failed", __func__);


    if(unlikely(dbIndexNumber != indexId))
    {
#if USE_MOLECULE_INDEX
        int indexFd = -1;
#endif

        PG_TRY();
        {
#if USE_MOLECULE_INDEX
            if(likely(molIndexAddress != MAP_FAILED))
            {
                if(unlikely(munmap(molIndexAddress, molIndexSize) < 0))
                    elog(ERROR, "%s: munmap() failed", __func__);

                molIndexAddress = MAP_FAILED;
            }


            char *indexFilePath = get_index_path(MOLECULE_INDEX_PREFIX, MOLECULE_INDEX_SUFFIX, dbIndexNumber);

            if((indexFd = open(indexFilePath, O_RDONLY, 0)) == -1)
                elog(ERROR, "%s: open() failed", __func__);

            struct stat st;

            if(fstat(indexFd, &st) < 0)
                elog(ERROR, "%s: fstat() failed", __func__);

            molIndexSize = st.st_size;

            if(unlikely((molIndexAddress = mmap(NULL, molIndexSize, PROT_READ, MAP_SHARED, indexFd, 0)) == MAP_FAILED))
                elog(ERROR, "%s: mmap() failed", __func__);

            if(unlikely(close(indexFd) < 0))
                elog(ERROR, "%s: close() failed", __func__);


            moleculeCount = *((uint64_t *) molIndexAddress);
            moleculeData = molIndexAddress + (moleculeCount + 1) * sizeof(uint64_t);
            offsetData = molIndexAddress + sizeof(uint64_t);
#endif

            if(unlikely(SPI_execute("select coalesce(max(id) + 1, 0) from " MOLECULES_TABLE, true, FETCH_ALL) != SPI_OK_SELECT))
                elog(ERROR, "%s: SPI_execute() failed", __func__);

            if(SPI_processed != 1 || SPI_tuptable == NULL || SPI_tuptable->tupdesc->natts != 1)
                elog(ERROR, "%s: SPI_execute() failed", __func__);

            char isNullFlag;
            moleculeCount = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isNullFlag));

            if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
                elog(ERROR, "%s: SPI_getbinval() failed", __func__);

            SPI_freetuptable(SPI_tuptable);

#if SHOW_STATS
            if(unlikely(SPI_execute("select count(*) from " MOLECULES_TABLE, true, FETCH_ALL) != SPI_OK_SELECT))
                elog(ERROR, "%s: SPI_execute() failed", __func__);

            if(SPI_processed != 1 || SPI_tuptable == NULL || SPI_tuptable->tupdesc->natts != 1)
                elog(ERROR, "%s: SPI_execute() failed", __func__);

            dbSize = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isNullFlag));

            if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
                elog(ERROR, "%s: SPI_getbinval() failed", __func__);

            SPI_freetuptable(SPI_tuptable);
#endif

            char *path = get_index_path(LUCY_INDEX_PREFIX, LUCY_INDEX_SUFFIX, dbIndexNumber);
            lucy_set_folder(&lucy, path);
            indexId = dbIndexNumber;
        }
        PG_CATCH();
        {
#if USE_MOLECULE_INDEX
            if(molIndexAddress != MAP_FAILED)
                munmap(molIndexAddress, molIndexSize);

            molIndexAddress = MAP_FAILED;

            if(indexFd != -1)
                close(indexFd);
#endif

            PG_RE_THROW();
        }
        PG_END_TRY();
    }

    SPI_finish();
}


PG_FUNCTION_INFO_V1(lucy_substructure_search);
Datum lucy_substructure_search(PG_FUNCTION_ARGS)
{
    if(SRF_IS_FIRSTCALL())
    {
#if SHOW_STATS
        struct timeval begin = time_get();
#endif
        lucy_subsearch_init();

        VarChar *query = PG_GETARG_VARCHAR_P(0);
        int32_t type = PG_GETARG_INT32(1);
        int32_t topN = PG_GETARG_INT32(2);
        GraphMode graphMode = PG_GETARG_INT32(3);
        ChargeMode chargeMode = PG_GETARG_INT32(4);
        IsotopeMode isotopeMode = PG_GETARG_INT32(5);
        StereoMode stereoMode = PG_GETARG_INT32(6);
        TautomerMode tautomerMode = PG_GETARG_INT32(7);
        int32_t vf2_timeout = PG_GETARG_INT32(8);

        FuncCallContext *funcctx = SRF_FIRSTCALL_INIT();

        PG_MEMCONTEXT_BEGIN(funcctx->multi_call_memory_ctx);

        SubstructureSearchData *info = (SubstructureSearchData *) palloc(sizeof(SubstructureSearchData));
        funcctx->user_fctx = info;

        info->topN = topN;
        info->graphMode = graphMode;
        info->chargeMode = chargeMode;
        info->isotopeMode = isotopeMode;
        info->stereoMode = stereoMode;
        info->vf2_timeout = vf2_timeout;

#if SHOW_STATS
        struct timeval java_begin = time_get();
#endif
        info->queryDataCount = java_parse_substructure_query(&info->queryData, VARDATA(query), VARSIZE(query) - VARHDRSZ,
                type, graphMode == GRAPH_EXACT, tautomerMode == TAUTOMER_INCHI);
#if SHOW_STATS
        struct timeval java_end = time_get();
#endif

        PG_FREE_IF_COPY(query, 0);

        info->queryDataPosition = -1;
        info->resultSet = NULL_RESULT_SET;
        info->tableRowCount = -1;
        info->tableRowPosition = -1;
        info->foundResults = 0;
#if USE_MOLECULE_INDEX == 0
        info->table = NULL;
#endif

        bitset_init_empty(&info->resultMask, moleculeCount);

        info->isomorphismContext = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
                "subsearch-lucy isomorphism context", ALLOCSET_DEFAULT_SIZES);
        info->targetContext = AllocSetContextCreate(funcctx->multi_call_memory_ctx,
                "subsearch-lucy target context", ALLOCSET_DEFAULT_SIZES);

#if USE_MOLECULE_INDEX
        info->arrayBuffer = (int32_t *) palloc(FETCH_SIZE * sizeof(int32_t));
#else
        info->arrayBuffer = (ArrayType *) palloc(FETCH_SIZE * sizeof(int32_t) + ARR_OVERHEAD_NONULLS(1));
        info->arrayBuffer->ndim = 1;
        info->arrayBuffer->dataoffset = 0;
        info->arrayBuffer->elemtype = INT4OID;
#endif

#if SHOW_STATS
        info->begin = begin;
        info->candidateCount = 0;
        info->prepareTime = time_spent(java_begin, java_end);
        info->indexTime = 0;
        info->matchTime = 0;
#endif

        PG_MEMCONTEXT_END();
    }


    bool connected = false;

    FuncCallContext *funcctx = SRF_PERCALL_SETUP();
    SubstructureSearchData *info = funcctx->user_fctx;

    Datum result;
    bool isNull = true;

    PG_TRY();
    {
        if(likely(info->topN <= 0 || info->topN != info->foundResults))
        {
            while(true)
            {
                if(unlikely(info->tableRowPosition == info->tableRowCount))
                {
#if USE_MOLECULE_INDEX == 0
                    if(info->table != NULL)
                    {
                        MemoryContextDelete(info->table->tuptabcxt);
                        info->table = NULL;
                    }
#endif

                    if(!lucy_is_open(&info->resultSet))
                    {
                        info->queryDataPosition++;

                        if(unlikely(info->queryDataPosition == info->queryDataCount))
                            break;

                        SubstructureQueryData *data = &(info->queryData[info->queryDataPosition]);

                        MemoryContextReset(info->isomorphismContext);
                        PG_MEMCONTEXT_BEGIN(info->isomorphismContext);

#if SHOW_STATS
                        struct timeval fingerprint_begin = time_get();
#endif
                        info->extended = molecule_is_extended_search_needed(data->molecule, info->chargeMode, info->isotopeMode);
                        molecule_init(&info->queryMolecule, data->molecule, data->restH, info->extended,
                                info->chargeMode != CHARGE_IGNORE, info->isotopeMode != ISOTOPE_IGNORE, info->stereoMode != STEREO_IGNORE);
                        vf2state_init(&info->vf2state, &info->queryMolecule, info->graphMode, info->chargeMode, info->isotopeMode,
                                info->stereoMode);

                        StringFingerprint fp = string_substructure_fingerprint_get_query(&info->queryMolecule);
#if SHOW_STATS
                        struct timeval fingerprint_end = time_get();
                        info->prepareTime += time_spent(fingerprint_begin, fingerprint_end);
#endif


#if SHOW_STATS
                        struct timeval search_begin = time_get();
#endif
                        info->resultSet = lucy_search(&lucy, fp);
#if SHOW_STATS
                        struct timeval search_end = time_get();
                        info->indexTime += time_spent(search_begin, search_end);
#endif

                        if(fp.data != NULL)
                            pfree(fp.data);

                        PG_MEMCONTEXT_END();
                    }

#if USE_MOLECULE_INDEX
                    int32_t *arrayData = info->arrayBuffer;
#else
                    int32_t *arrayData = (int32_t *) ARR_DATA_PTR(info->arrayBuffer);
#endif

#if SHOW_STATS
                    struct timeval get_begin = time_get();
#endif
                    size_t count = lucy_get(&lucy, &info->resultSet, arrayData, FETCH_SIZE);
#if SHOW_STATS
                    struct timeval get_end = time_get();
                    info->indexTime += time_spent(get_begin, get_end);
#endif

                    if(unlikely(count == 0))
                        continue;

#if SHOW_STATS
                    struct timeval load_begin = time_get();
#endif

#if USE_MOLECULE_INDEX == 0
                    *(ARR_DIMS(info->arrayBuffer)) = count;
                    SET_VARSIZE(info->arrayBuffer, count * sizeof(int32_t) + ARR_OVERHEAD_NONULLS(1));

                    Datum values[] = { PointerGetDatum(info->arrayBuffer)};


                    if(unlikely(!connected && SPI_connect() != SPI_OK_CONNECT))
                         elog(ERROR, "%s: SPI_connect() failed", __func__);

                    connected = true;


                    if(unlikely(SPI_execute_plan(mainQueryPlan, values, NULL, true, 0) != SPI_OK_SELECT))
                        elog(ERROR, "%s: SPI_execute_plan() failed", __func__);

                    if(unlikely(SPI_processed != count || SPI_tuptable == NULL || SPI_tuptable->tupdesc->natts != 2))
                        elog(ERROR, "%s: SPI_execute_plan() failed", __func__);

                    info->table = SPI_tuptable;
                    MemoryContextSetParent(SPI_tuptable->tuptabcxt, funcctx->multi_call_memory_ctx);
#endif

                    info->tableRowCount = count;
                    info->tableRowPosition = 0;

#if SHOW_STATS
                    struct timeval load_end = time_get();
                    info->matchTime += time_spent(load_begin, load_end);
#endif
                }

#if SHOW_STATS
                struct timeval match_begin = time_get();
#endif

#if USE_MOLECULE_INDEX
                int32_t id = info->arrayBuffer[info->tableRowPosition];
                uint8_t *molecule = moleculeData + offsetData[id];
#else
                TupleDesc tupdesc = info->table->tupdesc;
                HeapTuple tuple = info->table->vals[info->tableRowPosition];
                char isNullFlag;


                int32_t id = DatumGetInt32(SPI_getbinval(tuple, tupdesc, 1, &isNullFlag));

                if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
                    elog(ERROR, "%s: SPI_getbinval() failed", __func__);


                Datum moleculeDatum = SPI_getbinval(tuple, tupdesc, 2, &isNullFlag);

                if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
                    elog(ERROR, "%s: SPI_getbinval() failed", __func__);


                bytea *moleculeData;

                PG_MEMCONTEXT_BEGIN(info->targetContext);
                moleculeData = DatumGetByteaP(moleculeDatum);
                PG_MEMCONTEXT_END();

                uint8_t *molecule = (uint8_t *) VARDATA(moleculeData);
#endif

                bool match;

                PG_MEMCONTEXT_BEGIN(info->targetContext);
                Molecule target;
                molecule_init(&target, molecule, NULL, info->extended, info->chargeMode != CHARGE_IGNORE,
                        info->isotopeMode != ISOTOPE_IGNORE, info->stereoMode != STEREO_IGNORE);
                match = vf2state_match(&info->vf2state, &target, id, info->vf2_timeout);
                PG_MEMCONTEXT_END();
                MemoryContextReset(info->targetContext);

                info->tableRowPosition++;

#if SHOW_STATS
                info->candidateCount++;
                struct timeval match_end = time_get();
                info->matchTime += time_spent(match_begin, match_end);
#endif

                if(match)
                {
                    bitset_set(&info->resultMask, id);
                    info->foundResults++;
                    result = Int32GetDatum(id);
                    isNull = false;
                    break;
                }
            }
        }
    }
    PG_CATCH();
    {
        lucy_fail(&lucy, &info->resultSet);

        PG_RE_THROW();
    }
    PG_END_TRY();

    if(connected)
        SPI_finish();

    if(unlikely(isNull))
    {
#if SHOW_STATS
        struct timeval end = time_get();
        int64_t spentTime = time_spent(info->begin, end);
        int64_t sumTime = info->prepareTime + info->indexTime + info->matchTime;
        double scale = spentTime / 100.0;

        double specificity = 100.0 * (dbSize - info->candidateCount) / (dbSize - info->foundResults);
        double precision = 100.0 * info->foundResults / info->candidateCount;

        if(info->candidateCount == 0 && info->foundResults == 0)
            precision = 100.0;

        elog(NOTICE, "stat:   candidates      results  specificity    precision");
        elog(NOTICE, "stat: %12i %12i %11.3f%% %11.3f%%", info->candidateCount, info->foundResults, specificity, precision);
        elog(NOTICE, "time:  fingerprint        index        match          sum        total");
        elog(NOTICE, "time: %10.3fms %10.3fms %10.3fms %10.3fms %10.3fms", time_to_ms(info->prepareTime),
                time_to_ms(info->indexTime), time_to_ms(info->matchTime), time_to_ms(sumTime), time_to_ms(spentTime));
        elog(NOTICE, "time: %11.2f%% %11.2f%% %11.2f%% %11.2f%% ", info->prepareTime / scale,
                info->indexTime / scale, info->matchTime / scale, sumTime / scale);
#endif

        SRF_RETURN_DONE(funcctx);
    }
    else
    {
        SRF_RETURN_NEXT(funcctx, result);
    }
}
