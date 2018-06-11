/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.c
 *	  routines to handle ModifyTable nodes.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeModifyTable.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitModifyTable - initialize the ModifyTable node
 *		ExecModifyTable		- retrieve the next tuple from the node
 *		ExecEndModifyTable	- shut down the ModifyTable node
 *		ExecReScanModifyTable - rescan the ModifyTable node
 *
 *	 NOTES
 *		Each ModifyTable node contains a list of one or more subplans,
 *		much like an Append node.  There is one subplan per result relation.
 *		The key reason for this is that in an inherited UPDATE command, each
 *		result relation could have a different schema (more or different
 *		columns) requiring a different plan tree to produce it.  In an
 *		inherited DELETE, all the subplans should produce the same output
 *		rowtype, but we might still find that different plans are appropriate
 *		for different child relations.
 *
 *		If the query specifies RETURNING, then the ModifyTable returns a
 *		RETURNING tuple after completing each row insert, update, or delete.
 *		It must be called again to continue the operation.	Without RETURNING,
 *		we just loop within the node until all the work is done, then
 *		return NULL.  This avoids useless call/return overhead.
 */

#include "postgres.h"

#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/tqual.h"

#include "access/fileam.h"
#include "access/transam.h"
#include "cdb/cdbaocsam.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbpartition.h"
#include "cdb/cdbvars.h"
#include "executor/execDML.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"


/*
 * Verify that the tuples to be produced by INSERT or UPDATE match the
 * target relation's rowtype
 *
 * We do this to guard against stale plans.  If plan invalidation is
 * functioning properly then we should never get a failure here, but better
 * safe than sorry.  Note that this is called after we have obtained lock
 * on the target rel, so the rowtype can't change underneath us.
 *
 * The plan output is represented by its targetlist, because that makes
 * handling the dropped-column case easier.
 */
static void
ExecCheckPlanOutput(Relation resultRel, List *targetList)
{
	TupleDesc	resultDesc = RelationGetDescr(resultRel);
	int			attno = 0;
	ListCell   *lc;

	foreach(lc, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Form_pg_attribute attr;

		if (tle->resjunk)
			continue;			/* ignore junk tlist items */

		if (attno >= resultDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Query has too many columns.")));
		attr = resultDesc->attrs[attno++];

		if (!attr->attisdropped)
		{
			/* Normal case: demand type match */
			if (exprType((Node *) tle->expr) != attr->atttypid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
								   format_type_be(attr->atttypid),
								   attno,
							 format_type_be(exprType((Node *) tle->expr)))));
		}
		else
		{
			/*
			 * For a dropped column, we can't check atttypid (it's likely 0).
			 * In any case the planner has most likely inserted an INT4 null.
			 * What we insist on is just *some* NULL constant.
			 */
			if (!IsA(tle->expr, Const) ||
				!((Const *) tle->expr)->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Query provides a value for a dropped column at ordinal position %d.",
								   attno)));
		}
	}
	if (attno != resultDesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
		  errmsg("table row type and query-specified row type do not match"),
				 errdetail("Query has too few columns.")));
}

/*
 * ExecProcessReturning --- evaluate a RETURNING list
 *
 * projectReturning: RETURNING projection info for current result rel
 * tupleSlot: slot holding tuple actually inserted/updated/deleted
 * planSlot: slot holding tuple returned by top subplan node
 *
 * Returns a slot holding the result tuple
 */
static TupleTableSlot *
ExecProcessReturning(ProjectionInfo *projectReturning,
					 TupleTableSlot *tupleSlot,
					 TupleTableSlot *planSlot)
{
	ExprContext *econtext = projectReturning->pi_exprContext;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous cycle.
	 */
	ResetExprContext(econtext);

	/* Make tuple and any needed join variables available to ExecProject */
	econtext->ecxt_scantuple = tupleSlot;
	econtext->ecxt_outertuple = planSlot;

	/* Compute the RETURNING expressions */
	return ExecProject(projectReturning, NULL);
}

/* ----------------------------------------------------------------
 *		ExecInsert
 *
 *		For INSERT, we have to insert the tuple into the target relation
 *		and insert appropriate tuples into the index relations.
 *
 *		Returns RETURNING result if any, otherwise NULL.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecInsert(TupleTableSlot *slot,
		   TupleTableSlot *planSlot,
		   EState *estate,
		   bool canSetTag,
		   PlanGenerator planGen,
		   bool isUpdate)
{
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	Oid			newId;
	List	   *recheckIndexes = NIL;

	bool		rel_is_heap = false;
	bool 		rel_is_aorows = false;
	bool		rel_is_aocols = false;
	bool		rel_is_external = false;
	ItemPointerData lastTid;
	Oid			tuple_oid = InvalidOid;

	/*
	 * get information on the (current) result relation
	 */
	if (estate->es_result_partitions)
	{
		resultRelInfo = slot_get_partition(slot, estate);

		/* Check whether the user provided the correct leaf part only if required */
		if (!dml_ignore_target_partition_check)
		{
			Assert(NULL != estate->es_result_partitions->part &&
					NULL != resultRelInfo->ri_RelationDesc);

			List *resultRelations = estate->es_plannedstmt->resultRelations;
			/*
			 * Only inheritance can generate multiple result relations and inheritance
			 * is not compatible with partitions. As we are in inserting in partitioned
			 * table, we should not have more than one resultRelation
			 */
			Assert(list_length(resultRelations) == 1);
			/* We only have one resultRelations entry where the user originally intended to insert */
			int rteIdxForUserRel = linitial_int(resultRelations);
			Assert (rteIdxForUserRel > 0);
			Oid userProvidedRel = InvalidOid;

			if (1 == rteIdxForUserRel)
			{
				/* Optimization for typical case */
				userProvidedRel = ((RangeTblEntry *) estate->es_plannedstmt->rtable->head->data.ptr_value)->relid;
			}
			else
			{
				userProvidedRel = getrelid(rteIdxForUserRel, estate->es_plannedstmt->rtable);
			}

			/* Error out if user provides a leaf partition that does not match with our calculated partition */
			if (userProvidedRel != estate->es_result_partitions->part->parrelid &&
				userProvidedRel != resultRelInfo->ri_RelationDesc->rd_id)
			{
				ereport(ERROR,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("Trying to insert row into wrong partition"),
						 errdetail("Expected partition: %s, provided partition: %s",
							resultRelInfo->ri_RelationDesc->rd_rel->relname.data,
							estate->es_result_relation_info->ri_RelationDesc->rd_rel->relname.data)));
			}
		}
		estate->es_result_relation_info = resultRelInfo;
	}
	else
	{
		resultRelInfo = estate->es_result_relation_info;
	}

	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	rel_is_heap = RelationIsHeap(resultRelationDesc);
	rel_is_aocols = RelationIsAoCols(resultRelationDesc);
	rel_is_aorows = RelationIsAoRows(resultRelationDesc);
	rel_is_external = RelationIsExternal(resultRelationDesc);

	/*
	 * Prepare the right kind of "insert desc".
	 */
	if (rel_is_aorows)
	{
		if (resultRelInfo->ri_aoInsertDesc == NULL)
		{
			/* Set the pre-assigned fileseg number to insert into */
			ResultRelInfoSetSegno(resultRelInfo, estate->es_result_aosegnos);

			resultRelInfo->ri_aoInsertDesc =
				appendonly_insert_init(resultRelationDesc,
									   resultRelInfo->ri_aosegno,
									   false);
		}
	}
	else if (rel_is_aocols)
	{
		if (resultRelInfo->ri_aocsInsertDesc == NULL)
		{
			ResultRelInfoSetSegno(resultRelInfo, estate->es_result_aosegnos);
			resultRelInfo->ri_aocsInsertDesc = aocs_insert_init(resultRelationDesc,
																resultRelInfo->ri_aosegno, false);
		}
	}
	else if (rel_is_external)
	{
		if (resultRelInfo->ri_extInsertDesc == NULL)
			resultRelInfo->ri_extInsertDesc = external_insert_init(resultRelationDesc);
	}

	/*
	 * If the result relation has OIDs, force the tuple's OID to zero so that
	 * heap_insert will assign a fresh OID.  Usually the OID already will be
	 * zero at this point, but there are corner cases where the plan tree can
	 * return a tuple extracted literally from some table with the same
	 * rowtype.
	 *
	 * XXX if we ever wanted to allow users to assign their own OIDs to new
	 * rows, this'd be the place to do it.  For the moment, we make a point of
	 * doing this before calling triggers, so that a user-supplied trigger
	 * could hack the OID if desired.
	 *
	 * GPDB: In PostgreSQL, here we set the Oid in the HeapTuple, which is a
	 * local copy at this point. But in GPDB, we don't materialize the tuple
	 * yet, because we might need a MemTuple or a HeapTuple depending on
	 * what kind of a table this is (or neither for an AOCS table, since
	 * aocs_insert() works directly off the slot). So we keep the Oid in a
	 * local variable for now, and only set it in the tuple just before the
	 * call to heap/appendonly/external_insert().
	 */
	if (resultRelationDesc->rd_rel->relhasoids)
	{
		tuple_oid = InvalidOid;

		/*
		 * But if this is really an UPDATE, try to preserve the old OID.
		 */
		if (isUpdate)
		{
			GenericTuple gtuple;

			gtuple = ExecFetchSlotGenericTuple(slot, false);

			if (!is_memtuple(gtuple))
				tuple_oid = HeapTupleGetOid((HeapTuple) gtuple);
			else
			{
				if (resultRelInfo->ri_aoInsertDesc)
					tuple_oid = MemTupleGetOid((MemTuple) gtuple,
											   resultRelInfo->ri_aoInsertDesc->mt_bind);
			}
		}
	}

	slot = reconstructMatchingTupleSlot(slot, resultRelInfo);

	if (rel_is_external &&
		estate->es_result_partitions &&
		estate->es_result_partitions->part->parrelid != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Insert into external partitions not supported.")));
	}

	Assert(slot != NULL);

	/* BEFORE ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_before_row &&
		!isUpdate)
	{
		slot = ExecBRInsertTriggers(estate, resultRelInfo, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* trigger might have changed tuple */
		if (resultRelationDesc->rd_rel->relhasoids)
		{
			if (TupHasHeapTuple(slot))
			{
				HeapTuple trigtup = TupGetHeapTuple(slot);

				tuple_oid = HeapTupleGetOid(trigtup);
			}
		}
	}

	/* INSTEAD OF ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_instead_row)
	{
		slot = ExecIRInsertTriggers(estate, resultRelInfo, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		newId = InvalidOid;
	}
	else
	{
		/*
		 * Check the constraints of the tuple
		 */
		if (resultRelationDesc->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);

		/*
		 * insert the tuple
		 *
		 * Note: heap_insert returns the tid (location) of the new tuple in the
		 * t_self field.
		 *
		 * NOTE: for append-only relations we use the append-only access methods.
		 */
		if (rel_is_aorows)
		{
			MemTuple	mtuple;

			if (resultRelInfo->ri_aoInsertDesc == NULL)
			{
				/* Set the pre-assigned fileseg number to insert into */
				ResultRelInfoSetSegno(resultRelInfo, estate->es_result_aosegnos);

				resultRelInfo->ri_aoInsertDesc =
					appendonly_insert_init(resultRelationDesc,
										   resultRelInfo->ri_aosegno,
										   false);
			}

			mtuple = ExecFetchSlotMemTuple(slot, false);
			newId = appendonly_insert(resultRelInfo->ri_aoInsertDesc, mtuple, tuple_oid, (AOTupleId *) &lastTid);
			(resultRelInfo->ri_aoprocessed)++;
		}
		else if (rel_is_aocols)
		{
			if (resultRelInfo->ri_aocsInsertDesc == NULL)
			{
				ResultRelInfoSetSegno(resultRelInfo, estate->es_result_aosegnos);
				resultRelInfo->ri_aocsInsertDesc = aocs_insert_init(resultRelationDesc, 
																	resultRelInfo->ri_aosegno, false);
			}

			newId = aocs_insert(resultRelInfo->ri_aocsInsertDesc, slot);
			lastTid = *slot_get_ctid(slot);
			(resultRelInfo->ri_aoprocessed)++;
		}
		else if (rel_is_external)
		{
			/* Writable external table */
			HeapTuple tuple;

			if (resultRelInfo->ri_extInsertDesc == NULL)
				resultRelInfo->ri_extInsertDesc = external_insert_init(resultRelationDesc);

			/*
			 * get the heap tuple out of the tuple table slot, making sure we have a
			 * writable copy. (external_insert() can scribble on the tuple)
			 */
			tuple = ExecMaterializeSlot(slot);
			if (resultRelationDesc->rd_rel->relhasoids)
				HeapTupleSetOid(tuple, tuple_oid);

			newId = external_insert(resultRelInfo->ri_extInsertDesc, tuple);
			ItemPointerSetInvalid(&lastTid);
		}
		else
		{
			HeapTuple tuple;

			Insist(rel_is_heap);

			/*
			 * get the heap tuple out of the tuple table slot, making sure we have a
			 * writable copy. (heap_insert() will scribble on the tuple)
			 */
			tuple = ExecMaterializeSlot(slot);
			if (resultRelationDesc->rd_rel->relhasoids)
				HeapTupleSetOid(tuple, tuple_oid);

			newId = heap_insert(resultRelationDesc,
								tuple,
								estate->es_output_cid, 0, NULL,
								GetCurrentTransactionId());
			lastTid = tuple->t_self;
		}

		/*
		 * insert index entries for tuple
		 */
		if (resultRelInfo->ri_NumIndices > 0)
			recheckIndexes = ExecInsertIndexTuples(slot, &lastTid,
												   estate);
	}
	if (canSetTag)
	{
		(estate->es_processed)++;
		estate->es_lastoid = newId;
		setLastTid(&lastTid);
	}

	slot->tts_tableOid = RelationGetRelid(resultRelationDesc);

	/* AFTER ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_after_row &&
		!isUpdate)
	{
		HeapTuple tuple = ExecMaterializeSlot(slot);

		/*
		 * GPDB_90_MERGE_FIXME: do we really need this? It seems like if ORCA
		 * can get us here, we shouldn't worry about it. Commenting to get
		 * alter_table working with ORCA.
		 *
		Assert(planGen == PLANGEN_PLANNER);
		 */

		ExecARInsertTriggers(estate, resultRelInfo, tuple, recheckIndexes);
	}

	list_free(recheckIndexes);

	/* Process RETURNING if present */
	if (resultRelInfo->ri_projectReturning)
		return ExecProcessReturning(resultRelInfo->ri_projectReturning,
									slot, planSlot);

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecDelete
 *
 *		DELETE is like UPDATE, except that we delete the tuple and no
 *		index modifications are needed.
 *
 *		When deleting from a table, tupleid identifies the tuple to
 *		delete and oldtuple is NULL.  When deleting from a view,
 *		oldtuple is passed to the INSTEAD OF triggers and identifies
 *		what to delete, and tupleid is invalid.
 *
 *		In GPDB, DELETE can be part of an update operation when
 *		there is a preceding SplitUpdate node. 
 *
 *		Returns RETURNING result if any, otherwise NULL.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecDelete(ItemPointer tupleid,
		   HeapTupleHeader oldtuple,
		   TupleTableSlot *planSlot,
		   EPQState *epqstate,
		   EState *estate,
		   bool canSetTag,
		   PlanGenerator planGen,
		   bool isUpdate)
{
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	HTSU_Result result;
	ItemPointerData update_ctid;
	TransactionId update_xmax = InvalidTransactionId;

	/*
	 * get information on the (current) result relation
	 */
	if (estate->es_result_partitions && planGen == PLANGEN_OPTIMIZER)
	{
		Assert(estate->es_result_partitions->part->parrelid);

#ifdef USE_ASSERT_CHECKING
		Oid parent = estate->es_result_partitions->part->parrelid;
#endif

		/* Obtain part for current tuple. */
		resultRelInfo = slot_get_partition(planSlot, estate);
		estate->es_result_relation_info = resultRelInfo;

#ifdef USE_ASSERT_CHECKING
		Oid part = RelationGetRelid(resultRelInfo->ri_RelationDesc);
#endif

		Assert(parent != part);
	}
	else
	{
		resultRelInfo = estate->es_result_relation_info;
	}
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	if (planGen == PLANGEN_PLANNER)
	{
		/* BEFORE ROW DELETE Triggers */
		if (resultRelInfo->ri_TrigDesc &&
			resultRelInfo->ri_TrigDesc->trig_delete_before_row)
		{
			bool		dodelete;

			dodelete = ExecBRDeleteTriggers(estate, epqstate, resultRelInfo,
											tupleid);

			if (!dodelete)			/* "do nothing" */
				return NULL;
		}
	}

	bool isHeapTable = RelationIsHeap(resultRelationDesc);
	bool isAORowsTable = RelationIsAoRows(resultRelationDesc);
	bool isAOColsTable = RelationIsAoCols(resultRelationDesc);
	bool isExternalTable = RelationIsExternal(resultRelationDesc);

	if (isExternalTable && estate->es_result_partitions && 
		estate->es_result_partitions->part->parrelid != 0)
	{
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("Delete from external partitions not supported.")));			
	}

	/* INSTEAD OF ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_instead_row)
	{
		HeapTupleData tuple;
		bool		dodelete;

		Assert(oldtuple != NULL);
		tuple.t_data = oldtuple;
		tuple.t_len = HeapTupleHeaderGetDatumLength(oldtuple);
		ItemPointerSetInvalid(&(tuple.t_self));

		dodelete = ExecIRDeleteTriggers(estate, resultRelInfo, &tuple);

		if (!dodelete)			/* "do nothing" */
			return NULL;
	}
	else
	{
		/*
		 * delete the tuple
		 *
		 * Note: if es_crosscheck_snapshot isn't InvalidSnapshot, we check
		 * that the row to be deleted is visible to that snapshot, and throw a
		 * can't-serialize error if not. This is a special-case behavior
		 * needed for referential integrity updates in transaction-snapshot
		 * mode transactions.
		 */
ldelete:;
		if (isHeapTable)
		{
			result = heap_delete(resultRelationDesc, tupleid,
								 &update_ctid, &update_xmax,
								 estate->es_output_cid,
								 estate->es_crosscheck_snapshot,
								 true /* wait for commit */ );
		}
		else if (isAORowsTable)
		{
			if (IsolationUsesXactSnapshot())
			{
				if (!isUpdate)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("Deletes on append-only tables are not supported in serializable transactions.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("Updates on append-only tables are not supported in serializable transactions.")));
			}

			if (resultRelInfo->ri_deleteDesc == NULL)
			{
				resultRelInfo->ri_deleteDesc = 
					appendonly_delete_init(resultRelationDesc, GetActiveSnapshot());
			}

			AOTupleId *aoTupleId = (AOTupleId *) tupleid;
			result = appendonly_delete(resultRelInfo->ri_deleteDesc, aoTupleId);
		} 
		else if (isAOColsTable)
		{
			if (IsolationUsesXactSnapshot())
			{
				if (!isUpdate)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("Deletes on append-only tables are not supported in serializable transactions.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("Updates on append-only tables are not supported in serializable transactions.")));
			}

			if (resultRelInfo->ri_deleteDesc == NULL)
			{
				resultRelInfo->ri_deleteDesc = 
					aocs_delete_init(resultRelationDesc);
			}

			AOTupleId *aoTupleId = (AOTupleId *) tupleid;
			result = aocs_delete(resultRelInfo->ri_deleteDesc, aoTupleId);
		}
		else
		{
			Insist(0);
		}
		switch (result)
		{
			case HeapTupleSelfUpdated:
				/* already deleted by self; nothing to do */

				/*-------
				 * In an scenario in which R(a,b) and S(a,b) have
				 *        R               S
				 *    ________         ________
				 *     (1, 1)           (1, 2)
				 *                      (1, 7)
				 *
				 *  An update query such as:
				 *   UPDATE R SET a = S.b  FROM S WHERE R.b = S.a;
				 *
				 *  will have an non-deterministic output. The tuple in R
				 * can be updated to (2,1) or (7,1).
				 * Since the introduction of SplitUpdate, these queries will
				 * send multiple requests to delete the same tuple. Therefore,
				 * in order to avoid a non-deterministic output,
				 * an error is reported in such scenario.
				 *
				 * ORCA requires this check as it does not support multiple
				 * updates to a row by the same query
				 *-------
				*/
				if (isUpdate)
				{
					ereport(ERROR,
							(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION ),
							 errmsg("multiple updates to a row by the same query is not allowed")));
				}
				return NULL;

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				if (!ItemPointerEquals(tupleid, &update_ctid))
				{
					TupleTableSlot *epqslot;

					epqslot = EvalPlanQual(estate,
										   epqstate,
										   resultRelationDesc,
										   resultRelInfo->ri_RangeTableIndex,
										   &update_ctid,
										   update_xmax);
					if (!TupIsNull(epqslot))
					{
						*tupleid = update_ctid;
						goto ldelete;
					}
				}
				/* tuple already deleted; nothing to do */
				return NULL;

			default:
				elog(ERROR, "unrecognized heap_delete status: %u", result);
				return NULL;
		}

		/*
		 * Note: Normally one would think that we have to delete index tuples
		 * associated with the heap tuple now...
		 *
		 * ... but in POSTGRES, we have no need to do this because VACUUM will
		 * take care of it later.  We can't delete index tuples immediately
		 * anyway, since the tuple is still visible to other transactions.
		 */
	}

	if (canSetTag)
		(estate->es_processed)++;

	if (!isUpdate)
	{
		/*
		 * To notify master if tuples deleted or not, to update mod_count.
		 */
		(resultRelInfo->ri_aoprocessed)++;
	}

	if (!(isAORowsTable || isAOColsTable) && planGen == PLANGEN_PLANNER)
	{
		/* AFTER ROW DELETE Triggers */
		ExecARDeleteTriggers(estate, resultRelInfo, tupleid);
	}

	/* Process RETURNING if present */
	if (resultRelInfo->ri_projectReturning)
	{
		/*
		 * We have to put the target tuple into a slot, which means first we
		 * gotta fetch it.	We can use the trigger tuple slot.
		 */
		TupleTableSlot *slot = estate->es_trig_tuple_slot;
		TupleTableSlot *rslot;
		HeapTupleData deltuple;
		Buffer		delbuffer;

		// GPDB_91_MERGE_FIXME: What if it's an AO table?

		if (oldtuple != NULL)
		{
			deltuple.t_data = oldtuple;
			deltuple.t_len = HeapTupleHeaderGetDatumLength(oldtuple);
			ItemPointerSetInvalid(&(deltuple.t_self));
			delbuffer = InvalidBuffer;
		}
		else
		{
			deltuple.t_self = *tupleid;
			if (!heap_fetch(resultRelationDesc, SnapshotAny,
							&deltuple, &delbuffer, false, NULL))
				elog(ERROR, "failed to fetch deleted tuple for DELETE RETURNING");
		}

		if (slot->tts_tupleDescriptor != RelationGetDescr(resultRelationDesc))
			ExecSetSlotDescriptor(slot, RelationGetDescr(resultRelationDesc));
		ExecStoreHeapTuple(&deltuple, slot, InvalidBuffer, false);

		rslot = ExecProcessReturning(resultRelInfo->ri_projectReturning,
									 slot, planSlot);

		ExecClearTuple(slot);
		if (BufferIsValid(delbuffer))
			ReleaseBuffer(delbuffer);

		return rslot;
	}

	return NULL;
}

/*
 * Check if the tuple being updated will stay in the same part and throw ERROR
 * if not.  This check is especially necessary for default partition that has
 * no constraint on it.  partslot is the tuple being updated, and
 * resultRelInfo is the target relation of this update.  Call this only
 * estate has valid es_result_partitions.
 */
static void
checkPartitionUpdate(EState *estate, TupleTableSlot *partslot,
					 ResultRelInfo *resultRelInfo)
{
	Relation	resultRelationDesc = resultRelInfo->ri_RelationDesc;
	AttrNumber	max_attr;
	Datum	   *values = NULL;
	bool	   *nulls = NULL;
	TupleDesc	tupdesc = NULL;
	Oid			parentRelid;
	Oid			targetid;

	Assert(estate->es_partition_state != NULL &&
		   estate->es_partition_state->accessMethods != NULL);
	if (!estate->es_partition_state->accessMethods->part_cxt)
		estate->es_partition_state->accessMethods->part_cxt =
			GetPerTupleExprContext(estate)->ecxt_per_tuple_memory;

	Assert(PointerIsValid(estate->es_result_partitions));

	/*
	 * As opposed to INSERT, resultRelation here is the same child part
	 * as scan origin.  However, the partition selection is done with the
	 * parent partition's attribute numbers, so if this result (child) part
	 * has physically-different attribute numbers due to dropped columns,
	 * we should map the child attribute numbers to the parent's attribute
	 * numbers to perform the partition selection.
	 * EState doesn't have the parent relation information at the moment,
	 * so we have to do a hard job here by opening it and compare the
	 * tuple descriptors.  If we find we need to map attribute numbers,
	 * max_partition_attr could also be bogus for this child part,
	 * so we end up materializing the whole columns using slot_getallattrs().
	 * The purpose of this code is just to prevent the tuple from
	 * incorrectly staying in default partition that has no constraint
	 * (parts with constraint will throw an error if the tuple is changing
	 * partition keys to out of part value anyway.)  It's a bit overkill
	 * to do this complicated logic just for this purpose, which is necessary
	 * with our current partitioning design, but I hope some day we can
	 * change this so that we disallow phyisically-different tuple descriptor
	 * across partition.
	 */
	parentRelid = estate->es_result_partitions->part->parrelid;

	/*
	 * I don't believe this is the case currently, but we check the parent relid
	 * in case the updating partition has changed since the last time we opened it.
	 */
	if (resultRelInfo->ri_PartitionParent &&
		parentRelid != RelationGetRelid(resultRelInfo->ri_PartitionParent))
	{
		resultRelInfo->ri_PartCheckTupDescMatch = 0;
		if (resultRelInfo->ri_PartCheckMap != NULL)
			pfree(resultRelInfo->ri_PartCheckMap);
		if (resultRelInfo->ri_PartitionParent)
			relation_close(resultRelInfo->ri_PartitionParent, AccessShareLock);
	}

	/*
	 * Check this at the first pass only to avoid repeated catalog access.
	 */
	if (resultRelInfo->ri_PartCheckTupDescMatch == 0 &&
		parentRelid != RelationGetRelid(resultRelInfo->ri_RelationDesc))
	{
		Relation	parentRel;
		TupleDesc	resultTupdesc, parentTupdesc;

		/*
		 * We are on a child part, let's see the tuple descriptor looks like
		 * the parent's one.  Probably this won't cause deadlock because
		 * DML should have opened the parent table with appropriate lock.
		 */
		parentRel = relation_open(parentRelid, AccessShareLock);
		resultTupdesc = RelationGetDescr(resultRelationDesc);
		parentTupdesc = RelationGetDescr(parentRel);
		if (!equalTupleDescs(resultTupdesc, parentTupdesc, false))
		{
			AttrMap		   *map;
			MemoryContext	oldcontext;

			/* Tuple looks different.  Construct attribute mapping. */
			oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
			map_part_attrs(resultRelationDesc, parentRel, &map, true);
			MemoryContextSwitchTo(oldcontext);

			/* And save it for later use. */
			resultRelInfo->ri_PartCheckMap = map;

			resultRelInfo->ri_PartCheckTupDescMatch = -1;
		}
		else
			resultRelInfo->ri_PartCheckTupDescMatch = 1;

		resultRelInfo->ri_PartitionParent = parentRel;
		/* parentRel will be closed as part of ResultRelInfo cleanup */
	}

	if (resultRelInfo->ri_PartCheckMap != NULL)
	{
		Datum	   *parent_values;
		bool	   *parent_nulls;
		Relation	parentRel = resultRelInfo->ri_PartitionParent;
		TupleDesc	parentTupdesc;
		AttrMap	   *map;

		Assert(parentRel != NULL);
		parentTupdesc = RelationGetDescr(parentRel);

		/*
		 * We need to map the attribute numbers to parent's one, to
		 * select the would-be destination relation, since all partition
		 * rules are based on the parent relation's tuple descriptor.
		 * max_partition_attr can be bogus as well, so don't use it.
		 */
		slot_getallattrs(partslot);
		values = slot_get_values(partslot);
		nulls = slot_get_isnull(partslot);
		parent_values = palloc(parentTupdesc->natts * sizeof(Datum));
		parent_nulls = palloc0(parentTupdesc->natts * sizeof(bool));

		map = resultRelInfo->ri_PartCheckMap;
		reconstructTupleValues(map, values, nulls, partslot->tts_tupleDescriptor->natts,
							   parent_values, parent_nulls, parentTupdesc->natts);

		/* Now we have values/nulls in parent's view. */
		values = parent_values;
		nulls = parent_nulls;
		tupdesc = RelationGetDescr(parentRel);
	}
	else
	{
		/*
		 * map == NULL means we can just fetch values/nulls from the
		 * current slot.
		 */
		Assert(nulls == NULL && tupdesc == NULL);
		max_attr = estate->es_partition_state->max_partition_attr;
		slot_getsomeattrs(partslot, max_attr);
		/* values/nulls pointing to partslot's array. */
		values = slot_get_values(partslot);
		nulls = slot_get_isnull(partslot);
		tupdesc = partslot->tts_tupleDescriptor;
	}

	/* And select the destination relation that this tuple would go to. */
	targetid = selectPartition(estate->es_result_partitions, values,
							   nulls, tupdesc,
							   estate->es_partition_state->accessMethods);

	/* Free up if we allocated mapped attributes. */
	if (values != slot_get_values(partslot))
	{
		Assert(nulls != slot_get_isnull(partslot));
		pfree(values);
		pfree(nulls);
	}

	if (!OidIsValid(targetid))
		ereport(ERROR,
				(errcode(ERRCODE_NO_PARTITION_FOR_PARTITIONING_KEY),
				 errmsg("no partition for partitioning key")));

	if (RelationGetRelid(resultRelationDesc) != targetid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("moving tuple from partition \"%s\" to "
						"partition \"%s\" not supported",
						get_rel_name(RelationGetRelid(resultRelationDesc)),
						get_rel_name(targetid))));
}

/* ----------------------------------------------------------------
 *		ExecUpdate
 *
 *		note: we can't run UPDATE queries with transactions
 *		off because UPDATEs are actually INSERTs and our
 *		scan will mistakenly loop forever, updating the tuple
 *		it just inserted..	This should be fixed but until it
 *		is, we don't want to get stuck in an infinite loop
 *		which corrupts your database..
 *
 *		When updating a table, tupleid identifies the tuple to
 *		update and oldtuple is NULL.  When updating a view, oldtuple
 *		is passed to the INSTEAD OF triggers and identifies what to
 *		update, and tupleid is invalid.
 *
 *		Returns RETURNING result if any, otherwise NULL.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecUpdate(ItemPointer tupleid,
		   HeapTupleHeader oldtuple,
		   TupleTableSlot *slot,
		   TupleTableSlot *planSlot,
		   EPQState *epqstate,
		   EState *estate,
		   bool canSetTag)
{
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	HTSU_Result result;
	ItemPointerData update_ctid;
	TransactionId update_xmax = InvalidTransactionId;
	List	   *recheckIndexes = NIL;
	ItemPointerData lastTid;
	bool		wasHotUpdate;

	/*
	 * abort the operation if not running transactions
	 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "cannot UPDATE during bootstrap");

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	bool		rel_is_heap = RelationIsHeap(resultRelationDesc);
	bool 		rel_is_aorows = RelationIsAoRows(resultRelationDesc);
	bool		rel_is_aocols = RelationIsAoCols(resultRelationDesc);
	bool		rel_is_external = RelationIsExternal(resultRelationDesc);

	if (rel_is_external &&
		estate->es_result_partitions &&
		estate->es_result_partitions->part->parrelid != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Update external partitions not supported.")));
	}

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */

	if (rel_is_aorows || rel_is_aocols)
	{
		/*
		 * It is necessary to reconstruct a logically compatible tuple to
		 * a physically compatible tuple.  The slot's tuple descriptor comes
		 * from the projection target list, which doesn't indicate dropped
		 * columns, and MemTuple cannot deal with cases without converting
		 * the target list back into the original relation's tuple desc.
		 */
		slot = reconstructMatchingTupleSlot(slot, resultRelInfo);
	}

	/* see if this update would move the tuple to a different partition */
	if (estate->es_result_partitions)
		checkPartitionUpdate(estate, slot, resultRelInfo);

	/* BEFORE ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_before_row)
	{
		slot = ExecBRUpdateTriggers(estate, epqstate, resultRelInfo,
									tupleid, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;
	}

	/* INSTEAD OF ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_instead_row)
	{
		HeapTupleData oldtup;

		Assert(oldtuple != NULL);
		oldtup.t_data = oldtuple;
		oldtup.t_len = HeapTupleHeaderGetDatumLength(oldtuple);
		ItemPointerSetInvalid(&(oldtup.t_self));

		slot = ExecIRUpdateTriggers(estate, resultRelInfo,
									&oldtup, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* trigger might have changed tuple */
	}
	else
	{
		/*
		 * Check the constraints of the tuple
		 *
		 * If we generate a new candidate tuple after EvalPlanQual testing, we
		 * must loop back here and recheck constraints.  (We don't need to
		 * redo triggers, however.	If there are any BEFORE triggers then
		 * trigger.c will have done heap_lock_tuple to lock the correct tuple,
		 * so there's no need to do them again.)
		 */
lreplace:;
		if (resultRelationDesc->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate);

		/*
		 * replace the heap tuple
		 *
		 * Note: if es_crosscheck_snapshot isn't InvalidSnapshot, we check
		 * that the row to be updated is visible to that snapshot, and throw a
		 * can't-serialize error if not. This is a special-case behavior
		 * needed for referential integrity updates in transaction-snapshot
		 * mode transactions.
		 */
		if (rel_is_heap)
		{
			HeapTuple tuple;

			tuple = ExecMaterializeSlot(slot);

			result = heap_update(resultRelationDesc, tupleid, tuple,
							 &update_ctid, &update_xmax,
							 estate->es_output_cid,
							 estate->es_crosscheck_snapshot,
							 true /* wait for commit */ );

			lastTid = tuple->t_self;
			wasHotUpdate = HeapTupleIsHeapOnly(tuple) != 0;
		}
		else if (rel_is_aorows)
		{
			MemTuple mtuple;

			if (IsolationUsesXactSnapshot())
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Updates on append-only tables are not supported in serializable transactions.")));
			}

			if (resultRelInfo->ri_updateDesc == NULL)
			{
				ResultRelInfoSetSegno(resultRelInfo, estate->es_result_aosegnos);
				resultRelInfo->ri_updateDesc = (AppendOnlyUpdateDesc)
					appendonly_update_init(resultRelationDesc, GetActiveSnapshot(), resultRelInfo->ri_aosegno);
			}

			mtuple = ExecFetchSlotMemTuple(slot, false);

			result = appendonly_update(resultRelInfo->ri_updateDesc,
									   mtuple, (AOTupleId *) tupleid, (AOTupleId *) &lastTid);
			(resultRelInfo->ri_aoprocessed)++;
			wasHotUpdate = false;
		}
		else if (rel_is_aocols)
		{
			if (IsolationUsesXactSnapshot())
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Updates on append-only tables are not supported in serializable transactions.")));
			}

			if (resultRelInfo->ri_updateDesc == NULL)
			{
				ResultRelInfoSetSegno(resultRelInfo, estate->es_result_aosegnos);
				resultRelInfo->ri_updateDesc = (AppendOnlyUpdateDesc)
					aocs_update_init(resultRelationDesc, resultRelInfo->ri_aosegno);
			}
			result = aocs_update(resultRelInfo->ri_updateDesc,
								 slot, (AOTupleId *) tupleid, (AOTupleId *) &lastTid);
			(resultRelInfo->ri_aoprocessed)++;
			wasHotUpdate = false;
		}
		else
		{
			elog(ERROR, "invalid relation type");
		}

		switch (result)
		{
			case HeapTupleSelfUpdated:
				/* already deleted by self; nothing to do */
				return NULL;

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				if (!ItemPointerEquals(tupleid, &update_ctid))
				{
					TupleTableSlot *epqslot;

					epqslot = EvalPlanQual(estate,
										   epqstate,
										   resultRelationDesc,
										   resultRelInfo->ri_RangeTableIndex,
										   &update_ctid,
										   update_xmax);
					if (!TupIsNull(epqslot))
					{
						*tupleid = update_ctid;
						slot = ExecFilterJunk(resultRelInfo->ri_junkFilter, epqslot);
						goto lreplace;
					}
				}
				/* tuple already deleted; nothing to do */
				return NULL;

			default:
				elog(ERROR, "unrecognized heap_update status: %u", result);
				return NULL;
		}

		/*
		 * Note: instead of having to update the old index tuples associated
		 * with the heap tuple, all we do is form and insert new index tuples.
		 * This is because UPDATEs are actually DELETEs and INSERTs, and index
		 * tuple deletion is done later by VACUUM (see notes in ExecDelete).
		 * All we do here is insert new index tuples.  -cim 9/27/89
		 */

		/*
		 * insert index entries for tuple
		 *
		 * Note: heap_update returns the tid (location) of the new tuple in
		 * the t_self field.
		 *
		 * If it's a HOT update, we mustn't insert new index entries.
		 */
		if (resultRelInfo->ri_NumIndices > 0 && !wasHotUpdate)
			recheckIndexes = ExecInsertIndexTuples(slot, &lastTid,
												   estate);
	}

	if (canSetTag)
		(estate->es_processed)++;

	/* AFTER ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_after_row &&
		rel_is_heap)
	{
		HeapTuple tuple = ExecMaterializeSlot(slot);

		ExecARUpdateTriggers(estate, resultRelInfo, tupleid, tuple,
						 recheckIndexes);
	}

	list_free(recheckIndexes);

	/* Process RETURNING if present */
	if (resultRelInfo->ri_projectReturning)
		return ExecProcessReturning(resultRelInfo->ri_projectReturning,
									slot, planSlot);

	return NULL;
}


/*
 * Process BEFORE EACH STATEMENT triggers
 */
static void
fireBSTriggers(ModifyTableState *node)
{
	switch (node->operation)
	{
		case CMD_INSERT:
			ExecBSInsertTriggers(node->ps.state, node->resultRelInfo);
			break;
		case CMD_UPDATE:
			ExecBSUpdateTriggers(node->ps.state, node->resultRelInfo);
			break;
		case CMD_DELETE:
			ExecBSDeleteTriggers(node->ps.state, node->resultRelInfo);
			break;
		default:
			elog(ERROR, "unknown operation");
			break;
	}
}

/*
 * Process AFTER EACH STATEMENT triggers
 */
static void
fireASTriggers(ModifyTableState *node)
{
	switch (node->operation)
	{
		case CMD_INSERT:
			ExecASInsertTriggers(node->ps.state, node->resultRelInfo);
			break;
		case CMD_UPDATE:
			ExecASUpdateTriggers(node->ps.state, node->resultRelInfo);
			break;
		case CMD_DELETE:
			ExecASDeleteTriggers(node->ps.state, node->resultRelInfo);
			break;
		default:
			elog(ERROR, "unknown operation");
			break;
	}
}


/* ----------------------------------------------------------------
 *	   ExecModifyTable
 *
 *		Perform table modifications as required, and return RETURNING results
 *		if needed.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecModifyTable(ModifyTableState *node)
{
	EState	   *estate = node->ps.state;
	CmdType		operation = node->operation;
	ResultRelInfo *saved_resultRelInfo;
	ResultRelInfo *resultRelInfo;
	PlanState  *subplanstate;
	JunkFilter *junkfilter;
	TupleTableSlot *slot;
	TupleTableSlot *planSlot;
	ItemPointer tupleid = NULL;
	ItemPointerData tuple_ctid;
	HeapTupleHeader oldtuple = NULL;

	/*
	 * If we've already completed processing, don't try to do more.  We need
	 * this test because ExecPostprocessPlan might call us an extra time, and
	 * our subplan's nodes aren't necessarily robust against being called
	 * extra times.
	 */
	if (node->mt_done)
		return NULL;

	if (Gp_role == GP_ROLE_EXECUTE && !Gp_is_writer)
	{
		elog(ERROR, "INSERT/UPDATE/DELETE must be executed by a writer segworker group");
	}

	/*
	 * On first call, fire BEFORE STATEMENT triggers before proceeding.
	 */
	if (node->fireBSTriggers)
	{
		fireBSTriggers(node);
		node->fireBSTriggers = false;
	}

	/* Preload local variables */
	resultRelInfo = node->resultRelInfo + node->mt_whichplan;
	subplanstate = node->mt_plans[node->mt_whichplan];
	junkfilter = resultRelInfo->ri_junkFilter;

	/*
	 * es_result_relation_info must point to the currently active result
	 * relation while we are within this ModifyTable node.	Even though
	 * ModifyTable nodes can't be nested statically, they can be nested
	 * dynamically (since our subplan could include a reference to a modifying
	 * CTE).  So we have to save and restore the caller's value.
	 */
	saved_resultRelInfo = estate->es_result_relation_info;

	estate->es_result_relation_info = resultRelInfo;

	/*
	 * Fetch rows from subplan(s), and execute the required table modification
	 * for each row.
	 */
	for (;;)
	{
		/*
		 * Reset the per-output-tuple exprcontext.	This is needed because
		 * triggers expect to use that context as workspace.  It's a bit ugly
		 * to do this below the top level of the plan, however.  We might need
		 * to rethink this later.
		 */
		ResetPerTupleExprContext(estate);

		planSlot = ExecProcNode(subplanstate);

		if (TupIsNull(planSlot))
		{
			/* advance to next subplan if any */
			node->mt_whichplan++;
			if (node->mt_whichplan < node->mt_nplans)
			{
				resultRelInfo++;
				subplanstate = node->mt_plans[node->mt_whichplan];
				junkfilter = resultRelInfo->ri_junkFilter;
				estate->es_result_relation_info = resultRelInfo;
				EvalPlanQualSetPlan(&node->mt_epqstate, subplanstate->plan,
									node->mt_arowmarks[node->mt_whichplan]);
				continue;
			}
			else
				break;
		}

		EvalPlanQualSetSlot(&node->mt_epqstate, planSlot);
		slot = planSlot;

		if (junkfilter != NULL)
		{
			/*
			 * extract the 'ctid' or 'wholerow' junk attribute.
			 */
			if (operation == CMD_UPDATE || operation == CMD_DELETE)
			{
				Datum		datum;
				bool		isNull;

				if (resultRelInfo->ri_RelationDesc->rd_rel->relkind == RELKIND_RELATION)
				{
					datum = ExecGetJunkAttribute(slot,
												 junkfilter->jf_junkAttNo,
												 &isNull);
					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "ctid is NULL");

					tupleid = (ItemPointer) DatumGetPointer(datum);
					tuple_ctid = *tupleid;		/* be sure we don't free
												 * ctid!! */
					tupleid = &tuple_ctid;
				}
				else
				{
					datum = ExecGetJunkAttribute(slot,
												 junkfilter->jf_junkAttNo,
												 &isNull);
					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "wholerow is NULL");

					oldtuple = DatumGetHeapTupleHeader(datum);
				}
			}

			/*
			 * apply the junkfilter if needed.
			 */
			if (operation != CMD_DELETE)
				slot = ExecFilterJunk(junkfilter, slot);
		}

		switch (operation)
		{
			case CMD_INSERT:
				slot = ExecInsert(slot, planSlot, estate, node->canSetTag,
								  PLANGEN_PLANNER, false /* isUpdate */);
				break;
			case CMD_UPDATE:
				slot = ExecUpdate(tupleid, oldtuple, slot, planSlot,
								&node->mt_epqstate, estate, node->canSetTag);
				break;
			case CMD_DELETE:
				slot = ExecDelete(tupleid, oldtuple, planSlot,
								  &node->mt_epqstate, estate, node->canSetTag,
								  PLANGEN_PLANNER, false /* isUpdate */);
				break;
			default:
				elog(ERROR, "unknown operation");
				break;
		}

		/*
		 * If the target is a partitioned table, ExecInsert / ExecUpdate /
		 * ExecDelete might have changed es_result_relation_info to point to
		 * a partition, instead of the top-level table. Reset it. (It would
		 * be more tidy if those functions cleaned up after themselves, but
		 * it's more robust to do it here just once.)
		 */
		estate->es_result_relation_info = resultRelInfo;

		/*
		 * If we got a RETURNING result, return it to caller.  We'll continue
		 * the work on next call.
		 */
		if (slot)
		{
			estate->es_result_relation_info = saved_resultRelInfo;
			return slot;
		}
	}

	/* Restore es_result_relation_info before exiting */
	estate->es_result_relation_info = saved_resultRelInfo;

	/*
	 * We're done, but fire AFTER STATEMENT triggers before exiting.
	 */
	/* In GPDB, don't fire statement triggers in reader processes */
	if (Gp_role != GP_ROLE_EXECUTE || Gp_is_writer)
		fireASTriggers(node);

	node->mt_done = true;

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitModifyTable
 * ----------------------------------------------------------------
 */
ModifyTableState *
ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags)
{
	ModifyTableState *mtstate;
	CmdType		operation = node->operation;
	int			nplans = list_length(node->plans);
	ResultRelInfo *saved_resultRelInfo;
	ResultRelInfo *resultRelInfo;
	TupleDesc	tupDesc;
	Plan	   *subplan;
	ListCell   *l;
	int			i;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * This should NOT get called during EvalPlanQual; we should have passed a
	 * subplan tree to EvalPlanQual, instead.  Use a runtime test not just
	 * Assert because this condition is easy to miss in testing ...
	 */
	if (estate->es_epqTuple != NULL)
		elog(ERROR, "ModifyTable should not be called during EvalPlanQual");

	/*
	 * create state structure
	 */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = (Plan *) node;
	mtstate->ps.state = estate;
	mtstate->ps.targetlist = NIL;		/* not actually used */

	mtstate->operation = operation;
	mtstate->canSetTag = node->canSetTag;
	mtstate->mt_done = false;

	mtstate->mt_plans = (PlanState **) palloc0(sizeof(PlanState *) * nplans);
	mtstate->resultRelInfo = estate->es_result_relations + node->resultRelIndex;
	mtstate->mt_arowmarks = (List **) palloc0(sizeof(List *) * nplans);
	mtstate->mt_nplans = nplans;
	/* set up epqstate with dummy subplan data for the moment */
	EvalPlanQualInit(&mtstate->mt_epqstate, estate, NULL, NIL, node->epqParam);

	/* GPDB: Don't fire statement-triggers in QE reader processes */
	if (Gp_role != GP_ROLE_EXECUTE || Gp_is_writer)
		mtstate->fireBSTriggers = true;

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "mt_plans".  This is also a convenient place to
	 * verify that the proposed target relations are valid and open their
	 * indexes for insertion of new index entries.	Note we *must* set
	 * estate->es_result_relation_info correctly while we initialize each
	 * sub-plan; ExecContextForcesOids depends on that!
	 */
	saved_resultRelInfo = estate->es_result_relation_info;

	resultRelInfo = mtstate->resultRelInfo;
	i = 0;
	foreach(l, node->plans)
	{
		subplan = (Plan *) lfirst(l);

		/*
		 * Verify result relation is a valid target for the current operation
		 */
		CheckValidResultRel(resultRelInfo->ri_RelationDesc, operation);

		/*
		 * If there are indices on the result relation, open them and save
		 * descriptors in the result relation info, so that we can add new
		 * index entries for the tuples we add/update.	We need not do this
		 * for a DELETE, however, since deletion doesn't affect indexes.
		 */
		if (resultRelInfo->ri_RelationDesc->rd_rel->relhasindex &&
			operation != CMD_DELETE)
			ExecOpenIndices(resultRelInfo);

		/* Now init the plan for this result rel */
		estate->es_result_relation_info = resultRelInfo;
		mtstate->mt_plans[i] = ExecInitNode(subplan, estate, eflags);

		resultRelInfo++;
		i++;
	}

	estate->es_result_relation_info = saved_resultRelInfo;

	/*
	 * Initialize RETURNING projections if needed.
	 */
	if (node->returningLists)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * Initialize result tuple slot and assign its rowtype using the first
		 * RETURNING list.	We assume the rest will look the same.
		 */
		tupDesc = ExecTypeFromTL((List *) linitial(node->returningLists),
								 false);

		/* Set up a slot for the output of the RETURNING projection(s) */
		ExecInitResultTupleSlot(estate, &mtstate->ps);
		ExecAssignResultType(&mtstate->ps, tupDesc);
		slot = mtstate->ps.ps_ResultTupleSlot;

		/* Need an econtext too */
		econtext = CreateExprContext(estate);
		mtstate->ps.ps_ExprContext = econtext;

		/*
		 * Build a projection for each result rel.
		 */
		resultRelInfo = mtstate->resultRelInfo;
		foreach(l, node->returningLists)
		{
			List	   *rlist = (List *) lfirst(l);
			List	   *rliststate;

			rliststate = (List *) ExecInitExpr((Expr *) rlist, &mtstate->ps);
			resultRelInfo->ri_projectReturning =
				ExecBuildProjectionInfo(rliststate, econtext, slot,
									 resultRelInfo->ri_RelationDesc->rd_att);
			resultRelInfo++;
		}
	}
	else
	{
		/*
		 * We still must construct a dummy result tuple type, because InitPlan
		 * expects one (maybe should change that?).
		 */
		tupDesc = ExecTypeFromTL(NIL, false);
		ExecInitResultTupleSlot(estate, &mtstate->ps);
		ExecAssignResultType(&mtstate->ps, tupDesc);

		mtstate->ps.ps_ExprContext = NULL;
	}

	/*
	 * If we have any secondary relations in an UPDATE or DELETE, they need to
	 * be treated like non-locked relations in SELECT FOR UPDATE, ie, the
	 * EvalPlanQual mechanism needs to be told about them.	Locate the
	 * relevant ExecRowMarks.
	 */
	foreach(l, node->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(l);
		ExecRowMark *erm;
		bool		isdistributed = false;

		Assert(IsA(rc, PlanRowMark));

		/* ignore "parent" rowmarks; they are irrelevant at runtime */
		if (rc->isParent)
			continue;

		/*
		 * Like in preprocess_targetlist, ignore distributed tables.
		 */
		/*
		 * GPDB_91_MERGE_FIXME: we are largely just ignoring locking altogether.
		 * Perhaps that's OK as long as we take a full table lock on any UPDATEs
		 * or DELETEs. But sure doesn't seem right. Can we do better?
		 */
		{
			RangeTblEntry *rte = rt_fetch(rc->rti, estate->es_plannedstmt->rtable);

			if (rte->rtekind == RTE_RELATION)
			{
				Relation relation = heap_open(rte->relid, NoLock);
				if (relation->rd_cdbpolicy &&
					relation->rd_cdbpolicy->ptype == POLICYTYPE_PARTITIONED)
					isdistributed = true;
				heap_close(relation, NoLock);
				if (isdistributed)
					continue;
			}
		}
		if (Gp_role == GP_ROLE_EXECUTE)
		{
			/*
			 * In the executor, we don't have information on which tables are
			 * distributed. Assume that everything is; we wouldn't be running this
			 * slice on an entry table otherwise.
			 */
			continue;
		}

		/* find ExecRowMark (same for all subplans) */
		erm = ExecFindRowMark(estate, rc->rti);

		/* build ExecAuxRowMark for each subplan */
		for (i = 0; i < nplans; i++)
		{
			ExecAuxRowMark *aerm;

			subplan = mtstate->mt_plans[i]->plan;
			aerm = ExecBuildAuxRowMark(erm, subplan->targetlist);
			mtstate->mt_arowmarks[i] = lappend(mtstate->mt_arowmarks[i], aerm);
		}
	}

	/* GPDB_90_MERGE_FIXME: This code was heavily refactored in the merge.
	 * This used to be in ExecInsert, in execMain.c. It was also heavily
	 * modified in GPDB, to handle rowmarks differently, and to disable
	 * some sanity checks for ORCA. I did not copy over those GPDB changes,
	 * because I wasn't sure how, and wasn't sure which of the changes
	 * were still needed.
	 */

	/* select first subplan */
	mtstate->mt_whichplan = 0;
	subplan = (Plan *) linitial(node->plans);
	EvalPlanQualSetPlan(&mtstate->mt_epqstate, subplan,
						mtstate->mt_arowmarks[0]);

	/*
	 * Initialize the junk filter(s) if needed.  INSERT queries need a filter
	 * if there are any junk attrs in the tlist.  UPDATE and DELETE always
	 * need a filter, since there's always a junk 'ctid' or 'wholerow'
	 * attribute present --- no need to look first.
	 *
	 * If there are multiple result relations, each one needs its own junk
	 * filter.	Note multiple rels are only possible for UPDATE/DELETE, so we
	 * can't be fooled by some needing a filter and some not.
	 *
	 * This section of code is also a convenient place to verify that the
	 * output of an INSERT or UPDATE matches the target table(s).
	 */
	{
		bool		junk_filter_needed = false;

		switch (operation)
		{
			case CMD_INSERT:
				foreach(l, subplan->targetlist)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(l);

					if (tle->resjunk)
					{
						junk_filter_needed = true;
						break;
					}
				}
				break;
			case CMD_UPDATE:
			case CMD_DELETE:
				junk_filter_needed = true;
				break;
			default:
				elog(ERROR, "unknown operation");
				break;
		}

		if (junk_filter_needed)
		{
			resultRelInfo = mtstate->resultRelInfo;
			for (i = 0; i < nplans; i++)
			{
				JunkFilter *j;

				subplan = mtstate->mt_plans[i]->plan;
				if (operation == CMD_INSERT || operation == CMD_UPDATE)
					ExecCheckPlanOutput(resultRelInfo->ri_RelationDesc,
										subplan->targetlist);

				j = ExecInitJunkFilter(subplan->targetlist,
							resultRelInfo->ri_RelationDesc->rd_att->tdhasoid,
									   ExecInitExtraTupleSlot(estate));

				if (operation == CMD_UPDATE || operation == CMD_DELETE)
				{
					/* For UPDATE/DELETE, find the appropriate junk attr now */
					if (resultRelInfo->ri_RelationDesc->rd_rel->relkind == RELKIND_RELATION)
					{
						j->jf_junkAttNo = ExecFindJunkAttribute(j, "ctid");
						if (!AttributeNumberIsValid(j->jf_junkAttNo))
							elog(ERROR, "could not find junk ctid column");
					}
					else
					{
						j->jf_junkAttNo = ExecFindJunkAttribute(j, "wholerow");
						if (!AttributeNumberIsValid(j->jf_junkAttNo))
							elog(ERROR, "could not find junk wholerow column");
					}
				}

				resultRelInfo->ri_junkFilter = j;
				resultRelInfo++;
			}
		}
		else
		{
			if (operation == CMD_INSERT)
				ExecCheckPlanOutput(mtstate->resultRelInfo->ri_RelationDesc,
									subplan->targetlist);
		}
	}

	/*
	 * Set up a tuple table slot for use for trigger output tuples. In a plan
	 * containing multiple ModifyTable nodes, all can share one such slot, so
	 * we keep it in the estate.
	 */
	if (estate->es_trig_tuple_slot == NULL)
		estate->es_trig_tuple_slot = ExecInitExtraTupleSlot(estate);

	/*
	 * Lastly, if this is not the primary (canSetTag) ModifyTable node, add it
	 * to estate->es_auxmodifytables so that it will be run to completion by
	 * ExecPostprocessPlan.  (It'd actually work fine to add the primary
	 * ModifyTable node too, but there's no need.)  Note the use of lcons not
	 * lappend: we need later-initialized ModifyTable nodes to be shut down
	 * before earlier ones.  This ensures that we don't throw away RETURNING
	 * rows that need to be seen by a later CTE subplan.
	 */
	if (Gp_role == GP_ROLE_EXECUTE || Gp_role == GP_ROLE_UTILITY)
	{
		/*
		 * We do not need this unless in executor or with utility role. Note
		 * This was added for the data modifying CTE feature but there are other
		 * cases could run into this also.
		 */
		if (!mtstate->canSetTag)
			estate->es_auxmodifytables = lcons(mtstate,
											   estate->es_auxmodifytables);
	}

	return mtstate;
}

/* ----------------------------------------------------------------
 *		ExecEndModifyTable
 *
 *		Shuts down the plan.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndModifyTable(ModifyTableState *node)
{
	int			i;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * Terminate EPQ execution if active
	 */
	EvalPlanQualEnd(&node->mt_epqstate);

	/*
	 * shut down subplans
	 */
	for (i = 0; i < node->mt_nplans; i++)
		ExecEndNode(node->mt_plans[i]);
}

void
ExecReScanModifyTable(ModifyTableState *node)
{
	/*
	 * Currently, we don't need to support rescan on ModifyTable nodes. The
	 * semantics of that would be a bit debatable anyway.
	 */
	elog(ERROR, "ExecReScanModifyTable is not implemented");
}