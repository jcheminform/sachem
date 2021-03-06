#include <postgres.h>
#include <access/htup.h>
#include <catalog/pg_type.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <funcapi.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <access/htup_details.h>
#include "bitset.h"
#include "common.h"
#include "heap.h"
#include "sachem.h"
#include "measurement.h"
#include "ecdk.h"


#define SHOW_STATS              0


typedef struct
{
    int32_t topN;
    float4 cutoff;

    BitSet fp;
    int queryBitCount;

    float4 bound;
    int32_t lowBucketNum;
    int32_t highBucketNum;
    int32_t currBucketNum;
    int32_t foundResults;

    SPITupleTable *table;
    int tableRowCount;
    int tableRowPosition;

    Heap heap;

#if SHOW_STATS
    struct timeval begin;
#endif

} SimilaritySearchData;


static bool initialized = false;
static bool javaInitialized = false;
static SPIPlanPtr mainQueryPlan = NULL;
static TupleDesc tupdesc = NULL;


static void ecdk_simsearch_init(void)
{
    if(unlikely(initialized == false))
    {
        if(unlikely(javaInitialized == false))
        {
            ecdk_java_init();
            javaInitialized = true;
        }


        /* prepare query plan */
        if(unlikely(mainQueryPlan == NULL))
        {
            if(unlikely(SPI_connect() != SPI_OK_CONNECT))
                elog(ERROR, "%s: SPI_connect() failed", __func__);

            SPIPlanPtr plan = SPI_prepare("select id, fp from " FINGERPRINT_TABLE " where bit_count = $1", 1, (Oid[]) { INT4OID });

            if(unlikely(SPI_keepplan(plan) == SPI_ERROR_ARGUMENT))
                elog(ERROR, "%s: SPI_keepplan() failed", __func__);

            mainQueryPlan = plan;
            SPI_finish();
        }


        /* create tuple description */
        if(unlikely(tupdesc == NULL))
        {
            TupleDesc desc = NULL;

            PG_MEMCONTEXT_BEGIN(TopMemoryContext);
            PG_TRY();
            {
                desc = CreateTemplateTupleDesc(2, false);
                TupleDescInitEntry(desc, (AttrNumber) 1, "compound", INT4OID, -1, 0);
                TupleDescInitEntry(desc, (AttrNumber) 2, "score", FLOAT4OID, -1, 0);
                desc = BlessTupleDesc(desc);
                tupdesc = desc;
            }
            PG_CATCH();
            {
                if(desc != NULL)
                    FreeTupleDesc(desc);

                PG_RE_THROW();
            }
            PG_END_TRY();
            PG_MEMCONTEXT_END();
        }


        initialized = true;
    }
}


PG_FUNCTION_INFO_V1(ecdk_similarity_search);
Datum ecdk_similarity_search(PG_FUNCTION_ARGS)
{
    if(SRF_IS_FIRSTCALL())
    {
        ecdk_simsearch_init();

#if SHOW_STATS
        struct timeval begin = time_get();
#endif

        VarChar *query = PG_GETARG_VARCHAR_P(0);
        int32_t type = PG_GETARG_INT32(1);
        float4 cutoff = PG_GETARG_FLOAT4(2);
        int32_t topN = PG_GETARG_INT32(3);

        FuncCallContext *funcctx = SRF_FIRSTCALL_INIT();
        PG_MEMCONTEXT_BEGIN(funcctx->multi_call_memory_ctx);

        SimilaritySearchData *info = (SimilaritySearchData *) palloc(sizeof(SimilaritySearchData));
        funcctx->user_fctx = info;

        info->cutoff = cutoff;
        info->topN = topN;

        uint64_t *words;
        int length = ecdk_java_parse_similarity_query(&words, VARDATA(query), VARSIZE(query) - VARHDRSZ, type);

        PG_FREE_IF_COPY(query, 0);

        if(likely(length >= 0))
        {
            bitset_init(&info->fp, words, length);
            heap_init(&info->heap);

            info->queryBitCount = bitset_cardinality(&info->fp);
            info->lowBucketNum = info->queryBitCount - 1;
            info->highBucketNum = info->queryBitCount + 1;
            info->currBucketNum = info->queryBitCount;
            info->bound = 1.0f;
            info->foundResults = 0;

            info->table = NULL;
            info->tableRowCount = -1;
            info->tableRowPosition = -1;
        }
        else
        {
            // fake values to stop searching
            info->topN = INT32_MAX;
            info->foundResults = INT32_MAX;
        }

#if SHOW_STATS
        info->begin = begin;
#endif

        PG_MEMCONTEXT_END();
    }


    bool connected = false;

    FuncCallContext *funcctx = SRF_PERCALL_SETUP();
    SimilaritySearchData *info = funcctx->user_fctx;
    Heap *heap = &info->heap;

    HeapItem result;
    bool isNull = true;


    if(likely(info->topN <= 0 || info->topN != info->foundResults))
    {
        while(true)
        {
            if(heap_size(heap) > 0 && heap_head(heap).score >= info->bound)
            {
                result = heap_head(heap);
                heap_remove(heap);
                isNull = false;
                break;
            }

            if(info->bound < info->cutoff)
                break;

            if(unlikely(info->table == NULL))
            {
                if(unlikely(!connected && SPI_connect() != SPI_OK_CONNECT))
                     elog(ERROR, "%s: SPI_connect() failed", __func__);

                connected = true;

                Datum values[] = { Int32GetDatum(info->currBucketNum)};

                if(unlikely(SPI_execute_plan(mainQueryPlan, values, NULL, true, 0) != SPI_OK_SELECT))
                    elog(ERROR, "%s: SPI_execute_plan() failed", __func__);

                if(unlikely(SPI_tuptable == NULL || SPI_tuptable->tupdesc->natts != 2))
                    elog(ERROR, "%s: SPI_execute_plan() failed", __func__);

                info->table = SPI_tuptable;
                info->tableRowCount = SPI_processed;
                info->tableRowPosition = 0;

                MemoryContextSetParent(SPI_tuptable->tuptabcxt, funcctx->multi_call_memory_ctx);
            }


            if(unlikely(info->tableRowPosition == info->tableRowCount))
            {
                MemoryContextDelete(info->table->tuptabcxt);
                info->table = NULL;

                float up = info->queryBitCount / (float) info->highBucketNum;
                float down = info->lowBucketNum / (float) info->queryBitCount;

                if(up > down)
                {
                    info->currBucketNum = info->highBucketNum;
                    info->highBucketNum++;
                }
                else
                {
                    info->currBucketNum = info->lowBucketNum;
                    info->lowBucketNum--;
                }

                if(info->lowBucketNum < 0 && info->highBucketNum > EXTFP_SIZE)
                    break;

                if(info->currBucketNum < info->queryBitCount)
                    info->bound = info->currBucketNum / (float4) info->queryBitCount;
                else
                    info->bound = info->queryBitCount / (float4) info->currBucketNum;

                continue;
            }


            TupleDesc tupdesc = info->table->tupdesc;
            HeapTuple tuple = info->table->vals[info->tableRowPosition++];
            char isNullFlag;


            Datum id = SPI_getbinval(tuple, tupdesc, 1, &isNullFlag);

            if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
                elog(ERROR, "%s: SPI_getbinval() failed", __func__);


            Datum fpDatum = SPI_getbinval(tuple, tupdesc, 2, &isNullFlag);

            if(unlikely(SPI_result == SPI_ERROR_NOATTRIBUTE || isNullFlag))
                elog(ERROR, "%s: SPI_getbinval() failed", __func__);


            BitSet fp;
            ArrayType *fpArray = DatumGetArrayTypeP(fpDatum);
            bitset_init(&fp, (uint64_t *) ARR_DATA_PTR(fpArray), ARR_DIMS(fpArray)[0]);
            int targetBitCount = bitset_cardinality(&fp);

            int bitsInCommon = bitset_and_cardinality(&info->fp, &fp);
            float4 score = (info->queryBitCount == 0 && targetBitCount == 0) ? 1 :
                    bitsInCommon / (float4) (info->queryBitCount + targetBitCount - bitsInCommon);

            if(score == info->bound)
            {
                result.id = id;
                result.score = score;
                isNull = false;
                break;
            }

            if(score >= info->cutoff)
                heap_add(&info->heap, (HeapItem) {.id = DatumGetInt32(id), .score = score});
        }
    }

    if(connected)
        SPI_finish();

    if(unlikely(isNull))
    {
#if SHOW_STATS
        struct timeval end = time_get();
        int64_t spentTime = time_spent(info->begin, end);
        elog(NOTICE, "stat: %i", info->foundResults);
        elog(NOTICE, "time: %.3fms", time_to_ms(spentTime));
#endif

        SRF_RETURN_DONE(funcctx);
    }
    else
    {
        info->foundResults++;

        char isnull[2] = {0, 0};
        Datum values[2] = {Int32GetDatum(result.id), Float4GetDatum(result.score)};
        HeapTuple tuple = heap_form_tuple(tupdesc, values, isnull);

        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
}
