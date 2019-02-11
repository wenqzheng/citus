/*-------------------------------------------------------------------------
 *
 * fast_path_select.c
 *
 * Planning logic for fast path router planner queries. In this context,
 * we define "Fast Path Planning for SELECT" as queries where Citus can
 * skip relying on the standard_planner().
 *
 * For router planner, standard_planner() is mostly important to generate
 * the necessary restriction information. Later, the restriction information
 * generated by it used to decide whether all the shards that a distributed
 * query touches reside on a single worker node. However, standard_planner()
 * does a lot of extra things such as generating the plan, which are mostly
 * unnecessary in the context of distributed planning.
 *
 * There are certain types of queries where Citus could skip relying on
 * standard_planner() to generate the restriction information. For queries
 * in the following format, Citus does not need any information that the
 * standard_planner() generates:
 *   SELECT ... FROM single_table WHERE distribution_key = X;
 *
 * Note that the queries might not be as simple as the above such that
 * GROUP BY, WINDOW FUNCIONS, ORDER BY or HAVING etc. are all acceptable. The
 * only rule is that the query is on a single distributed (or reference) table
 * and there is a "distribution_key = X;" in the WHERE clause. With that, we
 * could use to decide the shard that a distributed query touches reside on
 * a worker node.
 *
 * Copyright (c) 2019, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "distributed/multi_physical_planner.h" /* only to use some utility functions */
#include "distributed/metadata_cache.h"
#include "distributed/multi_router_planner.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/shard_pruning.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "optimizer/clauses.h"


static bool ColumnMatchExpressionAtTopLevelConjunction(Node *node, Var *column);


/*
 * GeneratePlaceHolderPlannedStmt creates a planned statement which contains
 * a sequential scan on the relation that is accessed by the input query.
 * The returned PlannedStmt is not proper (e.g., set_plan_references() is
 * not called on the plan or the quals are not set), so should not be
 * passed to the executor directly. This is only useful to have a
 * placeholder PlannedStmt where target list is properly set. Note that
 * this is what router executor relies on.
 *
 * This function makes the assumption (and the assertion) that
 * the input query is in the form defined by FastPathRouterQuery().
 */
PlannedStmt *
GeneratePlaceHolderPlannedStmt(Query *parse)
{
	PlannedStmt *result = makeNode(PlannedStmt);
	SeqScan *seqScanNode = makeNode(SeqScan);
	Plan *plan = &seqScanNode->plan;
	Oid relationId = InvalidOid;

	AssertArg(FastPathRouterQuery(parse));

	/* there is only a single relation rte */
	seqScanNode->scanrelid = 1;

	plan->targetlist = copyObject(parse->targetList);
	plan->qual = NULL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->plan_node_id = 1;

	/*  rtable is used for access permission checks */
	result->commandType = CMD_SELECT;
	result->queryId = parse->queryId;
	result->stmt_len = parse->stmt_len;

	result->rtable = copyObject(parse->rtable);
	result->planTree = (Plan *) plan;

	relationId = ExtractFirstDistributedTableId(parse);
	result->relationOids = list_make1_oid(relationId);

	return result;
}


/*
 * FastPathRouterQuery gets a query and returns true if the query is eligable for
 * being a fast path router query.
 * The requirements for the fast path query can be listed below:
 *
 *   - SELECT query without CTES, sublinks-subqueries, set operations
 *   - The query should touch only a single hash distributed or reference table
 *   - The distribution with equality operator should be in the WHERE clause
 *      and it should be ANDed with any other filters. Also, the distribution
 *      key should only exists once in the WHERE clause. So basically,
 *          SELECT ... FROM dist_table WHERE dist_key = X
 */
bool
FastPathRouterQuery(Query *query)
{
	RangeTblEntry *rangeTableEntry = NULL;
	List *rangeTableList = NIL;
	FromExpr *joinTree = query->jointree;
	Node *quals = NULL;
	Oid distributedTableId = InvalidOid;
	Var *distributionKey = NULL;
	DistTableCacheEntry *cacheEntry = NULL;
	List *varClauseList = NIL;
	ListCell *varClauseCell = NULL;
	int partitionColumnReferenceCount = 0;

	/*
	 * We want to deal with only very simple select queries. Some of the
	 * checks might be too restrictive, still we prefer this way.
	 */
	if (query->commandType != CMD_SELECT || query->cteList != NIL ||
		query->hasSubLinks || query->setOperations != NULL ||
		query->hasForUpdate || query->hasTargetSRFs || query->hasRowSecurity)
	{
		return false;
	}

	/*
	 * Pull all range table entries. We prefer this to make sure that
	 * there is no subqueries in the any part of the query, including
	 * the from clause, where clause, target list or HAVING clause etc.
	 */
	ExtractRangeTableEntryWalker((Node *) query, &rangeTableList);
	if (list_length(rangeTableList) != 1)
	{
		return false;
	}

	/* make sure that the only range table in FROM clause */
	if (list_length(query->rtable) != 1)
	{
		return false;
	}

	rangeTableEntry = (RangeTblEntry *) linitial(query->rtable);
	if (rangeTableEntry->rtekind != RTE_RELATION)
	{
		return false;
	}

	/*
	 * We don't want to deal with potentially overlapping
	 * append/range distributed tables.
	 */
	distributedTableId = rangeTableEntry->relid;
	cacheEntry = DistributedTableCacheEntry(distributedTableId);
	if (!(cacheEntry->partitionMethod == DISTRIBUTE_BY_HASH ||
		  cacheEntry->partitionMethod == DISTRIBUTE_BY_NONE))
	{
		return false;
	}

	/* WHERE clause should not be empty for distributed tables */
	if (joinTree == NULL ||
		(cacheEntry->partitionMethod != DISTRIBUTE_BY_NONE && joinTree->quals == NULL))
	{
		return false;
	}

	/* convert list of expressions into expression tree */
	quals = joinTree->quals;
	if (quals != NULL && IsA(quals, List))
	{
		quals = (Node *) make_ands_explicit((List *) quals);
	}

	/* WHERE false; queries are tricky, let the non-fast path handle that */
	if (ContainsFalseClause(make_ands_implicit((Expr *) quals)))
	{
		return false;
	}

	/*
	 * Distribution column must be used in a simple equality match check and it must be
	 * place at top level conjustion operator. In simple words, we should have
	 *	    WHERE dist_key = VALUE [AND  ....];
	 *
	 * We also skip this check for reference tables.
	 */
	distributionKey = PartitionColumn(distributedTableId, 1);
	if (distributionKey != NULL &&
		!ColumnMatchExpressionAtTopLevelConjunction(quals, distributionKey))
	{
		return false;
	}

	/* make sure partition column is used only once in the quals */
	varClauseList = pull_var_clause_default(quals);
	foreach(varClauseCell, varClauseList)
	{
		Var *column = (Var *) lfirst(varClauseCell);
		if (equal(column, distributionKey))
		{
			partitionColumnReferenceCount++;

			if (partitionColumnReferenceCount > 1)
			{
				return false;
			}
		}
	}

	/*
	 * With old APIs you can create hash tables without shards. Thus,
	 * make sure there is at least one shard for this table to use
	 * fast-path.
	 */
	if (cacheEntry->shardIntervalArrayLength == 0)
	{
		return false;
	}

	return true;
}


/*
 * ColumnMatchExpressionAtTopLevelConjunction returns true if the query contains an exact
 * match (equal) expression on the provided column. The function returns true only
 * if the match expression has an AND relation with the rest of the expression tree.
 */
static bool
ColumnMatchExpressionAtTopLevelConjunction(Node *node, Var *column)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, OpExpr))
	{
		OpExpr *opExpr = (OpExpr *) node;
		bool simpleExpression = SimpleOpExpression((Expr *) opExpr);
		bool columnInExpr = false;

		if (!simpleExpression)
		{
			return false;
		}

		columnInExpr = OpExpressionContainsColumn(opExpr, column);
		if (!columnInExpr)
		{
			return false;
		}

		return OperatorImplementsEquality(opExpr->opno);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr *boolExpr = (BoolExpr *) node;
		List *argumentList = boolExpr->args;
		ListCell *argumentCell = NULL;

		if (boolExpr->boolop != AND_EXPR)
		{
			return false;
		}

		foreach(argumentCell, argumentList)
		{
			Node *argumentNode = (Node *) lfirst(argumentCell);

			if (ColumnMatchExpressionAtTopLevelConjunction(argumentNode, column))
			{
				return true;
			}
		}
	}

	return false;
}