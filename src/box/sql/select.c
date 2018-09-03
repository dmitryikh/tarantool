/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains C code routines that are called by the parser
 * to handle SELECT statements in SQLite.
 */
#include "coll.h"
#include "sqliteInt.h"
#include "tarantoolInt.h"
#include "box/box.h"
#include "box/coll_id_cache.h"
#include "box/schema.h"
#include "box/session.h"

/*
 * Trace output macros
 */
#ifdef SELECTTRACE_ENABLED
/***/ int sqlite3SelectTrace = 0;
#define SELECTTRACE(K,P,S,X)  \
  if(sqlite3SelectTrace&(K))   \
    sqlite3DebugPrintf("%*s%s.%p: ",(P)->nSelectIndent*2-2,"",\
        (S)->zSelName,(S)),\
    sqlite3DebugPrintf X
#else
#define SELECTTRACE(K,P,S,X)
#endif

/*
 * An instance of the following object is used to record information about
 * how to process the DISTINCT keyword, to simplify passing that information
 * into the selectInnerLoop() routine.
 */
typedef struct DistinctCtx DistinctCtx;
struct DistinctCtx {
	u8 isTnct;		/* True if the DISTINCT keyword is present */
	u8 eTnctType;		/* One of the WHERE_DISTINCT_* operators */
	int tabTnct;		/* Ephemeral table used for DISTINCT processing */
	int addrTnct;		/* Address of OP_OpenEphemeral opcode for tabTnct */
};

/*
 * An instance of the following object is used to record information about
 * the ORDER BY (or GROUP BY) clause of query is being coded.
 */
typedef struct SortCtx SortCtx;
struct SortCtx {
	ExprList *pOrderBy;	/* The ORDER BY (or GROUP BY clause) */
	int nOBSat;		/* Number of ORDER BY terms satisfied by indices */
	int iECursor;		/* Cursor number for the sorter */
	int regReturn;		/* Register holding block-output return address */
	int labelBkOut;		/* Start label for the block-output subroutine */
	int addrSortIndex;	/* Address of the OP_SorterOpen or OP_OpenEphemeral */
	int labelDone;		/* Jump here when done, ex: LIMIT reached */
	u8 sortFlags;		/* Zero or more SORTFLAG_* bits */
	u8 bOrderedInnerLoop;	/* ORDER BY correctly sorts the inner loop */
};
#define SORTFLAG_UseSorter  0x01	/* Use SorterOpen instead of OpenEphemeral */
#define SORTFLAG_DESC 0xF0

/*
 * Delete all the content of a Select structure.  Deallocate the structure
 * itself only if bFree is true.
 */
static void
clearSelect(sqlite3 * db, Select * p, int bFree)
{
	while (p) {
		Select *pPrior = p->pPrior;
		sql_expr_list_delete(db, p->pEList);
		sqlite3SrcListDelete(db, p->pSrc);
		sql_expr_delete(db, p->pWhere, false);
		sql_expr_list_delete(db, p->pGroupBy);
		sql_expr_delete(db, p->pHaving, false);
		sql_expr_list_delete(db, p->pOrderBy);
		sql_expr_delete(db, p->pLimit, false);
		sql_expr_delete(db, p->pOffset, false);
		if (p->pWith)
			sqlite3WithDelete(db, p->pWith);
		if (bFree)
			sqlite3DbFree(db, p);
		p = pPrior;
		bFree = 1;
	}
}

/*
 * Initialize a SelectDest structure.
 */
void
sqlite3SelectDestInit(SelectDest * pDest, int eDest, int iParm)
{
	pDest->eDest = (u8) eDest;
	pDest->iSDParm = iParm;
	pDest->zAffSdst = 0;
	pDest->iSdst = 0;
	pDest->nSdst = 0;
}

/*
 * Allocate a new Select structure and return a pointer to that
 * structure.
 */
Select *
sqlite3SelectNew(Parse * pParse,	/* Parsing context */
		 ExprList * pEList,	/* which columns to include in the result */
		 SrcList * pSrc,	/* the FROM clause -- which tables to scan */
		 Expr * pWhere,		/* the WHERE clause */
		 ExprList * pGroupBy,	/* the GROUP BY clause */
		 Expr * pHaving,	/* the HAVING clause */
		 ExprList * pOrderBy,	/* the ORDER BY clause */
		 u32 selFlags,		/* Flag parameters, such as SF_Distinct */
		 Expr * pLimit,		/* LIMIT value.  NULL means not used */
		 Expr * pOffset)	/* OFFSET value.  NULL means no offset */
{
	Select *pNew;
	Select standin;
	sqlite3 *db = pParse->db;
	pNew = sqlite3DbMallocRawNN(db, sizeof(*pNew));
	if (pNew == 0) {
		assert(db->mallocFailed);
		pNew = &standin;
	}
	if (pEList == 0) {
		pEList = sql_expr_list_append(pParse->db, NULL,
					      sqlite3Expr(db, TK_ASTERISK, 0));
	}
	struct session MAYBE_UNUSED *user_session;
	user_session = current_session();
	pNew->pEList = pEList;
	pNew->op = TK_SELECT;
	pNew->selFlags = selFlags;
	pNew->iLimit = 0;
	pNew->iOffset = 0;
#ifdef SELECTTRACE_ENABLED
	pNew->zSelName[0] = 0;
	if (user_session->sql_flags & SQLITE_SelectTrace)
		sqlite3SelectTrace = 0xfff;
	else
		sqlite3SelectTrace = 0;
#endif
	pNew->addrOpenEphm[0] = -1;
	pNew->addrOpenEphm[1] = -1;
	pNew->nSelectRow = 0;
	if (pSrc == 0)
		pSrc = sqlite3DbMallocZero(db, sizeof(*pSrc));
	pNew->pSrc = pSrc;
	pNew->pWhere = pWhere;
	pNew->pGroupBy = pGroupBy;
	pNew->pHaving = pHaving;
	pNew->pOrderBy = pOrderBy;
	pNew->pPrior = 0;
	pNew->pNext = 0;
	pNew->pLimit = pLimit;
	pNew->pOffset = pOffset;
	pNew->pWith = 0;
	assert(pOffset == 0 || pLimit != 0 || pParse->nErr > 0
	       || db->mallocFailed != 0);
	if (db->mallocFailed) {
		clearSelect(db, pNew, pNew != &standin);
		pNew = 0;
	} else {
		assert(pNew->pSrc != 0 || pParse->nErr > 0);
	}
	assert(pNew != &standin);
	return pNew;
}

#ifdef SELECTTRACE_ENABLED
/*
 * Set the name of a Select object
 */
void
sqlite3SelectSetName(Select * p, const char *zName)
{
	if (p && zName) {
		sqlite3_snprintf(sizeof(p->zSelName), p->zSelName, "%s", zName);
	}
}
#endif

void
sql_select_delete(sqlite3 *db, Select *p)
{
	if (p)
		clearSelect(db, p, 1);
}

int
sql_select_from_table_count(const struct Select *select)
{
	assert(select != NULL && select->pSrc != NULL);
	return select->pSrc->nSrc;
}

const char *
sql_select_from_table_name(const struct Select *select, int i)
{
	assert(select != NULL && select->pSrc != NULL);
	assert(i >= 0 && i < select->pSrc->nSrc);
	return select->pSrc->a[i].zName;
}

/*
 * Return a pointer to the right-most SELECT statement in a compound.
 */
static Select *
findRightmost(Select * p)
{
	while (p->pNext)
		p = p->pNext;
	return p;
}


/**
 * Work the same as sqlite3SrcListAppend(), but before adding to
 * list provide check on name duplicates: only values with unique
 * names are appended.
 *
 * @param db Database handler.
 * @param list List of entries.
 * @param new_name Name of entity to be added.
 * @retval @list with new element on success, old one otherwise.
 */
static struct SrcList *
src_list_append_unique(struct sqlite3 *db, struct SrcList *list,
		       const char *new_name)
{
	assert(list != NULL);
	assert(new_name != NULL);

	for (int i = 0; i < list->nSrc; ++i) {
		const char *name = list->a[i].zName;
		if (name != NULL && strcmp(new_name, name) == 0)
			return list;
	}
	struct Token token = { new_name, strlen(new_name), 0 };
	return sqlite3SrcListAppend(db, list, &token);
}

/**
 * This function is an inner call of recursive traverse through
 * select AST starting from interface function
 * sql_select_expand_from_tables().
 *
 * @param top_select The root of AST.
 * @param sub_select sub-select of current level recursion.
 */
static void
expand_names_sub_select(struct Select *top_select, struct Select *sub_select)
{
	assert(top_select != NULL);
	assert(sub_select != NULL);
	struct SrcList_item *sub_src = sub_select->pSrc->a;
	for (int i = 0; i < sub_select->pSrc->nSrc; ++i, ++sub_src) {
		if (sub_src->zName == NULL) {
			expand_names_sub_select(top_select, sub_src->pSelect);
		} else {
			top_select->pSrc =
				src_list_append_unique(sql_get(),
						       top_select->pSrc,
						       sub_src->zName);
		}
	}
}

void
sql_select_expand_from_tables(struct Select *select)
{
	assert(select != NULL);
	struct SrcList_item *src = select->pSrc->a;
	for (int i = 0; i < select->pSrc->nSrc; ++i, ++src) {
		if (select->pSrc->a[i].zName == NULL)
			expand_names_sub_select(select, src->pSelect);
	}
}

/*
 * Given 1 to 3 identifiers preceding the JOIN keyword, determine the
 * type of join.  Return an integer constant that expresses that type
 * in terms of the following bit values:
 *
 *     JT_INNER
 *     JT_CROSS
 *     JT_OUTER
 *     JT_NATURAL
 *     JT_LEFT
 *     JT_RIGHT
 *
 * A full outer join is the combination of JT_LEFT and JT_RIGHT.
 *
 * If an illegal or unsupported join type is seen, then still return
 * a join type, but put an error in the pParse structure.
 */
int
sqlite3JoinType(Parse * pParse, Token * pA, Token * pB, Token * pC)
{
	int jointype = 0;
	Token *apAll[3];
	Token *p;
	/*   0123456789 123456789 123456789 123 */
	static const char zKeyText[] = "naturaleftouterightfullinnercross";
	static const struct {
		u8 i;		/* Beginning of keyword text in zKeyText[] */
		u8 nChar;	/* Length of the keyword in characters */
		u8 code;	/* Join type mask */
	} aKeyword[] = {
		/* natural */  {
		0, 7, JT_NATURAL},
		    /* left    */  {
		6, 4, JT_LEFT | JT_OUTER},
		    /* outer   */  {
		10, 5, JT_OUTER},
		    /* right   */  {
		14, 5, JT_RIGHT | JT_OUTER},
		    /* full    */  {
		19, 4, JT_LEFT | JT_RIGHT | JT_OUTER},
		    /* inner   */  {
		23, 5, JT_INNER},
		    /* cross   */  {
	28, 5, JT_INNER | JT_CROSS},};
	int i, j;
	apAll[0] = pA;
	apAll[1] = pB;
	apAll[2] = pC;
	for (i = 0; i < 3 && apAll[i]; i++) {
		p = apAll[i];
		for (j = 0; j < ArraySize(aKeyword); j++) {
			if (p->n == aKeyword[j].nChar
			    && sqlite3StrNICmp((char *)p->z,
					       &zKeyText[aKeyword[j].i],
					       p->n) == 0) {
				jointype |= aKeyword[j].code;
				break;
			}
		}
		testcase(j == 0 || j == 1 || j == 2 || j == 3 || j == 4
			 || j == 5 || j == 6);
		if (j >= ArraySize(aKeyword)) {
			jointype |= JT_ERROR;
			break;
		}
	}
	if ((jointype & (JT_INNER | JT_OUTER)) == (JT_INNER | JT_OUTER) ||
	    (jointype & JT_ERROR) != 0) {
		const char *zSp = " ";
		assert(pB != 0);
		if (pC == 0) {
			zSp++;
		}
		sqlite3ErrorMsg(pParse, "unknown or unsupported join type: "
				"%T %T%s%T", pA, pB, zSp, pC);
		jointype = JT_INNER;
	} else if ((jointype & JT_OUTER) != 0
		   && (jointype & (JT_LEFT | JT_RIGHT)) != JT_LEFT) {
		sqlite3ErrorMsg(pParse,
				"RIGHT and FULL OUTER JOINs are not currently supported");
		jointype = JT_INNER;
	}
	return jointype;
}

/*
 * Return the index of a column in a table.  Return -1 if the column
 * is not contained in the table.
 */
static int
columnIndex(Table * pTab, const char *zCol)
{
	int i;
	for (i = 0; i < (int)pTab->def->field_count; i++) {
		if (strcmp(pTab->def->fields[i].name, zCol) == 0)
			return i;
	}
	return -1;
}

/*
 * Search the first N tables in pSrc, from left to right, looking for a
 * table that has a column named zCol.
 *
 * When found, set *piTab and *piCol to the table index and column index
 * of the matching column and return TRUE.
 *
 * If not found, return FALSE.
 */
static int
tableAndColumnIndex(SrcList * pSrc,	/* Array of tables to search */
		    int N,		/* Number of tables in pSrc->a[] to search */
		    const char *zCol,	/* Name of the column we are looking for */
		    int *piTab,		/* Write index of pSrc->a[] here */
		    int *piCol)		/* Write index of pSrc->a[*piTab].pTab->aCol[] here */
{
	int i;			/* For looping over tables in pSrc */
	int iCol;		/* Index of column matching zCol */

	assert((piTab == 0) == (piCol == 0));	/* Both or neither are NULL */
	for (i = 0; i < N; i++) {
		iCol = columnIndex(pSrc->a[i].pTab, zCol);
		if (iCol >= 0) {
			if (piTab) {
				*piTab = i;
				*piCol = iCol;
			}
			return 1;
		}
	}
	return 0;
}

/*
 * This function is used to add terms implied by JOIN syntax to the
 * WHERE clause expression of a SELECT statement. The new term, which
 * is ANDed with the existing WHERE clause, is of the form:
 *
 *    (tab1.col1 = tab2.col2)
 *
 * where tab1 is the iSrc'th table in SrcList pSrc and tab2 is the
 * (iSrc+1)'th. Column col1 is column iColLeft of tab1, and col2 is
 * column iColRight of tab2.
 */
static void
addWhereTerm(Parse * pParse,	/* Parsing context */
	     SrcList * pSrc,	/* List of tables in FROM clause */
	     int iLeft,		/* Index of first table to join in pSrc */
	     int iColLeft,	/* Index of column in first table */
	     int iRight,	/* Index of second table in pSrc */
	     int iColRight,	/* Index of column in second table */
	     int isOuterJoin,	/* True if this is an OUTER join */
	     Expr ** ppWhere)	/* IN/OUT: The WHERE clause to add to */
{
	sqlite3 *db = pParse->db;
	Expr *pE1;
	Expr *pE2;
	Expr *pEq;

	assert(iLeft < iRight);
	assert(pSrc->nSrc > iRight);
	assert(pSrc->a[iLeft].pTab);
	assert(pSrc->a[iRight].pTab);

	pE1 = sqlite3CreateColumnExpr(db, pSrc, iLeft, iColLeft);
	pE2 = sqlite3CreateColumnExpr(db, pSrc, iRight, iColRight);

	pEq = sqlite3PExpr(pParse, TK_EQ, pE1, pE2);
	if (pEq && isOuterJoin) {
		ExprSetProperty(pEq, EP_FromJoin);
		assert(!ExprHasProperty(pEq, EP_TokenOnly | EP_Reduced));
		ExprSetVVAProperty(pEq, EP_NoReduce);
		pEq->iRightJoinTable = (i16) pE2->iTable;
	}
	*ppWhere = sqlite3ExprAnd(db, *ppWhere, pEq);
}

/*
 * Set the EP_FromJoin property on all terms of the given expression.
 * And set the Expr.iRightJoinTable to iTable for every term in the
 * expression.
 *
 * The EP_FromJoin property is used on terms of an expression to tell
 * the LEFT OUTER JOIN processing logic that this term is part of the
 * join restriction specified in the ON or USING clause and not a part
 * of the more general WHERE clause.  These terms are moved over to the
 * WHERE clause during join processing but we need to remember that they
 * originated in the ON or USING clause.
 *
 * The Expr.iRightJoinTable tells the WHERE clause processing that the
 * expression depends on table iRightJoinTable even if that table is not
 * explicitly mentioned in the expression.  That information is needed
 * for cases like this:
 *
 *    SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.b AND t1.x=5
 *
 * The where clause needs to defer the handling of the t1.x=5
 * term until after the t2 loop of the join.  In that way, a
 * NULL t2 row will be inserted whenever t1.x!=5.  If we do not
 * defer the handling of t1.x=5, it will be processed immediately
 * after the t1 loop and rows with t1.x!=5 will never appear in
 * the output, which is incorrect.
 */
static void
setJoinExpr(Expr * p, int iTable)
{
	while (p) {
		ExprSetProperty(p, EP_FromJoin);
		assert(!ExprHasProperty(p, EP_TokenOnly | EP_Reduced));
		ExprSetVVAProperty(p, EP_NoReduce);
		p->iRightJoinTable = (i16) iTable;
		if (p->op == TK_FUNCTION && p->x.pList) {
			int i;
			for (i = 0; i < p->x.pList->nExpr; i++) {
				setJoinExpr(p->x.pList->a[i].pExpr, iTable);
			}
		}
		setJoinExpr(p->pLeft, iTable);
		p = p->pRight;
	}
}

/*
 * This routine processes the join information for a SELECT statement.
 * ON and USING clauses are converted into extra terms of the WHERE clause.
 * NATURAL joins also create extra WHERE clause terms.
 *
 * The terms of a FROM clause are contained in the Select.pSrc structure.
 * The left most table is the first entry in Select.pSrc.  The right-most
 * table is the last entry.  The join operator is held in the entry to
 * the left.  Thus entry 0 contains the join operator for the join between
 * entries 0 and 1.  Any ON or USING clauses associated with the join are
 * also attached to the left entry.
 *
 * This routine returns the number of errors encountered.
 */
static int
sqliteProcessJoin(Parse * pParse, Select * p)
{
	SrcList *pSrc;		/* All tables in the FROM clause */
	int i, j;		/* Loop counters */
	struct SrcList_item *pLeft;	/* Left table being joined */
	struct SrcList_item *pRight;	/* Right table being joined */

	pSrc = p->pSrc;
	pLeft = &pSrc->a[0];
	pRight = &pLeft[1];
	for (i = 0; i < pSrc->nSrc - 1; i++, pRight++, pLeft++) {
		Table *pLeftTab = pLeft->pTab;
		Table *pRightTab = pRight->pTab;
		int isOuter;

		if (NEVER(pLeftTab == 0 || pRightTab == 0))
			continue;
		isOuter = (pRight->fg.jointype & JT_OUTER) != 0;

		/* When the NATURAL keyword is present, add WHERE clause terms for
		 * every column that the two tables have in common.
		 */
		if (pRight->fg.jointype & JT_NATURAL) {
			if (pRight->pOn || pRight->pUsing) {
				sqlite3ErrorMsg(pParse,
						"a NATURAL join may not have "
						"an ON or USING clause", 0);
				return 1;
			}
			for (j = 0; j < (int)pRightTab->def->field_count; j++) {
				char *zName;	/* Name of column in the right table */
				int iLeft;	/* Matching left table */
				int iLeftCol;	/* Matching column in the left table */

				zName = pRightTab->def->fields[j].name;
				if (tableAndColumnIndex
				    (pSrc, i + 1, zName, &iLeft, &iLeftCol)) {
					addWhereTerm(pParse, pSrc, iLeft,
						     iLeftCol, i + 1, j,
						     isOuter, &p->pWhere);
				}
			}
		}

		/* Disallow both ON and USING clauses in the same join
		 */
		if (pRight->pOn && pRight->pUsing) {
			sqlite3ErrorMsg(pParse, "cannot have both ON and USING "
					"clauses in the same join");
			return 1;
		}

		/* Add the ON clause to the end of the WHERE clause, connected by
		 * an AND operator.
		 */
		if (pRight->pOn) {
			if (isOuter)
				setJoinExpr(pRight->pOn, pRight->iCursor);
			p->pWhere =
			    sqlite3ExprAnd(pParse->db, p->pWhere, pRight->pOn);
			pRight->pOn = 0;
		}

		/* Create extra terms on the WHERE clause for each column named
		 * in the USING clause.  Example: If the two tables to be joined are
		 * A and B and the USING clause names X, Y, and Z, then add this
		 * to the WHERE clause:    A.X=B.X AND A.Y=B.Y AND A.Z=B.Z
		 * Report an error if any column mentioned in the USING clause is
		 * not contained in both tables to be joined.
		 */
		if (pRight->pUsing) {
			IdList *pList = pRight->pUsing;
			for (j = 0; j < pList->nId; j++) {
				char *zName;	/* Name of the term in the USING clause */
				int iLeft;	/* Table on the left with matching column name */
				int iLeftCol;	/* Column number of matching column on the left */
				int iRightCol;	/* Column number of matching column on the right */

				zName = pList->a[j].zName;
				iRightCol = columnIndex(pRightTab, zName);
				if (iRightCol < 0
				    || !tableAndColumnIndex(pSrc, i + 1, zName,
							    &iLeft, &iLeftCol)
				    ) {
					sqlite3ErrorMsg(pParse,
							"cannot join using column %s - column "
							"not present in both tables",
							zName);
					return 1;
				}
				addWhereTerm(pParse, pSrc, iLeft, iLeftCol,
					     i + 1, iRightCol, isOuter,
					     &p->pWhere);
			}
		}
	}
	return 0;
}

/**
 * Given an expression list, generate a key_def structure that
 * records the collating sequence for each expression in that
 * expression list.
 *
 * If the ExprList is an ORDER BY or GROUP BY clause then the
 * resulting key_def structure is appropriate for initializing
 * a virtual index to implement that clause.  If the ExprList is
 * the result set of a SELECT then the key_info structure is
 * appropriate for initializing a virtual index to implement a
 * DISTINCT test.
 *
 * Space to hold the key_info structure is obtained from malloc.
 * The calling function is responsible for seeing that this
 * structure is eventually freed.
 *
 * @param parse Parsing context.
 * @param list Expression list.
 * @param start No of leading parts to skip.
 *
 * @retval Allocated key_def, NULL in case of OOM.
 */
static struct key_def *
sql_expr_list_to_key_def(struct Parse *parse, struct ExprList *list, int start);


/*
 * Generate code that will push the record in registers regData
 * through regData+nData-1 onto the sorter.
 */
static void
pushOntoSorter(Parse * pParse,		/* Parser context */
	       SortCtx * pSort,		/* Information about the ORDER BY clause */
	       Select * pSelect,	/* The whole SELECT statement */
	       int regData,		/* First register holding data to be sorted */
	       int regOrigData,		/* First register holding data before packing */
	       int nData,		/* Number of elements in the data array */
	       int nPrefixReg)		/* No. of reg prior to regData available for use */
{
	Vdbe *v = pParse->pVdbe;	/* Stmt under construction */
	int bSeq = ((pSort->sortFlags & SORTFLAG_UseSorter) == 0);
	int nExpr = pSort->pOrderBy->nExpr;	/* No. of ORDER BY terms */
	int nBase = nExpr + bSeq + nData;	/* Fields in sorter record */
	int regBase;		/* Regs for sorter record */
	int regRecord = ++pParse->nMem;	/* Assembled sorter record */
	int nOBSat = pSort->nOBSat;	/* ORDER BY terms to skip */
	int op;			/* Opcode to add sorter record to sorter */
	int iLimit;		/* LIMIT counter */

	assert(bSeq == 0 || bSeq == 1);
	assert(nData == 1 || regData == regOrigData || regOrigData == 0);
	if (nPrefixReg) {
		assert(nPrefixReg == nExpr + bSeq);
		regBase = regData - nExpr - bSeq;
	} else {
		regBase = pParse->nMem + 1;
		pParse->nMem += nBase;
	}
	assert(pSelect->iOffset == 0 || pSelect->iLimit != 0);
	iLimit = pSelect->iOffset ? pSelect->iOffset + 1 : pSelect->iLimit;
	pSort->labelDone = sqlite3VdbeMakeLabel(v);
	sqlite3ExprCodeExprList(pParse, pSort->pOrderBy, regBase, regOrigData,
				SQLITE_ECEL_DUP | (regOrigData ? SQLITE_ECEL_REF
						   : 0));
	if (bSeq) {
		sqlite3VdbeAddOp2(v, OP_Sequence, pSort->iECursor,
				  regBase + nExpr);
	}
	if (nPrefixReg == 0 && nData > 0) {
		sqlite3ExprCodeMove(pParse, regData, regBase + nExpr + bSeq,
				    nData);
	}
	sqlite3VdbeAddOp3(v, OP_MakeRecord, regBase + nOBSat, nBase - nOBSat,
			  regRecord);
	if (nOBSat > 0) {
		int regPrevKey;	/* The first nOBSat columns of the previous row */
		int addrFirst;	/* Address of the OP_IfNot opcode */
		int addrJmp;	/* Address of the OP_Jump opcode */
		VdbeOp *pOp;	/* Opcode that opens the sorter */
		int nKey;	/* Number of sorting key columns, including OP_Sequence */

		regPrevKey = pParse->nMem + 1;
		pParse->nMem += pSort->nOBSat;
		nKey = nExpr - pSort->nOBSat + bSeq;
		if (bSeq) {
			addrFirst =
			    sqlite3VdbeAddOp1(v, OP_IfNot, regBase + nExpr);
		} else {
			addrFirst =
			    sqlite3VdbeAddOp1(v, OP_SequenceTest,
					      pSort->iECursor);
		}
		VdbeCoverage(v);
		sqlite3VdbeAddOp3(v, OP_Compare, regPrevKey, regBase,
				  pSort->nOBSat);
		pOp = sqlite3VdbeGetOp(v, pSort->addrSortIndex);
		if (pParse->db->mallocFailed)
			return;
		pOp->p2 = nKey + nData;
		struct key_def *def = key_def_dup(pOp->p4.key_def);
		if (def == NULL) {
			sqlite3OomFault(pParse->db);
			return;
		}
		for (uint32_t i = 0; i < def->part_count; ++i)
			pOp->p4.key_def->parts[i].sort_order = SORT_ORDER_ASC;
		sqlite3VdbeChangeP4(v, -1, (char *)def, P4_KEYDEF);
		pOp->p4.key_def = sql_expr_list_to_key_def(pParse,
							   pSort->pOrderBy,
							   nOBSat);
		addrJmp = sqlite3VdbeCurrentAddr(v);
		sqlite3VdbeAddOp3(v, OP_Jump, addrJmp + 1, 0, addrJmp + 1);
		VdbeCoverage(v);
		pSort->labelBkOut = sqlite3VdbeMakeLabel(v);
		pSort->regReturn = ++pParse->nMem;
		sqlite3VdbeAddOp2(v, OP_Gosub, pSort->regReturn,
				  pSort->labelBkOut);
		sqlite3VdbeAddOp1(v, OP_ResetSorter, pSort->iECursor);
		if (iLimit) {
			sqlite3VdbeAddOp2(v, OP_IfNot, iLimit,
					  pSort->labelDone);
			VdbeCoverage(v);
		}
		sqlite3VdbeJumpHere(v, addrFirst);
		sqlite3ExprCodeMove(pParse, regBase, regPrevKey, pSort->nOBSat);
		sqlite3VdbeJumpHere(v, addrJmp);
	}
	if (pSort->sortFlags & SORTFLAG_UseSorter)
		op = OP_SorterInsert;
	else
		op = OP_IdxInsert;
	sqlite3VdbeAddOp2(v, op, pSort->iECursor, regRecord);
	if (iLimit) {
		int addr;
		int r1 = 0;
		/* Fill the sorter until it contains LIMIT+OFFSET entries.  (The iLimit
		 * register is initialized with value of LIMIT+OFFSET.)  After the sorter
		 * fills up, delete the least entry in the sorter after each insert.
		 * Thus we never hold more than the LIMIT+OFFSET rows in memory at once
		 */
		addr = sqlite3VdbeAddOp1(v, OP_IfNotZero, iLimit);
		VdbeCoverage(v);
		if (pSort->sortFlags & SORTFLAG_DESC) {
			int iNextInstr = sqlite3VdbeCurrentAddr(v) + 1;
			sqlite3VdbeAddOp2(v, OP_Rewind, pSort->iECursor, iNextInstr);
		} else {
			sqlite3VdbeAddOp1(v, OP_Last, pSort->iECursor);
		}
		if (pSort->bOrderedInnerLoop) {
			r1 = ++pParse->nMem;
			sqlite3VdbeAddOp3(v, OP_Column, pSort->iECursor, nExpr,
					  r1);
			VdbeComment((v, "seq"));
		}
		sqlite3VdbeAddOp1(v, OP_Delete, pSort->iECursor);
		if (pSort->bOrderedInnerLoop) {
			/* If the inner loop is driven by an index such that values from
			 * the same iteration of the inner loop are in sorted order, then
			 * immediately jump to the next iteration of an inner loop if the
			 * entry from the current iteration does not fit into the top
			 * LIMIT+OFFSET entries of the sorter.
			 */
			int iBrk = sqlite3VdbeCurrentAddr(v) + 2;
			sqlite3VdbeAddOp3(v, OP_Eq, regBase + nExpr, iBrk, r1);
			sqlite3VdbeChangeP5(v, SQLITE_NULLEQ);
			VdbeCoverage(v);
		}
		sqlite3VdbeJumpHere(v, addr);
	}
}

/*
 * Add code to implement the OFFSET
 */
static void
codeOffset(Vdbe * v,		/* Generate code into this VM */
	   int iOffset,		/* Register holding the offset counter */
	   int iContinue)	/* Jump here to skip the current record */
{
	if (iOffset > 0) {
		sqlite3VdbeAddOp3(v, OP_IfPos, iOffset, iContinue, 1);
		VdbeCoverage(v);
		VdbeComment((v, "OFFSET"));
	}
}

/*
 * Add code that will check to make sure the N registers starting at iMem
 * form a distinct entry.  iTab is a sorting index that holds previously
 * seen combinations of the N values.  A new entry is made in iTab
 * if the current N values are new.
 *
 * A jump to addrRepeat is made and the N+1 values are popped from the
 * stack if the top N elements are not distinct.
 */
static void
codeDistinct(Parse * pParse,	/* Parsing and code generating context */
	     int iTab,		/* A sorting index used to test for distinctness */
	     int addrRepeat,	/* Jump to here if not distinct */
	     int N,		/* Number of elements */
	     int iMem)		/* First element */
{
	Vdbe *v;
	int r1;

	v = pParse->pVdbe;
	r1 = sqlite3GetTempReg(pParse);
	sqlite3VdbeAddOp4Int(v, OP_Found, iTab, addrRepeat, iMem, N);
	VdbeCoverage(v);
	sqlite3VdbeAddOp3(v, OP_MakeRecord, iMem, N, r1);
	sqlite3VdbeAddOp2(v, OP_IdxInsert, iTab, r1);
	sqlite3ReleaseTempReg(pParse, r1);
}

/*
 * This routine generates the code for the inside of the inner loop
 * of a SELECT.
 *
 * If srcTab is negative, then the pEList expressions
 * are evaluated in order to get the data for this row.  If srcTab is
 * zero or more, then data is pulled from srcTab and pEList is used only
 * to get the number of columns and the collation sequence for each column.
 */
static void
selectInnerLoop(Parse * pParse,		/* The parser context */
		Select * p,		/* The complete select statement being coded */
		ExprList * pEList,	/* List of values being extracted */
		int srcTab,		/* Pull data from this table */
		SortCtx * pSort,	/* If not NULL, info on how to process ORDER BY */
		DistinctCtx * pDistinct,	/* If not NULL, info on how to process DISTINCT */
		SelectDest * pDest,	/* How to dispose of the results */
		int iContinue,		/* Jump here to continue with next row */
		int iBreak)		/* Jump here to break out of the inner loop */
{
	Vdbe *v = pParse->pVdbe;
	int i;
	int hasDistinct;		/* True if the DISTINCT keyword is present */
	int eDest = pDest->eDest;	/* How to dispose of results */
	int iParm = pDest->iSDParm;	/* First argument to disposal method */
	int nResultCol;			/* Number of result columns */
	int nPrefixReg = 0;		/* Number of extra registers before regResult */

	/* Usually, regResult is the first cell in an array of memory cells
	 * containing the current result row. In this case regOrig is set to the
	 * same value. However, if the results are being sent to the sorter, the
	 * values for any expressions that are also part of the sort-key are omitted
	 * from this array. In this case regOrig is set to zero.
	 */
	int regResult;		/* Start of memory holding current results */
	int regOrig;		/* Start of memory holding full result (or 0) */

	assert(v);
	assert(pEList != 0);
	hasDistinct = pDistinct ? pDistinct->eTnctType : WHERE_DISTINCT_NOOP;
	if (pSort && pSort->pOrderBy == 0)
		pSort = 0;
	if (pSort == 0 && !hasDistinct) {
		assert(iContinue != 0);
		codeOffset(v, p->iOffset, iContinue);
	}

	/* Pull the requested columns.
	 */
	nResultCol = pEList->nExpr;

	if (pDest->iSdst == 0) {
		if (pSort) {
			nPrefixReg = pSort->pOrderBy->nExpr;
			if (!(pSort->sortFlags & SORTFLAG_UseSorter))
				nPrefixReg++;
			pParse->nMem += nPrefixReg;
		}
		pDest->iSdst = pParse->nMem + 1;
		pParse->nMem += nResultCol;
	} else if (pDest->iSdst + nResultCol > pParse->nMem) {
		/* This is an error condition that can result, for example, when a SELECT
		 * on the right-hand side of an INSERT contains more result columns than
		 * there are columns in the table on the left.  The error will be caught
		 * and reported later.  But we need to make sure enough memory is allocated
		 * to avoid other spurious errors in the meantime.
		 */
		pParse->nMem += nResultCol;
	}
	pDest->nSdst = nResultCol;
	regOrig = regResult = pDest->iSdst;
	if (srcTab >= 0) {
		for (i = 0; i < nResultCol; i++) {
			sqlite3VdbeAddOp3(v, OP_Column, srcTab, i,
					  regResult + i);
			VdbeComment((v, "%s", pEList->a[i].zName));
		}
	} else if (eDest != SRT_Exists) {
		/* If the destination is an EXISTS(...) expression, the actual
		 * values returned by the SELECT are not required.
		 */
		u8 ecelFlags;
		if (eDest == SRT_Mem || eDest == SRT_Output
		    || eDest == SRT_Coroutine) {
			ecelFlags = SQLITE_ECEL_DUP;
		} else {
			ecelFlags = 0;
		}
		if (pSort && hasDistinct == 0 && eDest != SRT_EphemTab
		    && eDest != SRT_Table) {
			/* For each expression in pEList that is a copy of an expression in
			 * the ORDER BY clause (pSort->pOrderBy), set the associated
			 * iOrderByCol value to one more than the index of the ORDER BY
			 * expression within the sort-key that pushOntoSorter() will generate.
			 * This allows the pEList field to be omitted from the sorted record,
			 * saving space and CPU cycles.
			 */
			ecelFlags |= (SQLITE_ECEL_OMITREF | SQLITE_ECEL_REF);
			/* This optimization is temporary disabled. It seems
			 * that it was possible to create table with n columns and
			 * insert tuple with m columns, where m < n. Contrary, Tarantool
			 * doesn't allow to alter the number of fields in tuple
			 * to be inserted. This may be uncommented by delaying the
			 * creation of table until insertion of first tuple,
			 * so that the number of fields in tuple can be precisely calculated.
			 */
			/*for (i = pSort->nOBSat; i < pSort->pOrderBy->nExpr; i++) {
				int j;
				if ((j =
				     pSort->pOrderBy->a[i].u.x.iOrderByCol) >
				    0) {
					pEList->a[j - 1].u.x.iOrderByCol =
					    (u16) (i + 1 - pSort->nOBSat);
				}
			}*/
			regOrig = 0;
			assert(eDest == SRT_Set || eDest == SRT_Mem
			       || eDest == SRT_Coroutine
			       || eDest == SRT_Output);
		}
		nResultCol =
		    sqlite3ExprCodeExprList(pParse, pEList, regResult, 0,
					    ecelFlags);
	}

	/* If the DISTINCT keyword was present on the SELECT statement
	 * and this row has been seen before, then do not make this row
	 * part of the result.
	 */
	if (hasDistinct) {
		switch (pDistinct->eTnctType) {
		case WHERE_DISTINCT_ORDERED:{
				VdbeOp *pOp;	/* No longer required OpenEphemeral instr. */
				int iJump;	/* Jump destination */
				int regPrev;	/* Previous row content */

				/* Allocate space for the previous row */
				regPrev = pParse->nMem + 1;
				pParse->nMem += nResultCol;

				/* Change the OP_OpenEphemeral coded earlier to an OP_Null
				 * sets the MEM_Cleared bit on the first register of the
				 * previous value.  This will cause the OP_Ne below to always
				 * fail on the first iteration of the loop even if the first
				 * row is all NULLs.
				 */
				sqlite3VdbeChangeToNoop(v, pDistinct->addrTnct);
				pOp = sqlite3VdbeGetOp(v, pDistinct->addrTnct);
				pOp->opcode = OP_Null;
				pOp->p1 = 1;
				pOp->p2 = regPrev;

				iJump = sqlite3VdbeCurrentAddr(v) + nResultCol;
				for (i = 0; i < nResultCol; i++) {
					bool is_found;
					uint32_t id;
					struct coll *coll =
					    sql_expr_coll(pParse,
							  pEList->a[i].pExpr,
							  &is_found, &id);
					if (i < nResultCol - 1) {
						sqlite3VdbeAddOp3(v, OP_Ne,
								  regResult + i,
								  iJump,
								  regPrev + i);
						VdbeCoverage(v);
					} else {
						sqlite3VdbeAddOp3(v, OP_Eq,
								  regResult + i,
								  iContinue,
								  regPrev + i);
						VdbeCoverage(v);
					}
					if (is_found) {
						sqlite3VdbeChangeP4(v, -1,
								    (const char *)coll,
								    P4_COLLSEQ);
					}
					sqlite3VdbeChangeP5(v, SQLITE_NULLEQ);
				}
				assert(sqlite3VdbeCurrentAddr(v) == iJump
				       || pParse->db->mallocFailed);
				sqlite3VdbeAddOp3(v, OP_Copy, regResult,
						  regPrev, nResultCol - 1);
				break;
			}

		case WHERE_DISTINCT_UNIQUE:{
				sqlite3VdbeChangeToNoop(v, pDistinct->addrTnct);
				break;
			}

		default:{
				assert(pDistinct->eTnctType ==
				       WHERE_DISTINCT_UNORDERED);
				codeDistinct(pParse, pDistinct->tabTnct,
					     iContinue, nResultCol, regResult);
				break;
			}
		}
		if (pSort == 0) {
			codeOffset(v, p->iOffset, iContinue);
		}
	}

	switch (eDest) {
		/* In this mode, write each query result to the key of the temporary
		 * table iParm.
		 */
#ifndef SQLITE_OMIT_COMPOUND_SELECT
	case SRT_Union:{
			int r1;
			r1 = sqlite3GetTempReg(pParse);
			sqlite3VdbeAddOp3(v, OP_MakeRecord, regResult,
					  nResultCol, r1);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm, r1);
			sqlite3ReleaseTempReg(pParse, r1);
			break;
		}

		/* Construct a record from the query result, but instead of
		 * saving that record, use it as a key to delete elements from
		 * the temporary table iParm.
		 */
	case SRT_Except:{
			sqlite3VdbeAddOp3(v, OP_IdxDelete, iParm, regResult,
					  nResultCol);
			break;
		}
#endif				/* SQLITE_OMIT_COMPOUND_SELECT */

		/* Store the result as data using a unique key.
		 */
	case SRT_Fifo:
	case SRT_DistFifo:
	case SRT_Table:
	case SRT_EphemTab:{
			int r1 = sqlite3GetTempRange(pParse, nPrefixReg + 1);
			testcase(eDest == SRT_Table);
			testcase(eDest == SRT_EphemTab);
			testcase(eDest == SRT_Fifo);
			testcase(eDest == SRT_DistFifo);
			sqlite3VdbeAddOp3(v, OP_MakeRecord, regResult,
					  nResultCol, r1 + nPrefixReg);
			/* Set flag to save memory allocating one by malloc. */
			sqlite3VdbeChangeP5(v, 1);
#ifndef SQLITE_OMIT_CTE
			if (eDest == SRT_DistFifo) {
				/* If the destination is DistFifo, then cursor (iParm+1) is open
				 * on an ephemeral index. If the current row is already present
				 * in the index, do not write it to the output. If not, add the
				 * current row to the index and proceed with writing it to the
				 * output table as well.
				 */
				int addr = sqlite3VdbeCurrentAddr(v) + 6;
				sqlite3VdbeAddOp4Int(v, OP_Found, iParm + 1,
						     addr, r1, 0);
				VdbeCoverage(v);
				sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm + 1,
						  r1);
				assert(pSort == 0);
			}
#endif
			if (pSort) {
				pushOntoSorter(pParse, pSort, p,
					       r1 + nPrefixReg, regResult, 1,
					       nPrefixReg);
			} else {
				int regRec = sqlite3GetTempReg(pParse);
				/* Last column is required for ID. */
				int regCopy = sqlite3GetTempRange(pParse, nResultCol + 1);
				sqlite3VdbeAddOp3(v, OP_NextIdEphemeral, iParm,
						  nResultCol, regCopy + nResultCol);
				/* Positioning ID column to be last in inserted tuple.
				 * NextId -> regCopy + n + 1
				 * Copy [regResult, regResult + n] -> [regCopy, regCopy + n]
				 * MakeRecord -> [regCopy, regCopy + n + 1] -> regRec
				 * IdxInsert -> regRec
				 */
				sqlite3VdbeAddOp3(v, OP_Copy, regResult, regCopy, nResultCol - 1);
				sqlite3VdbeAddOp3(v, OP_MakeRecord, regCopy, nResultCol + 1, regRec);
				/* Set flag to save memory allocating one by malloc. */
				sqlite3VdbeChangeP5(v, 1);
				sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm, regRec);
				sqlite3ReleaseTempReg(pParse, regRec);
				sqlite3ReleaseTempRange(pParse, regCopy, nResultCol + 1);
			}
			sqlite3ReleaseTempRange(pParse, r1, nPrefixReg + 1);
			break;
		}
		/* If we are creating a set for an "expr IN (SELECT ...)" construct,
		 * then there should be a single item on the stack.  Write this
		 * item into the set table with bogus data.
		 */
	case SRT_Set:{
			if (pSort) {
				/* At first glance you would think we could optimize out the
				 * ORDER BY in this case since the order of entries in the set
				 * does not matter.  But there might be a LIMIT clause, in which
				 * case the order does matter.
				 */
				pushOntoSorter(pParse, pSort, p, regResult,
					       regOrig, nResultCol, nPrefixReg);
			} else {
				int r1 = sqlite3GetTempReg(pParse);
				assert(sqlite3Strlen30(pDest->zAffSdst) ==
				       (unsigned int)nResultCol);
				sqlite3VdbeAddOp4(v, OP_MakeRecord, regResult,
						  nResultCol, r1,
						  pDest->zAffSdst, nResultCol);
				sqlite3ExprCacheAffinityChange(pParse,
							       regResult,
							       nResultCol);
				sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm, r1);
				sqlite3ReleaseTempReg(pParse, r1);
			}
			break;
		}

		/* If any row exist in the result set, record that fact and abort.
		 */
	case SRT_Exists:{
			sqlite3VdbeAddOp2(v, OP_Integer, 1, iParm);
			/* The LIMIT clause will terminate the loop for us */
			break;
		}

		/* If this is a scalar select that is part of an expression, then
		 * store the results in the appropriate memory cell or array of
		 * memory cells and break out of the scan loop.
		 */
	case SRT_Mem:{
			if (pSort) {
				assert(nResultCol <= pDest->nSdst);
				pushOntoSorter(pParse, pSort, p, regResult,
					       regOrig, nResultCol, nPrefixReg);
			} else {
				assert(nResultCol == pDest->nSdst);
				assert(regResult == iParm);
				/* The LIMIT clause will jump out of the loop for us */
			}
			break;
		}

	case SRT_Coroutine:	/* Send data to a co-routine */
	case SRT_Output:{	/* Return the results */
			testcase(eDest == SRT_Coroutine);
			testcase(eDest == SRT_Output);
			if (pSort) {
				pushOntoSorter(pParse, pSort, p, regResult,
					       regOrig, nResultCol, nPrefixReg);
			} else if (eDest == SRT_Coroutine) {
				sqlite3VdbeAddOp1(v, OP_Yield, pDest->iSDParm);
			} else {
				sqlite3VdbeAddOp2(v, OP_ResultRow, regResult,
						  nResultCol);
				sqlite3ExprCacheAffinityChange(pParse,
							       regResult,
							       nResultCol);
			}
			break;
		}

#ifndef SQLITE_OMIT_CTE
		/* Write the results into a priority queue that is order according to
		 * pDest->pOrderBy (in pSO).  pDest->iSDParm (in iParm) is the cursor for an
		 * index with pSO->nExpr+2 columns.  Build a key using pSO for the first
		 * pSO->nExpr columns, then make sure all keys are unique by adding a
		 * final OP_Sequence column.  The last column is the record as a blob.
		 */
	case SRT_DistQueue:
	case SRT_Queue:{
			int nKey;
			int r1, r2, r3;
			int addrTest = 0;
			ExprList *pSO;
			pSO = pDest->pOrderBy;
			assert(pSO);
			nKey = pSO->nExpr;
			r1 = sqlite3GetTempReg(pParse);
			r2 = sqlite3GetTempRange(pParse, nKey + 2);
			r3 = r2 + nKey + 1;
			if (eDest == SRT_DistQueue) {
				/* If the destination is DistQueue, then cursor (iParm+1) is open
				 * on a second ephemeral index that holds all values every previously
				 * added to the queue.
				 */
				addrTest =
				    sqlite3VdbeAddOp4Int(v, OP_Found, iParm + 1,
							 0, regResult,
							 nResultCol);
				VdbeCoverage(v);
			}
			sqlite3VdbeAddOp3(v, OP_MakeRecord, regResult,
					  nResultCol, r3);
			if (eDest == SRT_DistQueue) {
				sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm + 1,
						  r3);
			}
			for (i = 0; i < nKey; i++) {
				sqlite3VdbeAddOp2(v, OP_SCopy,
						  regResult +
						  pSO->a[i].u.x.iOrderByCol - 1,
						  r2 + i);
			}
			sqlite3VdbeAddOp2(v, OP_Sequence, iParm, r2 + nKey);
			sqlite3VdbeAddOp3(v, OP_MakeRecord, r2, nKey + 2, r1);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm, r1);
			if (addrTest)
				sqlite3VdbeJumpHere(v, addrTest);
			sqlite3ReleaseTempReg(pParse, r1);
			sqlite3ReleaseTempRange(pParse, r2, nKey + 2);
			break;
		}
#endif				/* SQLITE_OMIT_CTE */

		/* Discard the results.  This is used for SELECT statements inside
		 * the body of a TRIGGER.  The purpose of such selects is to call
		 * user-defined functions that have side effects.  We do not care
		 * about the actual results of the select.
		 */
	default:{
			assert(eDest == SRT_Discard);
			break;
		}
	}

	/* Jump to the end of the loop if the LIMIT is reached.  Except, if
	 * there is a sorter, in which case the sorter has already limited
	 * the output for us.
	 */
	if (pSort == 0 && p->iLimit) {
		sqlite3VdbeAddOp2(v, OP_DecrJumpZero, p->iLimit, iBreak);
		VdbeCoverage(v);
	}
}

static struct key_def *
sql_expr_list_to_key_def(struct Parse *parse, struct ExprList *list, int start)
{
	int expr_count = list->nExpr;
	struct key_def *def = key_def_new(expr_count);
	if (def == NULL) {
		sqlite3OomFault(parse->db);
		return NULL;
	}
	struct ExprList_item *item = list->a + start;
	for (int i = start; i < expr_count; ++i, ++item) {
		bool unused;
		uint32_t id;
		struct coll *coll =
			sql_expr_coll(parse, item->pExpr, &unused, &id);
		key_def_set_part(def, i-start, i-start, FIELD_TYPE_SCALAR,
				 ON_CONFLICT_ACTION_ABORT, coll, id,
				 item->sort_order);
	}
	return def;
}

/*
 * Name of the connection operator, used for error messages.
 */
static const char *
selectOpName(int id)
{
	char *z;
	switch (id) {
	case TK_ALL:
		z = "UNION ALL";
		break;
	case TK_INTERSECT:
		z = "INTERSECT";
		break;
	case TK_EXCEPT:
		z = "EXCEPT";
		break;
	default:
		z = "UNION";
		break;
	}
	return z;
}

/*
 * Unless an "EXPLAIN QUERY PLAN" command is being processed, this function
 * is a no-op. Otherwise, it adds a single row of output to the EQP result,
 * where the caption is of the form:
 *
 *   "USE TEMP B-TREE FOR xxx"
 *
 * where xxx is one of "DISTINCT", "ORDER BY" or "GROUP BY". Exactly which
 * is determined by the zUsage argument.
 */
static void
explainTempTable(Parse * pParse, const char *zUsage)
{
	if (pParse->explain == 2) {
		Vdbe *v = pParse->pVdbe;
		char *zMsg =
		    sqlite3MPrintf(pParse->db, "USE TEMP B-TREE FOR %s",
				   zUsage);
		sqlite3VdbeAddOp4(v, OP_Explain, pParse->iSelectId, 0, 0, zMsg,
				  P4_DYNAMIC);
	}
}

#if !defined(SQLITE_OMIT_COMPOUND_SELECT)
/*
 * Unless an "EXPLAIN QUERY PLAN" command is being processed, this function
 * is a no-op. Otherwise, it adds a single row of output to the EQP result,
 * where the caption is of one of the two forms:
 *
 *   "COMPOSITE SUBQUERIES iSub1 and iSub2 (op)"
 *   "COMPOSITE SUBQUERIES iSub1 and iSub2 USING TEMP B-TREE (op)"
 *
 * where iSub1 and iSub2 are the integers passed as the corresponding
 * function parameters, and op is the text representation of the parameter
 * of the same name. The parameter "op" must be one of TK_UNION, TK_EXCEPT,
 * TK_INTERSECT or TK_ALL. The first form is used if argument bUseTmp is
 * false, or the second form if it is true.
 */
static void
explainComposite(Parse * pParse,	/* Parse context */
		 int op,	/* One of TK_UNION, TK_EXCEPT etc. */
		 int iSub1,	/* Subquery id 1 */
		 int iSub2,	/* Subquery id 2 */
		 int bUseTmp	/* True if a temp table was used */
    )
{
	assert(op == TK_UNION || op == TK_EXCEPT || op == TK_INTERSECT
	       || op == TK_ALL);
	if (pParse->explain == 2) {
		Vdbe *v = pParse->pVdbe;
		char *zMsg =
		    sqlite3MPrintf(pParse->db,
				   "COMPOUND SUBQUERIES %d AND %d %s(%s)",
				   iSub1, iSub2,
				   bUseTmp ? "USING TEMP B-TREE " : "",
				   selectOpName(op)
		    );
		sqlite3VdbeAddOp4(v, OP_Explain, pParse->iSelectId, 0, 0, zMsg,
				  P4_DYNAMIC);
	}
}
#else
/* No-op versions of the explainXXX() functions and macros. */
#define explainComposite(v,w,x,y,z)
#endif

/*
 * If the inner loop was generated using a non-null pOrderBy argument,
 * then the results were placed in a sorter.  After the loop is terminated
 * we need to run the sorter and output the results.  The following
 * routine generates the code needed to do that.
 */
static void
generateSortTail(Parse * pParse,	/* Parsing context */
		 Select * p,		/* The SELECT statement */
		 SortCtx * pSort,	/* Information on the ORDER BY clause */
		 int nColumn,		/* Number of columns of data */
		 SelectDest * pDest)	/* Write the sorted results here */
{
	Vdbe *v = pParse->pVdbe;	/* The prepared statement */
	int addrBreak = pSort->labelDone;	/* Jump here to exit loop */
	int addrContinue = sqlite3VdbeMakeLabel(v);	/* Jump here for next cycle */
	int addr;
	int addrOnce = 0;
	int iTab;
	ExprList *pOrderBy = pSort->pOrderBy;
	int eDest = pDest->eDest;
	int iParm = pDest->iSDParm;
	int regRow;
	int regTupleid;
	int iCol;
	int nKey;
	int iSortTab;		/* Sorter cursor to read from */
	int nSortData;		/* Trailing values to read from sorter */
	int i;
	int bSeq;		/* True if sorter record includes seq. no. */
	struct ExprList_item *aOutEx = p->pEList->a;

	assert(addrBreak < 0);
	if (pSort->labelBkOut) {
		sqlite3VdbeAddOp2(v, OP_Gosub, pSort->regReturn,
				  pSort->labelBkOut);
		sqlite3VdbeGoto(v, addrBreak);
		sqlite3VdbeResolveLabel(v, pSort->labelBkOut);
	}
	iTab = pSort->iECursor;
	if (eDest == SRT_Output || eDest == SRT_Coroutine || eDest == SRT_Mem) {
		regTupleid = 0;
		regRow = pDest->iSdst;
		nSortData = nColumn;
	} else {
		regTupleid = sqlite3GetTempReg(pParse);
		regRow = sqlite3GetTempRange(pParse, nColumn);
		nSortData = nColumn;
	}
	nKey = pOrderBy->nExpr - pSort->nOBSat;
	if (pSort->sortFlags & SORTFLAG_UseSorter) {
		int regSortOut = ++pParse->nMem;
		iSortTab = pParse->nTab++;
		if (pSort->labelBkOut) {
			addrOnce = sqlite3VdbeAddOp0(v, OP_Once);
			VdbeCoverage(v);
		}
		sqlite3VdbeAddOp3(v, OP_OpenPseudo, iSortTab, regSortOut,
				  nKey + 1 + nSortData);
		if (addrOnce)
			sqlite3VdbeJumpHere(v, addrOnce);
		addr = 1 + sqlite3VdbeAddOp2(v, OP_SorterSort, iTab, addrBreak);
		VdbeCoverage(v);
		codeOffset(v, p->iOffset, addrContinue);
		sqlite3VdbeAddOp3(v, OP_SorterData, iTab, regSortOut, iSortTab);
		bSeq = 0;
	} else {
		/* In case of DESC sorting order data should be taken from
		 * the end of table. */
		int opPositioning = (pSort->sortFlags & SORTFLAG_DESC) ?
				    OP_Last : OP_Sort;
		addr = 1 + sqlite3VdbeAddOp2(v, opPositioning, iTab, addrBreak);
		VdbeCoverage(v);
		codeOffset(v, p->iOffset, addrContinue);
		iSortTab = iTab;
		bSeq = 1;
	}
	for (i = 0, iCol = nKey + bSeq; i < nSortData; i++) {
		int iRead;
		if (aOutEx[i].u.x.iOrderByCol) {
			iRead = aOutEx[i].u.x.iOrderByCol - 1;
		} else {
			iRead = iCol++;
		}
		sqlite3VdbeAddOp3(v, OP_Column, iSortTab, iRead, regRow + i);
		VdbeComment((v, "%s",
			     aOutEx[i].zName ? aOutEx[i].zName : aOutEx[i].
			     zSpan));
	}
	switch (eDest) {
	case SRT_Table:
	case SRT_EphemTab: {
			int regCopy = sqlite3GetTempRange(pParse,  nColumn);
			sqlite3VdbeAddOp3(v, OP_NextIdEphemeral, iParm, nColumn, regTupleid);
			sqlite3VdbeAddOp3(v, OP_Copy, regRow, regCopy, nSortData - 1);
			sqlite3VdbeAddOp3(v, OP_MakeRecord, regCopy, nColumn + 1, regRow);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm, regRow);
			sqlite3ReleaseTempReg(pParse, regCopy);
			break;
		}
	case SRT_Set:{
			assert((unsigned int)nColumn ==
			       sqlite3Strlen30(pDest->zAffSdst));
			sqlite3VdbeAddOp4(v, OP_MakeRecord, regRow, nColumn,
					  regTupleid, pDest->zAffSdst, nColumn);
			sqlite3ExprCacheAffinityChange(pParse, regRow, nColumn);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, iParm, regTupleid);
			break;
		}
	case SRT_Mem:{
			/* The LIMIT clause will terminate the loop for us */
			break;
		}
	default: {
			assert(eDest == SRT_Output || eDest == SRT_Coroutine);
			testcase(eDest == SRT_Output);
			testcase(eDest == SRT_Coroutine);
			if (eDest == SRT_Output) {
				sqlite3VdbeAddOp2(v, OP_ResultRow, pDest->iSdst,
						  nColumn);
				sqlite3ExprCacheAffinityChange(pParse,
							       pDest->iSdst,
							       nColumn);
			} else {
				sqlite3VdbeAddOp1(v, OP_Yield, pDest->iSDParm);
			}
			break;
		}
	}
	if (regTupleid) {
		if (eDest == SRT_Set) {
			sqlite3ReleaseTempRange(pParse, regRow, nColumn);
		} else {
			sqlite3ReleaseTempReg(pParse, regRow);
		}
		sqlite3ReleaseTempReg(pParse, regTupleid);
	}
	/* The bottom of the loop
	 */
	sqlite3VdbeResolveLabel(v, addrContinue);
	if (pSort->sortFlags & SORTFLAG_UseSorter) {
		sqlite3VdbeAddOp2(v, OP_SorterNext, iTab, addr);
		VdbeCoverage(v);
	} else {
		/* In case of DESC sorting cursor should move backward. */
		int opPositioning = (pSort->sortFlags & SORTFLAG_DESC) ?
				    OP_Prev : OP_Next;
		sqlite3VdbeAddOp2(v, opPositioning, iTab, addr);
		VdbeCoverage(v);
	}
	if (pSort->regReturn)
		sqlite3VdbeAddOp1(v, OP_Return, pSort->regReturn);
	sqlite3VdbeResolveLabel(v, addrBreak);
}

/*
 * Return a pointer to a string containing the 'declaration type' of the
 * expression pExpr. The string may be treated as static by the caller.
 *
 * Also try to estimate the size of the returned value and return that
 * result in *pEstWidth.
 *
 * The declaration type is the exact datatype definition extracted from the
 * original CREATE TABLE statement if the expression is a column.
 * Exactly when an expression is considered a column can be complex
 * in the presence of subqueries. The result-set expression in all
 * of the following SELECT statements is considered a column by this function.
 *
 *   SELECT col FROM tbl;
 *   SELECT (SELECT col FROM tbl;
 *   SELECT (SELECT col FROM tbl);
 *   SELECT abc FROM (SELECT col AS abc FROM tbl);
 *
 * The declaration type for any expression other than a column is NULL.
 *
 * This routine has either 3 or 6 parameters depending on whether or not
 * the SQLITE_ENABLE_COLUMN_METADATA compile-time option is used.
 */
#ifdef SQLITE_ENABLE_COLUMN_METADATA
#define columnType(A,B,C,D) columnTypeImpl(A,B,C,D)
#else				/* if !defined(SQLITE_ENABLE_COLUMN_METADATA) */
#define columnType(A,B,C,D) columnTypeImpl(A,B)
#endif
static enum field_type
columnTypeImpl(NameContext * pNC, Expr * pExpr
#ifdef SQLITE_ENABLE_COLUMN_METADATA
	       , const char **pzOrigCol,
#endif
)
{
	enum field_type column_type = FIELD_TYPE_SCALAR;
	int j;
#ifdef SQLITE_ENABLE_COLUMN_METADATA
	char const *zOrigTab = 0;
	char const *zOrigCol = 0;
#endif

	assert(pExpr != 0);
	assert(pNC->pSrcList != 0);
	switch (pExpr->op) {
	case TK_AGG_COLUMN:
	case TK_COLUMN:{
			/* The expression is a column. Locate the table the column is being
			 * extracted from in NameContext.pSrcList. This table may be real
			 * database table or a subquery.
			 */
			Table *pTab = 0;	/* Table structure column is extracted from */
			Select *pS = 0;	/* Select the column is extracted from */
			int iCol = pExpr->iColumn;	/* Index of column in pTab */
			testcase(pExpr->op == TK_AGG_COLUMN);
			testcase(pExpr->op == TK_COLUMN);
			while (pNC && !pTab) {
				SrcList *pTabList = pNC->pSrcList;
				for (j = 0;
				     j < pTabList->nSrc
				     && pTabList->a[j].iCursor != pExpr->iTable;
				     j++) ;
				if (j < pTabList->nSrc) {
					pTab = pTabList->a[j].pTab;
					pS = pTabList->a[j].pSelect;
				} else {
					pNC = pNC->pNext;
				}
			}

			if (pTab == 0) {
				/* At one time, code such as "SELECT new.x" within a trigger would
				 * cause this condition to run.  Since then, we have restructured how
				 * trigger code is generated and so this condition is no longer
				 * possible. However, it can still be true for statements like
				 * the following:
				 *
				 *   CREATE TABLE t1(col INTEGER);
				 *   SELECT (SELECT t1.col) FROM FROM t1;
				 *
				 * when columnType() is called on the expression "t1.col" in the
				 * sub-select. In this case, set the column type to NULL, even
				 * though it should really be "INTEGER".
				 *
				 * This is not a problem, as the column type of "t1.col" is never
				 * used. When columnType() is called on the expression
				 * "(SELECT t1.col)", the correct type is returned (see the TK_SELECT
				 * branch below.
				 */
				break;
			}

			assert(pTab && pExpr->space_def == pTab->def);
			if (pS) {
				/* The "table" is actually a sub-select or a view in the FROM clause
				 * of the SELECT statement. Return the declaration type and origin
				 * data for the result-set column of the sub-select.
				 */
				assert(iCol >= 0);
				if (ALWAYS(iCol < pS->pEList->nExpr)) {
					/* The ALWAYS() is because
					 * iCol>=pS->pEList->nExpr will have been
					 * caught already by name resolution.
					 */
					NameContext sNC;
					Expr *p = pS->pEList->a[iCol].pExpr;
					sNC.pSrcList = pS->pSrc;
					sNC.pNext = pNC;
					sNC.pParse = pNC->pParse;
					column_type =
					    columnType(&sNC, p, &zOrigTab,
						       &zOrigCol);
				}
			} else if (pTab->pSchema) {
				/* A real table */
				assert(!pS);
				assert(iCol >= 0 &&
				       iCol < (int)pTab->def->field_count);
#ifdef SQLITE_ENABLE_COLUMN_METADATA
				zOrigCol = pTab->def->fields[iCol].name;
				zType = pTab->def->fields[iCol].type;
				zOrigTab = pTab->zName;
#else
				column_type = pTab->def->fields[iCol].type;
#endif
			}
			break;
		}
	case TK_SELECT:{
			/* The expression is a sub-select. Return the declaration type and
			 * origin info for the single column in the result set of the SELECT
			 * statement.
			 */
			NameContext sNC;
			Select *pS = pExpr->x.pSelect;
			Expr *p = pS->pEList->a[0].pExpr;
			assert(ExprHasProperty(pExpr, EP_xIsSelect));
			sNC.pSrcList = pS->pSrc;
			sNC.pNext = pNC;
			sNC.pParse = pNC->pParse;
			column_type =
			    columnType(&sNC, p, &zOrigTab, &zOrigCol);
			break;
		}
	}

#ifdef SQLITE_ENABLE_COLUMN_METADATA
	if (pzOrigTab) {
		assert(pzOrigTab && pzOrigCol);
		*pzOrigTab = zOrigTab;
		*pzOrigCol = zOrigCol;
	}
#endif
	return column_type;
}

/*
 * Generate code that will tell the VDBE the names of columns
 * in the result set.  This information is used to provide the
 * azCol[] values in the callback.
 */
static void
generateColumnNames(Parse * pParse,	/* Parser context */
		    SrcList * pTabList,	/* List of tables */
		    ExprList * pEList)	/* Expressions defining the result set */
{
	Vdbe *v = pParse->pVdbe;
	int i, j;
	sqlite3 *db = pParse->db;
	int fullNames, shortNames;
	struct session *user_session = current_session();
	/* If this is an EXPLAIN, skip this step */
	if (pParse->explain) {
		return;
	}

	if (pParse->colNamesSet || db->mallocFailed)
		return;
	assert(v != 0);
	assert(pTabList != 0);
	pParse->colNamesSet = 1;
	fullNames = (user_session->sql_flags & SQLITE_FullColNames) != 0;
	shortNames = (user_session->sql_flags & SQLITE_ShortColNames) != 0;
	sqlite3VdbeSetNumCols(v, pEList->nExpr);
	for (i = 0; i < pEList->nExpr; i++) {
		Expr *p;
		p = pEList->a[i].pExpr;
		if (NEVER(p == 0))
			continue;
		if (pEList->a[i].zName) {
			char *zName = pEList->a[i].zName;
			sqlite3VdbeSetColName(v, i, COLNAME_NAME, zName,
					      SQLITE_TRANSIENT);
		} else if (p->op == TK_COLUMN || p->op == TK_AGG_COLUMN) {
			Table *pTab;
			char *zCol;
			int iCol = p->iColumn;
			for (j = 0; ALWAYS(j < pTabList->nSrc); j++) {
				if (pTabList->a[j].iCursor == p->iTable)
					break;
			}
			assert(j < pTabList->nSrc);
			pTab = pTabList->a[j].pTab;
			assert(iCol >= 0 && iCol < (int)pTab->def->field_count);
			zCol = pTab->def->fields[iCol].name;
			if (!shortNames && !fullNames) {
				sqlite3VdbeSetColName(v, i, COLNAME_NAME,
						      sqlite3DbStrDup(db,
								      pEList->a[i].zSpan),
						      SQLITE_DYNAMIC);
			} else if (fullNames) {
				char *zName = 0;
				zName =
				    sqlite3MPrintf(db, "%s.%s", pTab->def->name,
						   zCol);
				sqlite3VdbeSetColName(v, i, COLNAME_NAME, zName,
						      SQLITE_DYNAMIC);
			} else {
				sqlite3VdbeSetColName(v, i, COLNAME_NAME, zCol,
						      SQLITE_TRANSIENT);
			}
		} else {
			const char *z = pEList->a[i].zSpan;
			z = z == 0 ? sqlite3MPrintf(db, "column%d",
						    i + 1) : sqlite3DbStrDup(db,
									     z);
			sqlite3VdbeSetColName(v, i, COLNAME_NAME, z,
					      SQLITE_DYNAMIC);
		}
	}
}

/*
 * Given an expression list (which is really the list of expressions
 * that form the result set of a SELECT statement) compute appropriate
 * column names for a table that would hold the expression list.
 *
 * All column names will be unique.
 *
 * Only the column names are computed.  Column.zType, Column.zColl,
 * and other fields of Column are zeroed.
 *
 * Return SQLITE_OK on success.  If a memory allocation error occurs,
 * store NULL in *paCol and 0 in *pnCol and return SQLITE_NOMEM.
 */
int
sqlite3ColumnsFromExprList(Parse * parse, ExprList * expr_list, Table *table)
{
	/* Database connection */
	sqlite3 *db = parse->db;
	u32 cnt;		/* Index added to make the name unique */
	Expr *p;		/* Expression for a single result column */
	char *zName;		/* Column name */
	int nName;		/* Size of name in zName[] */
	Hash ht;		/* Hash table of column names */

	sqlite3HashInit(&ht);
	uint32_t column_count =
		expr_list != NULL ? (uint32_t)expr_list->nExpr : 0;
	/*
	 * This should be a table without resolved columns.
	 * sqlite3ViewGetColumnNames could use it to resolve
	 * names for existing table.
	 */
	assert(table->def->fields == NULL);
	struct region *region = &parse->region;
	table->def->fields =
		region_alloc(region,
			     column_count * sizeof(table->def->fields[0]));
	if (table->def->fields == NULL) {
		sqlite3OomFault(db);
		goto cleanup;
	}
	for (uint32_t i = 0; i < column_count; i++) {
		memcpy(&table->def->fields[i], &field_def_default,
		       sizeof(field_def_default));
		table->def->fields[i].nullable_action = ON_CONFLICT_ACTION_NONE;
		table->def->fields[i].is_nullable = true;
	}
	table->def->field_count = column_count;

	for (uint32_t i = 0; i < column_count; i++) {
		/* Get an appropriate name for the column
		 */
		p = sqlite3ExprSkipCollate(expr_list->a[i].pExpr);
		if ((zName = expr_list->a[i].zName) != 0) {
			/* If the column contains an "AS <name>" phrase, use <name> as the name */
		} else {
			Expr *pColExpr = p;	/* The expression that is the result column name */
			struct space_def *space_def = NULL;
			while (pColExpr->op == TK_DOT) {
				pColExpr = pColExpr->pRight;
				assert(pColExpr != 0);
			}
			if (pColExpr->op == TK_COLUMN
			    && ALWAYS(pColExpr->space_def != NULL)) {
				/* For columns use the column name name */
				int iCol = pColExpr->iColumn;
				assert(iCol >= 0);
				space_def = pColExpr->space_def;
				zName = space_def->fields[iCol].name;
			} else if (pColExpr->op == TK_ID) {
				assert(!ExprHasProperty(pColExpr, EP_IntValue));
				zName = pColExpr->u.zToken;
			} else {
				/* Use the original text of the column expression as its name */
				zName = expr_list->a[i].zSpan;
			}
		}
		zName = sqlite3MPrintf(db, "%s", zName);

		/* Make sure the column name is unique.  If the name is not unique,
		 * append an integer to the name so that it becomes unique.
		 */
		cnt = 0;
		while (zName && sqlite3HashFind(&ht, zName) != 0) {
			nName = sqlite3Strlen30(zName);
			if (nName > 0) {
				int j;
				for (j = nName - 1;
				     j > 0 && sqlite3Isdigit(zName[j]); j--);
				if (zName[j] == ':')
					nName = j;
			}
			zName =
			    sqlite3MPrintf(db, "%.*z:%u", nName, zName, ++cnt);
			if (cnt > 3)
				sqlite3_randomness(sizeof(cnt), &cnt);
		}
		size_t name_len = strlen(zName);
		void *field = &table->def->fields[i];
		if (zName != NULL &&
		    sqlite3HashInsert(&ht, zName, field) == field)
			sqlite3OomFault(db);
		table->def->fields[i].name =
			region_alloc(region, name_len + 1);
		if (table->def->fields[i].name == NULL) {
			sqlite3OomFault(db);
			goto cleanup;
		} else {
			memcpy(table->def->fields[i].name, zName, name_len);
			table->def->fields[i].name[name_len] = '\0';
		}
	}
cleanup:
	sqlite3HashClear(&ht);
	int rc = db->mallocFailed ? SQLITE_NOMEM_BKPT : SQLITE_OK;
	if (rc != SQLITE_OK) {
		/*
		 * pTable->def could be not temporal in
		 * sqlite3ViewGetColumnNames so we need clean-up.
		 */
		table->def->fields = NULL;
		table->def->field_count = 0;
		rc = SQLITE_NOMEM_BKPT;
	}
	return rc;

}

/*
 * Add type and collation information to a column list based on
 * a SELECT statement.
 *
 * The column list presumably came from selectColumnNamesFromExprList().
 * The column list has only names, not types or collations.  This
 * routine goes through and adds the types and collations.
 *
 * This routine requires that all identifiers in the SELECT
 * statement be resolved.
 */
void
sqlite3SelectAddColumnTypeAndCollation(Parse * pParse,		/* Parsing contexts */
				       Table * pTab,		/* Add column type information to this table */
				       Select * pSelect)	/* SELECT used to determine types and collations */
{
	sqlite3 *db = pParse->db;
	NameContext sNC;
	Expr *p;
	struct ExprList_item *a;

	assert(pSelect != 0);
	assert((pSelect->selFlags & SF_Resolved) != 0);
	assert((int)pTab->def->field_count == pSelect->pEList->nExpr ||
	       db->mallocFailed);
	if (db->mallocFailed)
		return;
	memset(&sNC, 0, sizeof(sNC));
	sNC.pSrcList = pSelect->pSrc;
	a = pSelect->pEList->a;
	for (uint32_t i = 0; i < pTab->def->field_count; i++) {
		enum field_type type;
		p = a[i].pExpr;
		type = columnType(&sNC, p, 0, 0);
		pTab->def->fields[i].type = type;

		char affinity = sqlite3ExprAffinity(p);
		if (affinity == 0)
			affinity = AFFINITY_BLOB;
		pTab->def->fields[i].affinity = affinity;
		bool is_found;
		uint32_t coll_id;
		if (pTab->def->fields[i].coll_id == COLL_NONE &&
		    sql_expr_coll(pParse, p, &is_found, &coll_id) && is_found)
			pTab->def->fields[i].coll_id = coll_id;
	}
}

/*
 * Given a SELECT statement, generate a Table structure that describes
 * the result set of that SELECT.
 */
Table *
sqlite3ResultSetOfSelect(Parse * pParse, Select * pSelect)
{
	sqlite3 *db = pParse->db;
	uint32_t savedFlags;
	struct session *user_session = current_session();

	savedFlags = user_session->sql_flags;
	user_session->sql_flags |= ~SQLITE_FullColNames;
	user_session->sql_flags &= SQLITE_ShortColNames;
	sqlite3SelectPrep(pParse, pSelect, 0);
	if (pParse->nErr)
		return 0;
	while (pSelect->pPrior)
		pSelect = pSelect->pPrior;
	user_session->sql_flags = savedFlags;
	Table *table = sql_ephemeral_table_new(pParse, NULL);
	if (table == NULL)
		return 0;
	/* The sqlite3ResultSetOfSelect() is only used n contexts where lookaside
	 * is disabled
	 */
	assert(db->lookaside.bDisable);
	table->nTabRef = 1;
	table->tuple_log_count = DEFAULT_TUPLE_LOG_COUNT;
	assert(sqlite3LogEst(DEFAULT_TUPLE_COUNT) == DEFAULT_TUPLE_LOG_COUNT);
	sqlite3ColumnsFromExprList(pParse, pSelect->pEList, table);
	sqlite3SelectAddColumnTypeAndCollation(pParse, table, pSelect);
	if (db->mallocFailed) {
		sqlite3DeleteTable(db, table);
		return 0;
	}
	return table;
}

/*
 * Get a VDBE for the given parser context.  Create a new one if necessary.
 * If an error occurs, return NULL and leave a message in pParse.
 */
static SQLITE_NOINLINE Vdbe *
allocVdbe(Parse * pParse)
{
	Vdbe *v = pParse->pVdbe = sqlite3VdbeCreate(pParse);
	if (v == NULL)
		return NULL;
	sqlite3VdbeAddOp2(v, OP_Init, 0, 1);
	if (pParse->pToplevel == 0
	    && OptimizationEnabled(pParse->db, SQLITE_FactorOutConst)
	    ) {
		pParse->okConstFactor = 1;
	}
	return v;
}

Vdbe *
sqlite3GetVdbe(Parse * pParse)
{
	Vdbe *v = pParse->pVdbe;
	return v ? v : allocVdbe(pParse);
}

/*
 * Compute the iLimit and iOffset fields of the SELECT based on the
 * pLimit and pOffset expressions.  pLimit and pOffset hold the expressions
 * that appear in the original SQL statement after the LIMIT and OFFSET
 * keywords.  Or NULL if those keywords are omitted. iLimit and iOffset
 * are the integer memory register numbers for counters used to compute
 * the limit and offset.  If there is no limit and/or offset, then
 * iLimit and iOffset are negative.
 *
 * This routine changes the values of iLimit and iOffset only if
 * a limit or offset is defined by pLimit and pOffset.  iLimit and
 * iOffset should have been preset to appropriate default values (zero)
 * prior to calling this routine.
 *
 * The iOffset register (if it exists) is initialized to the value
 * of the OFFSET.  The iLimit register is initialized to LIMIT.  Register
 * iOffset+1 is initialized to LIMIT+OFFSET.
 *
 * Only if pLimit!=0 or pOffset!=0 do the limit registers get
 * redefined.  The UNION ALL operator uses this property to force
 * the reuse of the same limit and offset registers across multiple
 * SELECT statements.
 */
static void
computeLimitRegisters(Parse * pParse, Select * p, int iBreak)
{
	Vdbe *v = 0;
	int iLimit = 0;
	int iOffset;
	int n;
	if (p->iLimit)
		return;

	/*
	 * "LIMIT -1" always shows all rows.  There is some
	 * controversy about what the correct behavior should be.
	 * The current implementation interprets "LIMIT 0" to mean
	 * no rows.
	 */
	sqlite3ExprCacheClear(pParse);
	assert(p->pOffset == 0 || p->pLimit != 0);
	if (p->pLimit) {
		if((p->pLimit->flags & EP_Collate) != 0 ||
		   (p->pOffset != NULL &&
		   (p->pOffset->flags & EP_Collate) != 0)) {
			sqlite3ErrorMsg(pParse, "near \"COLLATE\": "\
						"syntax error");
			return;
		}
		p->iLimit = iLimit = ++pParse->nMem;
		v = sqlite3GetVdbe(pParse);
		assert(v != 0);
		if (sqlite3ExprIsInteger(p->pLimit, &n)) {
			sqlite3VdbeAddOp2(v, OP_Integer, n, iLimit);
			VdbeComment((v, "LIMIT counter"));
			if (n == 0) {
				sqlite3VdbeGoto(v, iBreak);
			} else if (n >= 0
				   && p->nSelectRow > sqlite3LogEst((u64) n)) {
				p->nSelectRow = sqlite3LogEst((u64) n);
				p->selFlags |= SF_FixedLimit;
			}
		} else {
			sqlite3ExprCode(pParse, p->pLimit, iLimit);
			sqlite3VdbeAddOp1(v, OP_MustBeInt, iLimit);
			VdbeCoverage(v);
			VdbeComment((v, "LIMIT counter"));
			sqlite3VdbeAddOp2(v, OP_IfNot, iLimit, iBreak);
			VdbeCoverage(v);
		}
		if ((p->selFlags & SF_SingleRow) != 0) {
			if (ExprHasProperty(p->pLimit, EP_System)) {
				/*
				 * Indirect LIMIT 1 is allowed only for
				 * requests returning only 1 row.
				 * To test this, we change LIMIT 1 to
				 * LIMIT 2 and will look up LIMIT 1 overflow
				 * at the sqlite3Select end.
				 */
				sqlite3VdbeAddOp2(v, OP_Integer, 2, iLimit);
			} else {
				/*
				 * User-defined complex limit for subquery
				 * could be only 1 as resulting value.
				 */
				int r1 = sqlite3GetTempReg(pParse);
				sqlite3VdbeAddOp2(v, OP_Integer, 1, r1);
				int no_err = sqlite3VdbeMakeLabel(v);
				sqlite3VdbeAddOp3(v, OP_Eq, iLimit, no_err, r1);
				const char *error =
					"SQL error: Expression subquery could "
					"be limited only with 1";
				sqlite3VdbeAddOp4(v, OP_Halt,
						  SQL_TARANTOOL_ERROR,
						  0, 0, error, P4_STATIC);
				sqlite3VdbeChangeP5(v, ER_SQL_EXECUTE);
				sqlite3VdbeResolveLabel(v, no_err);
				sqlite3ReleaseTempReg(pParse, r1);

				/* Runtime checks are no longer needed. */
				p->selFlags &= ~SF_SingleRow;
			}
		}
		if (p->pOffset) {
			p->iOffset = iOffset = ++pParse->nMem;
			pParse->nMem++;	/* Allocate an extra register for limit+offset */
			sqlite3ExprCode(pParse, p->pOffset, iOffset);
			sqlite3VdbeAddOp1(v, OP_MustBeInt, iOffset);
			VdbeCoverage(v);
			VdbeComment((v, "OFFSET counter"));
			sqlite3VdbeAddOp3(v, OP_OffsetLimit, iLimit,
					  iOffset + 1, iOffset);
			VdbeComment((v, "LIMIT+OFFSET"));
		}
	}
}

#ifndef SQLITE_OMIT_COMPOUND_SELECT
static struct coll *
multi_select_coll_seq_r(Parse *parser, Select *p, int n, bool *is_found,
			uint32_t *coll_id)
{
	struct coll *coll;
	if (p->pPrior != NULL) {
		coll = multi_select_coll_seq_r(parser, p->pPrior, n, is_found,
					       coll_id);
	} else {
		coll = NULL;
		*coll_id = COLL_NONE;
	}
	assert(n >= 0);
	/* iCol must be less than p->pEList->nExpr.  Otherwise an error would
	 * have been thrown during name resolution and we would not have gotten
	 * this far
	 */
	if (!*is_found && ALWAYS(n < p->pEList->nExpr)) {
		coll = sql_expr_coll(parser, p->pEList->a[n].pExpr, is_found,
				     coll_id);
	}
	return coll;
}

/**
 * The collating sequence for the compound select is taken from the
 * left-most term of the select that has a collating sequence.
 * @param parser Parser.
 * @param p Select.
 * @param n Column number.
 * @param[out] coll_id Collation identifer.
 * @retval The appropriate collating sequence for the n-th column
 *         of the result set for the compound-select statement
 *         "p".
 * @retval NULL The column has no default collating sequence.
 */
static inline struct coll *
multi_select_coll_seq(Parse *parser, Select *p, int n, uint32_t *coll_id)
{
	bool is_found = false;
	return multi_select_coll_seq_r(parser, p, n, &is_found, coll_id);
}

/**
 * The select statement passed as the second parameter is a
 * compound SELECT with an ORDER BY clause. This function
 * allocates and returns a key_def structure suitable for
 * implementing the ORDER BY.
 *
 * Space to hold the key_def structure is obtained from malloc.
 * The calling function is responsible for ensuring that this
 * structure is eventually freed.
 *
 * @param parse Parsing context.
 * @param s Select struct to analyze.
 * @param extra No of extra slots to allocate.
 *
 * @retval Allocated key_def, NULL in case of OOM.
 */
static struct key_def *
sql_multiselect_orderby_to_key_def(struct Parse *parse, struct Select *s,
				   int extra)
{
	int ob_count = s->pOrderBy->nExpr;
	struct key_def *key_def = key_def_new(ob_count + extra);
	if (key_def == NULL) {
		sqlite3OomFault(parse->db);
		return NULL;
	}

	ExprList *order_by = s->pOrderBy;
	for (int i = 0; i < ob_count; i++) {
		struct ExprList_item *item = &order_by->a[i];
		struct Expr *term = item->pExpr;
		struct coll *coll;
		uint32_t id;
		if ((term->flags & EP_Collate) != 0) {
			bool is_found = false;
			coll = sql_expr_coll(parse, term, &is_found, &id);
		} else {
			coll = multi_select_coll_seq(parse, s,
						     item->u.x.iOrderByCol - 1,
						     &id);
			if (coll != NULL) {
				const char *name = coll_by_id(id)->name;
				order_by->a[i].pExpr =
					sqlite3ExprAddCollateString(parse, term,
								    name);
			} else {
				order_by->a[i].pExpr =
					sqlite3ExprAddCollateString(parse, term,
								    "BINARY");
			}
		}
		key_def_set_part(key_def, i, i, FIELD_TYPE_SCALAR,
				 ON_CONFLICT_ACTION_ABORT, coll, id,
				 order_by->a[i].sort_order);
	}

	return key_def;
}

#ifndef SQLITE_OMIT_CTE
/*
 * This routine generates VDBE code to compute the content of a WITH RECURSIVE
 * query of the form:
 *
 *   <recursive-table> AS (<setup-query> UNION [ALL] <recursive-query>)
 *                         \___________/             \_______________/
 *                           p->pPrior                      p
 *
 *
 * There is exactly one reference to the recursive-table in the FROM clause
 * of recursive-query, marked with the SrcList->a[].fg.isRecursive flag.
 *
 * The setup-query runs once to generate an initial set of rows that go
 * into a Queue table.  Rows are extracted from the Queue table one by
 * one.  Each row extracted from Queue is output to pDest.  Then the single
 * extracted row (now in the iCurrent table) becomes the content of the
 * recursive-table for a recursive-query run.  The output of the recursive-query
 * is added back into the Queue table.  Then another row is extracted from Queue
 * and the iteration continues until the Queue table is empty.
 *
 * If the compound query operator is UNION then no duplicate rows are ever
 * inserted into the Queue table.  The iDistinct table keeps a copy of all rows
 * that have ever been inserted into Queue and causes duplicates to be
 * discarded.  If the operator is UNION ALL, then duplicates are allowed.
 *
 * If the query has an ORDER BY, then entries in the Queue table are kept in
 * ORDER BY order and the first entry is extracted for each cycle.  Without
 * an ORDER BY, the Queue table is just a FIFO.
 *
 * If a LIMIT clause is provided, then the iteration stops after LIMIT rows
 * have been output to pDest.  A LIMIT of zero means to output no rows and a
 * negative LIMIT means to output all rows.  If there is also an OFFSET clause
 * with a positive value, then the first OFFSET outputs are discarded rather
 * than being sent to pDest.  The LIMIT count does not begin until after OFFSET
 * rows have been skipped.
 */
static void
generateWithRecursiveQuery(Parse * pParse,	/* Parsing context */
			   Select * p,		/* The recursive SELECT to be coded */
			   SelectDest * pDest)	/* What to do with query results */
{
	SrcList *pSrc = p->pSrc;	/* The FROM clause of the recursive query */
	int nCol = p->pEList->nExpr;	/* Number of columns in the recursive table */
	Vdbe *v = pParse->pVdbe;	/* The prepared statement under construction */
	Select *pSetup = p->pPrior;	/* The setup query */
	int addrTop;		/* Top of the loop */
	int addrCont, addrBreak;	/* CONTINUE and BREAK addresses */
	int iCurrent = 0;	/* The Current table */
	int regCurrent;		/* Register holding Current table */
	int iQueue;		/* The Queue table */
	int iDistinct = 0;	/* To ensure unique results if UNION */
	int eDest = SRT_Fifo;	/* How to write to Queue */
	SelectDest destQueue;	/* SelectDest targetting the Queue table */
	int i;			/* Loop counter */
	int rc;			/* Result code */
	ExprList *pOrderBy;	/* The ORDER BY clause */
	Expr *pLimit, *pOffset;	/* Saved LIMIT and OFFSET */
	int regLimit, regOffset;	/* Registers used by LIMIT and OFFSET */

	/* Process the LIMIT and OFFSET clauses, if they exist */
	addrBreak = sqlite3VdbeMakeLabel(v);
	p->nSelectRow = 320;	/* 4 billion rows */
	computeLimitRegisters(pParse, p, addrBreak);
	pLimit = p->pLimit;
	pOffset = p->pOffset;
	regLimit = p->iLimit;
	regOffset = p->iOffset;
	p->pLimit = p->pOffset = 0;
	p->iLimit = p->iOffset = 0;
	pOrderBy = p->pOrderBy;

	/* Locate the cursor number of the Current table */
	for (i = 0; ALWAYS(i < pSrc->nSrc); i++) {
		if (pSrc->a[i].fg.isRecursive) {
			iCurrent = pSrc->a[i].iCursor;
			break;
		}
	}

	/* Allocate cursors numbers for Queue and Distinct.  The cursor number for
	 * the Distinct table must be exactly one greater than Queue in order
	 * for the SRT_DistFifo and SRT_DistQueue destinations to work.
	 */
	iQueue = pParse->nTab++;
	if (p->op == TK_UNION) {
		eDest = pOrderBy ? SRT_DistQueue : SRT_DistFifo;
		iDistinct = pParse->nTab++;
	} else {
		eDest = pOrderBy ? SRT_Queue : SRT_Fifo;
	}
	sqlite3SelectDestInit(&destQueue, eDest, iQueue);

	/* Allocate cursors for Current, Queue, and Distinct. */
	regCurrent = ++pParse->nMem;
	sqlite3VdbeAddOp3(v, OP_OpenPseudo, iCurrent, regCurrent, nCol);
	if (pOrderBy) {
		struct key_def *def = sql_multiselect_orderby_to_key_def(pParse,
									 p, 1);
		sqlite3VdbeAddOp4(v, OP_OpenTEphemeral, iQueue,
				  pOrderBy->nExpr + 2, 0, (char *)def,
				  P4_KEYDEF);
		VdbeComment((v, "Orderby table"));
		destQueue.pOrderBy = pOrderBy;
	} else {
		sqlite3VdbeAddOp2(v, OP_OpenTEphemeral, iQueue, nCol + 1);
		VdbeComment((v, "Queue table"));
	}
	if (iDistinct) {
		p->addrOpenEphm[0] =
		    sqlite3VdbeAddOp2(v, OP_OpenTEphemeral, iDistinct, 1);
		p->selFlags |= SF_UsesEphemeral;
		VdbeComment((v, "Distinct table"));
	}

	/* Detach the ORDER BY clause from the compound SELECT */
	p->pOrderBy = 0;

	/* Store the results of the setup-query in Queue. */
	pSetup->pNext = 0;
	rc = sqlite3Select(pParse, pSetup, &destQueue);
	pSetup->pNext = p;
	if (rc)
		goto end_of_recursive_query;

	/* Find the next row in the Queue and output that row */
	addrTop = sqlite3VdbeAddOp2(v, OP_Rewind, iQueue, addrBreak);
	VdbeCoverage(v);

	/* Transfer the next row in Queue over to Current */
	sqlite3VdbeAddOp1(v, OP_NullRow, iCurrent);	/* To reset column cache */
	if (pOrderBy) {
		sqlite3VdbeAddOp3(v, OP_Column, iQueue, pOrderBy->nExpr + 1,
				  regCurrent);
	} else {
		sqlite3VdbeAddOp2(v, OP_RowData, iQueue, regCurrent);
	}
	sqlite3VdbeAddOp1(v, OP_Delete, iQueue);

	/* Output the single row in Current */
	addrCont = sqlite3VdbeMakeLabel(v);
	codeOffset(v, regOffset, addrCont);
	selectInnerLoop(pParse, p, p->pEList, iCurrent,
			0, 0, pDest, addrCont, addrBreak);
	if (regLimit) {
		sqlite3VdbeAddOp2(v, OP_DecrJumpZero, regLimit, addrBreak);
		VdbeCoverage(v);
	}
	sqlite3VdbeResolveLabel(v, addrCont);

	/* Execute the recursive SELECT taking the single row in Current as
	 * the value for the recursive-table. Store the results in the Queue.
	 */
	if (p->selFlags & SF_Aggregate) {
		sqlite3ErrorMsg(pParse,
				"recursive aggregate queries not supported");
	} else {
		p->pPrior = 0;
		sqlite3Select(pParse, p, &destQueue);
		assert(p->pPrior == 0);
		p->pPrior = pSetup;
	}

	/* Keep running the loop until the Queue is empty */
	sqlite3VdbeGoto(v, addrTop);
	sqlite3VdbeResolveLabel(v, addrBreak);

 end_of_recursive_query:
	sql_expr_list_delete(pParse->db, p->pOrderBy);
	p->pOrderBy = pOrderBy;
	p->pLimit = pLimit;
	p->pOffset = pOffset;
	return;
}
#endif				/* SQLITE_OMIT_CTE */

/* Forward references */
static int multiSelectOrderBy(Parse * pParse,	/* Parsing context */
			      Select * p,	/* The right-most of SELECTs to be coded */
			      SelectDest * pDest	/* What to do with query results */
    );

/*
 * Handle the special case of a compound-select that originates from a
 * VALUES clause.  By handling this as a special case, we avoid deep
 * recursion, and thus do not need to enforce the SQLITE_LIMIT_COMPOUND_SELECT
 * on a VALUES clause.
 *
 * Because the Select object originates from a VALUES clause:
 *   (1) It has no LIMIT or OFFSET
 *   (2) All terms are UNION ALL
 *   (3) There is no ORDER BY clause
 */
static int
multiSelectValues(Parse * pParse,	/* Parsing context */
		  Select * p,		/* The right-most of SELECTs to be coded */
		  SelectDest * pDest)	/* What to do with query results */
{
	Select *pPrior;
	int nRow = 1;
	int rc = 0;
	assert(p->selFlags & SF_MultiValue);
	do {
		assert(p->selFlags & SF_Values);
		assert(p->op == TK_ALL
		       || (p->op == TK_SELECT && p->pPrior == 0));
		assert(p->pLimit == 0);
		assert(p->pOffset == 0);
		assert(p->pNext == 0
		       || p->pEList->nExpr == p->pNext->pEList->nExpr);
		if (p->pPrior == 0)
			break;
		assert(p->pPrior->pNext == p);
		p = p->pPrior;
		nRow++;
	} while (1);
	while (p) {
		pPrior = p->pPrior;
		p->pPrior = 0;
		rc = sqlite3Select(pParse, p, pDest);
		p->pPrior = pPrior;
		if (rc)
			break;
		p->nSelectRow = nRow;
		p = p->pNext;
	}
	return rc;
}

/*
 * This routine is called to process a compound query form from
 * two or more separate queries using UNION, UNION ALL, EXCEPT, or
 * INTERSECT
 *
 * "p" points to the right-most of the two queries.  the query on the
 * left is p->pPrior.  The left query could also be a compound query
 * in which case this routine will be called recursively.
 *
 * The results of the total query are to be written into a destination
 * of type eDest with parameter iParm.
 *
 * Example 1:  Consider a three-way compound SQL statement.
 *
 *     SELECT a FROM t1 UNION SELECT b FROM t2 UNION SELECT c FROM t3
 *
 * This statement is parsed up as follows:
 *
 *     SELECT c FROM t3
 *      |
 *      `----->  SELECT b FROM t2
 *                |
 *                `------>  SELECT a FROM t1
 *
 * The arrows in the diagram above represent the Select.pPrior pointer.
 * So if this routine is called with p equal to the t3 query, then
 * pPrior will be the t2 query.  p->op will be TK_UNION in this case.
 *
 * Notice that because of the way SQLite parses compound SELECTs, the
 * individual selects always group from left to right.
 */
static int
multiSelect(Parse * pParse,	/* Parsing context */
	    Select * p,		/* The right-most of SELECTs to be coded */
	    SelectDest * pDest)	/* What to do with query results */
{
	int rc = SQLITE_OK;	/* Success code from a subroutine */
	Select *pPrior;		/* Another SELECT immediately to our left */
	Vdbe *v;		/* Generate code to this VDBE */
	SelectDest dest;	/* Alternative data destination */
	Select *pDelete = 0;	/* Chain of simple selects to delete */
	sqlite3 *db;		/* Database connection */
	int iSub1 = 0;		/* EQP id of left-hand query */
	int iSub2 = 0;		/* EQP id of right-hand query */

	/* Make sure there is no ORDER BY or LIMIT clause on prior SELECTs.  Only
	 * the last (right-most) SELECT in the series may have an ORDER BY or LIMIT.
	 */
	assert(p && p->pPrior);	/* Calling function guarantees this much */
	assert((p->selFlags & SF_Recursive) == 0 || p->op == TK_ALL
	       || p->op == TK_UNION);
	db = pParse->db;
	pPrior = p->pPrior;
	dest = *pDest;
	if (pPrior->pOrderBy) {
		sqlite3ErrorMsg(pParse,
				"ORDER BY clause should come after %s not before",
				selectOpName(p->op));
		rc = 1;
		goto multi_select_end;
	}
	if (pPrior->pLimit) {
		sqlite3ErrorMsg(pParse,
				"LIMIT clause should come after %s not before",
				selectOpName(p->op));
		rc = 1;
		goto multi_select_end;
	}

	v = sqlite3GetVdbe(pParse);
	assert(v != 0);		/* The VDBE already created by calling function */

	/* Create the destination temporary table if necessary
	 */
	if (dest.eDest == SRT_EphemTab) {
		assert(p->pEList);
		int nCols = p->pEList->nExpr;
		sqlite3VdbeAddOp2(v, OP_OpenTEphemeral, dest.iSDParm, nCols + 1);
		VdbeComment((v, "Destination temp"));
		dest.eDest = SRT_Table;
	}

	/* Special handling for a compound-select that originates as a VALUES clause.
	 */
	if (p->selFlags & SF_MultiValue) {
		rc = multiSelectValues(pParse, p, &dest);
		goto multi_select_end;
	}

	/* Make sure all SELECTs in the statement have the same number of elements
	 * in their result sets.
	 */
	assert(p->pEList && pPrior->pEList);
	assert(p->pEList->nExpr == pPrior->pEList->nExpr);

#ifndef SQLITE_OMIT_CTE
	if (p->selFlags & SF_Recursive) {
		generateWithRecursiveQuery(pParse, p, &dest);
	} else
#endif

		/* Compound SELECTs that have an ORDER BY clause are handled separately.
		 */
	if (p->pOrderBy) {
		return multiSelectOrderBy(pParse, p, pDest);
	} else
		/* Generate code for the left and right SELECT statements.
		 */
		switch (p->op) {
		case TK_ALL:{
				int addr = 0;
				int nLimit;
				assert(!pPrior->pLimit);
				pPrior->iLimit = p->iLimit;
				pPrior->iOffset = p->iOffset;
				pPrior->pLimit = p->pLimit;
				pPrior->pOffset = p->pOffset;
				iSub1 = pParse->iNextSelectId;
				rc = sqlite3Select(pParse, pPrior, &dest);
				p->pLimit = 0;
				p->pOffset = 0;
				if (rc) {
					goto multi_select_end;
				}
				p->pPrior = 0;
				p->iLimit = pPrior->iLimit;
				p->iOffset = pPrior->iOffset;
				if (p->iLimit) {
					addr =
					    sqlite3VdbeAddOp1(v, OP_IfNot,
							      p->iLimit);
					VdbeCoverage(v);
					VdbeComment((v,
						     "Jump ahead if LIMIT reached"));
					if (p->iOffset) {
						sqlite3VdbeAddOp3(v,
								  OP_OffsetLimit,
								  p->iLimit,
								  p->iOffset +
								  1,
								  p->iOffset);
					}
				}
				iSub2 = pParse->iNextSelectId;
				rc = sqlite3Select(pParse, p, &dest);
				testcase(rc != SQLITE_OK);
				pDelete = p->pPrior;
				p->pPrior = pPrior;
				p->nSelectRow =
				    sqlite3LogEstAdd(p->nSelectRow,
						     pPrior->nSelectRow);
				if (pPrior->pLimit
				    && sqlite3ExprIsInteger(pPrior->pLimit,
							    &nLimit)
				    && nLimit > 0
				    && p->nSelectRow > sqlite3LogEst((u64) nLimit)
				    ) {
					p->nSelectRow =
					    sqlite3LogEst((u64) nLimit);
				}
				if (addr) {
					sqlite3VdbeJumpHere(v, addr);
				}
				break;
			}
		case TK_EXCEPT:
		case TK_UNION:{
				int unionTab;	/* Cursor number of the temporary table holding result */
				u8 op = 0;	/* One of the SRT_ operations to apply to self */
				int priorOp;	/* The SRT_ operation to apply to prior selects */
				Expr *pLimit, *pOffset;	/* Saved values of p->nLimit and p->nOffset */
				int addr;
				SelectDest uniondest;

				testcase(p->op == TK_EXCEPT);
				testcase(p->op == TK_UNION);
				priorOp = SRT_Union;
				if (dest.eDest == priorOp) {
					/* We can reuse a temporary table generated by a SELECT to our
					 * right.
					 */
					assert(p->pLimit == 0);	/* Not allowed on leftward elements */
					assert(p->pOffset == 0);	/* Not allowed on leftward elements */
					unionTab = dest.iSDParm;
				} else {
					/* We will need to create our own temporary table to hold the
					 * intermediate results.
					 */
					unionTab = pParse->nTab++;
					assert(p->pOrderBy == 0);
					addr =
					    sqlite3VdbeAddOp2(v,
							      OP_OpenTEphemeral,
							      unionTab, 0);
					assert(p->addrOpenEphm[0] == -1);
					p->addrOpenEphm[0] = addr;
					findRightmost(p)->selFlags |=
					    SF_UsesEphemeral;
					assert(p->pEList);
				}

				/* Code the SELECT statements to our left
				 */
				assert(!pPrior->pOrderBy);
				sqlite3SelectDestInit(&uniondest, priorOp,
						      unionTab);
				iSub1 = pParse->iNextSelectId;
				rc = sqlite3Select(pParse, pPrior, &uniondest);
				if (rc) {
					goto multi_select_end;
				}

				/* Code the current SELECT statement
				 */
				if (p->op == TK_EXCEPT) {
					op = SRT_Except;
				} else {
					assert(p->op == TK_UNION);
					op = SRT_Union;
				}
				p->pPrior = 0;
				pLimit = p->pLimit;
				p->pLimit = 0;
				pOffset = p->pOffset;
				p->pOffset = 0;
				uniondest.eDest = op;
				iSub2 = pParse->iNextSelectId;
				rc = sqlite3Select(pParse, p, &uniondest);
				testcase(rc != SQLITE_OK);
				/* Query flattening in sqlite3Select() might refill p->pOrderBy.
				 * Be sure to delete p->pOrderBy, therefore, to avoid a memory leak.
				 */
				sql_expr_list_delete(db, p->pOrderBy);
				pDelete = p->pPrior;
				p->pPrior = pPrior;
				p->pOrderBy = 0;
				if (p->op == TK_UNION) {
					p->nSelectRow =
					    sqlite3LogEstAdd(p->nSelectRow,
							     pPrior->
							     nSelectRow);
				}
				sql_expr_delete(db, p->pLimit, false);
				p->pLimit = pLimit;
				p->pOffset = pOffset;
				p->iLimit = 0;
				p->iOffset = 0;

				/* Convert the data in the temporary table into whatever form
				 * it is that we currently need.
				 */
				assert(unionTab == dest.iSDParm
				       || dest.eDest != priorOp);
				if (dest.eDest != priorOp) {
					int iCont, iBreak, iStart;
					assert(p->pEList);
					if (dest.eDest == SRT_Output) {
						Select *pFirst = p;
						while (pFirst->pPrior)
							pFirst = pFirst->pPrior;
						generateColumnNames(pParse,
								    pFirst->pSrc,
								    pFirst->pEList);
					}
					iBreak = sqlite3VdbeMakeLabel(v);
					iCont = sqlite3VdbeMakeLabel(v);
					computeLimitRegisters(pParse, p,
							      iBreak);
					sqlite3VdbeAddOp2(v, OP_Rewind,
							  unionTab, iBreak);
					VdbeCoverage(v);
					iStart = sqlite3VdbeCurrentAddr(v);
					selectInnerLoop(pParse, p, p->pEList,
							unionTab, 0, 0, &dest,
							iCont, iBreak);
					sqlite3VdbeResolveLabel(v, iCont);
					sqlite3VdbeAddOp2(v, OP_Next, unionTab,
							  iStart);
					VdbeCoverage(v);
					sqlite3VdbeResolveLabel(v, iBreak);
					sqlite3VdbeAddOp2(v, OP_Close, unionTab,
							  0);
				}
				break;
			}
		default:
			assert(p->op == TK_INTERSECT); {
				int tab1, tab2;
				int iCont, iBreak, iStart;
				Expr *pLimit, *pOffset;
				int addr;
				SelectDest intersectdest;
				int r1;

				/* INTERSECT is different from the others since it requires
				 * two temporary tables.  Hence it has its own case.  Begin
				 * by allocating the tables we will need.
				 */
				tab1 = pParse->nTab++;
				tab2 = pParse->nTab++;
				assert(p->pOrderBy == 0);

				addr =
				    sqlite3VdbeAddOp2(v, OP_OpenTEphemeral, tab1,
						      0);
				assert(p->addrOpenEphm[0] == -1);
				p->addrOpenEphm[0] = addr;
				findRightmost(p)->selFlags |= SF_UsesEphemeral;
				assert(p->pEList);

				/* Code the SELECTs to our left into temporary table "tab1".
				 */
				sqlite3SelectDestInit(&intersectdest, SRT_Union,
						      tab1);
				iSub1 = pParse->iNextSelectId;
				rc = sqlite3Select(pParse, pPrior,
						   &intersectdest);
				if (rc) {
					goto multi_select_end;
				}

				/* Code the current SELECT into temporary table "tab2"
				 */
				addr =
				    sqlite3VdbeAddOp2(v, OP_OpenTEphemeral, tab2,
						      0);
				assert(p->addrOpenEphm[1] == -1);
				p->addrOpenEphm[1] = addr;
				p->pPrior = 0;
				pLimit = p->pLimit;
				p->pLimit = 0;
				pOffset = p->pOffset;
				p->pOffset = 0;
				intersectdest.iSDParm = tab2;
				iSub2 = pParse->iNextSelectId;
				rc = sqlite3Select(pParse, p, &intersectdest);
				testcase(rc != SQLITE_OK);
				pDelete = p->pPrior;
				p->pPrior = pPrior;
				if (p->nSelectRow > pPrior->nSelectRow)
					p->nSelectRow = pPrior->nSelectRow;
				sql_expr_delete(db, p->pLimit, false);
				p->pLimit = pLimit;
				p->pOffset = pOffset;

				/* Generate code to take the intersection of the two temporary
				 * tables.
				 */
				assert(p->pEList);
				if (dest.eDest == SRT_Output) {
					Select *pFirst = p;
					while (pFirst->pPrior)
						pFirst = pFirst->pPrior;
					generateColumnNames(pParse,
							    pFirst->pSrc,
							    pFirst->pEList);
				}
				iBreak = sqlite3VdbeMakeLabel(v);
				iCont = sqlite3VdbeMakeLabel(v);
				computeLimitRegisters(pParse, p, iBreak);
				sqlite3VdbeAddOp2(v, OP_Rewind, tab1, iBreak);
				VdbeCoverage(v);
				r1 = sqlite3GetTempReg(pParse);
				iStart =
				    sqlite3VdbeAddOp2(v, OP_RowData, tab1, r1);
				sqlite3VdbeAddOp4Int(v, OP_NotFound, tab2,
						     iCont, r1, 0);
				VdbeCoverage(v);
				sqlite3ReleaseTempReg(pParse, r1);
				selectInnerLoop(pParse, p, p->pEList, tab1,
						0, 0, &dest, iCont, iBreak);
				sqlite3VdbeResolveLabel(v, iCont);
				sqlite3VdbeAddOp2(v, OP_Next, tab1, iStart);
				VdbeCoverage(v);
				sqlite3VdbeResolveLabel(v, iBreak);
				sqlite3VdbeAddOp2(v, OP_Close, tab2, 0);
				sqlite3VdbeAddOp2(v, OP_Close, tab1, 0);
				break;
			}
		}

	explainComposite(pParse, p->op, iSub1, iSub2, p->op != TK_ALL);

	/* Compute collating sequences used by
	 * temporary tables needed to implement the compound select.
	 * Attach the key_def structure to all temporary tables.
	 *
	 * This section is run by the right-most SELECT statement only.
	 * SELECT statements to the left always skip this part.  The right-most
	 * SELECT might also skip this part if it has no ORDER BY clause and
	 * no temp tables are required.
	 */
	if (p->selFlags & SF_UsesEphemeral) {
		assert(p->pNext == NULL);
		int nCol = p->pEList->nExpr;
		struct key_def *key_def = key_def_new(nCol);
		if (key_def == NULL) {
			sqlite3OomFault(db);
			goto multi_select_end;
		}
		for (int i = 0; i < nCol; i++) {
			uint32_t id;
			struct coll *coll = multi_select_coll_seq(pParse, p, i,
								  &id);
			key_def_set_part(key_def, i, i,
					 FIELD_TYPE_SCALAR,
					 ON_CONFLICT_ACTION_ABORT, coll, id,
					 SORT_ORDER_ASC);
		}

		for (struct Select *pLoop = p; pLoop; pLoop = pLoop->pPrior) {
			for (int i = 0; i < 2; i++) {
				int addr = pLoop->addrOpenEphm[i];
				if (addr < 0) {
					/* If [0] is unused then [1] is also unused.  So we can
					 * always safely abort as soon as the first unused slot is found
					 */
					assert(pLoop->addrOpenEphm[1] < 0);
					break;
				}
				sqlite3VdbeChangeP2(v, addr, nCol);
				struct key_def *dup_def = key_def_dup(key_def);
				if (dup_def == NULL) {
					free(key_def);
					sqlite3OomFault(db);
					goto multi_select_end;
				}

				sqlite3VdbeChangeP4(v, addr, (char *)dup_def,
						    P4_KEYDEF);
				pLoop->addrOpenEphm[i] = -1;
			}
		}
		free(key_def);
	}

 multi_select_end:
	pDest->iSdst = dest.iSdst;
	pDest->nSdst = dest.nSdst;
	sql_select_delete(db, pDelete);
	return rc;
}
#endif				/* SQLITE_OMIT_COMPOUND_SELECT */

void
sqlite3SelectWrongNumTermsError(struct Parse *parse, struct Select * p)
{
	if (p->selFlags & SF_Values) {
		sqlite3ErrorMsg(parse, "all VALUES must have the same number "\
				"of terms");
	} else {
		sqlite3ErrorMsg(parse, "SELECTs to the left and right of %s "
				"do not have the same number of result columns",
				selectOpName(p->op));
	}
}

/**
 * Code an output subroutine for a coroutine implementation of a
 * SELECT statment.
 *
 * The data to be output is contained in pIn->iSdst.  There are
 * pIn->nSdst columns to be output.  pDest is where the output
 * should be sent.
 *
 * regReturn is the number of the register holding the subroutine
 * return address.
 *
 * If regPrev>0 then it is the first register in a vector that
 * records the previous output.  mem[regPrev] is a flag that is
 * false if there has been no previous output.  If regPrev>0 then
 * code is generated to suppress duplicates.  def is used for
 * comparing keys.
 *
 * If the LIMIT found in p->iLimit is reached, jump immediately to
 * iBreak.
 *
 * @param parse Parsing context.
 * @param p The SELECT statement.
 * @param in Coroutine supplying data.
 * @param dest Where to send the data.
 * @param reg_ret The return address register.
 * @param reg_prev Previous result register.  No uniqueness if 0.
 * @param def For comparing with previous entry.
 * @param break_addr Jump here if we hit the LIMIT.
 *
 * @retval Address of generated routine.
 */
static int
generateOutputSubroutine(struct Parse *parse, struct Select *p,
			 struct SelectDest *in, struct SelectDest *dest,
			 int reg_ret, int reg_prev, const struct key_def *def,
			 int break_addr)
{
	Vdbe *v = parse->pVdbe;
	int iContinue;
	int addr;

	addr = sqlite3VdbeCurrentAddr(v);
	iContinue = sqlite3VdbeMakeLabel(v);

	/* Suppress duplicates for UNION, EXCEPT, and INTERSECT
	 */
	if (reg_prev) {
		int addr1, addr2;
		addr1 = sqlite3VdbeAddOp1(v, OP_IfNot, reg_prev);
		VdbeCoverage(v);
		struct key_def *dup_def = key_def_dup(def);
		if (dup_def == NULL) {
			sqlite3OomFault(parse->db);
			return 0;
		}
		addr2 =
		    sqlite3VdbeAddOp4(v, OP_Compare, in->iSdst, reg_prev + 1,
				      in->nSdst,
				      (char *)dup_def,
				      P4_KEYDEF);
		sqlite3VdbeAddOp3(v, OP_Jump, addr2 + 2, iContinue, addr2 + 2);
		VdbeCoverage(v);
		sqlite3VdbeJumpHere(v, addr1);
		sqlite3VdbeAddOp3(v, OP_Copy, in->iSdst, reg_prev + 1,
				  in->nSdst - 1);
		sqlite3VdbeAddOp2(v, OP_Integer, 1, reg_prev);
	}
	if (parse->db->mallocFailed)
		return 0;

	/* Suppress the first OFFSET entries if there is an OFFSET clause
	 */
	codeOffset(v, p->iOffset, iContinue);

	assert(dest->eDest != SRT_Exists);
	assert(dest->eDest != SRT_Table);
	switch (dest->eDest) {
		/* Store the result as data using a unique key.
		 */
	case SRT_EphemTab:{
			int regRec = sqlite3GetTempReg(parse);
			int regCopy = sqlite3GetTempRange(parse, in->nSdst + 1);
			sqlite3VdbeAddOp3(v, OP_NextIdEphemeral, dest->iSDParm,
					  in->nSdst, regCopy + in->nSdst);
			sqlite3VdbeAddOp3(v, OP_Copy, in->iSdst, regCopy,
					  in->nSdst - 1);
			sqlite3VdbeAddOp3(v, OP_MakeRecord, regCopy,
					  in->nSdst + 1, regRec);
			/* Set flag to save memory allocating one by malloc. */
			sqlite3VdbeChangeP5(v, 1);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, dest->iSDParm, regRec);
			sqlite3ReleaseTempRange(parse, regCopy, in->nSdst + 1);
			sqlite3ReleaseTempReg(parse, regRec);
			break;
		}
		/* If we are creating a set for an "expr IN (SELECT ...)".
		 */
	case SRT_Set:{
			int r1;
			testcase(in->nSdst > 1);
			r1 = sqlite3GetTempReg(parse);
			sqlite3VdbeAddOp4(v, OP_MakeRecord, in->iSdst,
					  in->nSdst, r1, dest->zAffSdst,
					  in->nSdst);
			sqlite3ExprCacheAffinityChange(parse, in->iSdst,
						       in->nSdst);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, dest->iSDParm, r1);
			sqlite3ReleaseTempReg(parse, r1);
			break;
		}

		/* If this is a scalar select that is part of an expression, then
		 * store the results in the appropriate memory cell and break out
		 * of the scan loop.
		 */
	case SRT_Mem:{
			assert(in->nSdst == 1 || parse->nErr > 0);
			testcase(in->nSdst != 1);
			sqlite3ExprCodeMove(parse, in->iSdst, dest->iSDParm,
					    1);
			/* The LIMIT clause will jump out of the loop for us */
			break;
		}
		/* The results are stored in a sequence of registers
		 * starting at dest->iSdst.  Then the co-routine yields.
		 */
	case SRT_Coroutine:{
			if (dest->iSdst == 0) {
				dest->iSdst =
				    sqlite3GetTempRange(parse, in->nSdst);
				dest->nSdst = in->nSdst;
			}
			sqlite3ExprCodeMove(parse, in->iSdst, dest->iSdst,
					    in->nSdst);
			sqlite3VdbeAddOp1(v, OP_Yield, dest->iSDParm);
			break;
		}

		/* If none of the above, then the result destination must be
		 * SRT_Output.  This routine is never called with any other
		 * destination other than the ones handled above or SRT_Output.
		 *
		 * For SRT_Output, results are stored in a sequence of registers.
		 * Then the OP_ResultRow opcode is used to cause sqlite3_step() to
		 * return the next row of result.
		 */
	default:{
			assert(dest->eDest == SRT_Output);
			sqlite3VdbeAddOp2(v, OP_ResultRow, in->iSdst,
					  in->nSdst);
			sqlite3ExprCacheAffinityChange(parse, in->iSdst,
						       in->nSdst);
			break;
		}
	}

	/* Jump to the end of the loop if the LIMIT is reached.
	 */
	if (p->iLimit) {
		sqlite3VdbeAddOp2(v, OP_DecrJumpZero, p->iLimit, break_addr);
		VdbeCoverage(v);
	}

	/* Generate the subroutine return
	 */
	sqlite3VdbeResolveLabel(v, iContinue);
	sqlite3VdbeAddOp1(v, OP_Return, reg_ret);

	return addr;
}

/*
 * Alternative compound select code generator for cases when there
 * is an ORDER BY clause.
 *
 * We assume a query of the following form:
 *
 *      <selectA>  <operator>  <selectB>  ORDER BY <orderbylist>
 *
 * <operator> is one of UNION ALL, UNION, EXCEPT, or INTERSECT.  The idea
 * is to code both <selectA> and <selectB> with the ORDER BY clause as
 * co-routines.  Then run the co-routines in parallel and merge the results
 * into the output.  In addition to the two coroutines (called selectA and
 * selectB) there are 7 subroutines:
 *
 *    outA:    Move the output of the selectA coroutine into the output
 *             of the compound query.
 *
 *    outB:    Move the output of the selectB coroutine into the output
 *             of the compound query.  (Only generated for UNION and
 *             UNION ALL.  EXCEPT and INSERTSECT never output a row that
 *             appears only in B.)
 *
 *    AltB:    Called when there is data from both coroutines and A<B.
 *
 *    AeqB:    Called when there is data from both coroutines and A==B.
 *
 *    AgtB:    Called when there is data from both coroutines and A>B.
 *
 *    EofA:    Called when data is exhausted from selectA.
 *
 *    EofB:    Called when data is exhausted from selectB.
 *
 * The implementation of the latter five subroutines depend on which
 * <operator> is used:
 *
 *
 *             UNION ALL         UNION            EXCEPT          INTERSECT
 *          -------------  -----------------  --------------  -----------------
 *   AltB:   outA, nextA      outA, nextA       outA, nextA         nextA
 *
 *   AeqB:   outA, nextA         nextA             nextA         outA, nextA
 *
 *   AgtB:   outB, nextB      outB, nextB          nextB            nextB
 *
 *   EofA:   outB, nextB      outB, nextB          halt             halt
 *
 *   EofB:   outA, nextA      outA, nextA       outA, nextA         halt
 *
 * In the AltB, AeqB, and AgtB subroutines, an EOF on A following nextA
 * causes an immediate jump to EofA and an EOF on B following nextB causes
 * an immediate jump to EofB.  Within EofA and EofB, and EOF on entry or
 * following nextX causes a jump to the end of the select processing.
 *
 * Duplicate removal in the UNION, EXCEPT, and INTERSECT cases is handled
 * within the output subroutine.  The regPrev register set holds the previously
 * output value.  A comparison is made against this value and the output
 * is skipped if the next results would be the same as the previous.
 *
 * The implementation plan is to implement the two coroutines and seven
 * subroutines first, then put the control logic at the bottom.  Like this:
 *
 *          goto Init
 *     coA: coroutine for left query (A)
 *     coB: coroutine for right query (B)
 *    outA: output one row of A
 *    outB: output one row of B (UNION and UNION ALL only)
 *    EofA: ...
 *    EofB: ...
 *    AltB: ...
 *    AeqB: ...
 *    AgtB: ...
 *    Init: initialize coroutine registers
 *          yield coA
 *          if eof(A) goto EofA
 *          yield coB
 *          if eof(B) goto EofB
 *    Cmpr: Compare A, B
 *          Jump AltB, AeqB, AgtB
 *     End: ...
 *
 * We call AltB, AeqB, AgtB, EofA, and EofB "subroutines" but they are not
 * actually called using Gosub and they do not Return.  EofA and EofB loop
 * until all data is exhausted then jump to the "end" labe.  AltB, AeqB,
 * and AgtB jump to either L2 or to one of EofA or EofB.
 */
#ifndef SQLITE_OMIT_COMPOUND_SELECT
static int
multiSelectOrderBy(Parse * pParse,	/* Parsing context */
		   Select * p,		/* The right-most of SELECTs to be coded */
		   SelectDest * pDest)	/* What to do with query results */
{
	int i, j;		/* Loop counters */
	Select *pPrior;		/* Another SELECT immediately to our left */
	Vdbe *v;		/* Generate code to this VDBE */
	SelectDest destA;	/* Destination for coroutine A */
	SelectDest destB;	/* Destination for coroutine B */
	int regAddrA;		/* Address register for select-A coroutine */
	int regAddrB;		/* Address register for select-B coroutine */
	int addrSelectA;	/* Address of the select-A coroutine */
	int addrSelectB;	/* Address of the select-B coroutine */
	int regOutA;		/* Address register for the output-A subroutine */
	int regOutB;		/* Address register for the output-B subroutine */
	int addrOutA;		/* Address of the output-A subroutine */
	int addrOutB = 0;	/* Address of the output-B subroutine */
	int addrEofA;		/* Address of the select-A-exhausted subroutine */
	int addrEofA_noB;	/* Alternate addrEofA if B is uninitialized */
	int addrEofB;		/* Address of the select-B-exhausted subroutine */
	int addrAltB;		/* Address of the A<B subroutine */
	int addrAeqB;		/* Address of the A==B subroutine */
	int addrAgtB;		/* Address of the A>B subroutine */
	int regLimitA;		/* Limit register for select-A */
	int regLimitB;		/* Limit register for select-A */
	int regPrev;		/* A range of registers to hold previous output */
	int savedLimit;		/* Saved value of p->iLimit */
	int savedOffset;	/* Saved value of p->iOffset */
	int labelCmpr;		/* Label for the start of the merge algorithm */
	int labelEnd;		/* Label for the end of the overall SELECT stmt */
	int addr1;		/* Jump instructions that get retargetted */
	int op;			/* One of TK_ALL, TK_UNION, TK_EXCEPT, TK_INTERSECT */
	/* Comparison information for duplicate removal */
	struct key_def *def_dup = NULL;
	/* Comparison information for merging rows */
	struct key_def *def_merge;
	sqlite3 *db;		/* Database connection */
	ExprList *pOrderBy;	/* The ORDER BY clause */
	int nOrderBy;		/* Number of terms in the ORDER BY clause */
	int *aPermute;		/* Mapping from ORDER BY terms to result set columns */
	int iSub1;		/* EQP id of left-hand query */
	int iSub2;		/* EQP id of right-hand query */

	assert(p->pOrderBy != 0);
	db = pParse->db;
	v = pParse->pVdbe;
	assert(v != 0);		/* Already thrown the error if VDBE alloc failed */
	labelEnd = sqlite3VdbeMakeLabel(v);
	labelCmpr = sqlite3VdbeMakeLabel(v);

	/* Patch up the ORDER BY clause
	 */
	op = p->op;
	pPrior = p->pPrior;
	assert(pPrior->pOrderBy == 0);
	pOrderBy = p->pOrderBy;
	assert(pOrderBy);
	nOrderBy = pOrderBy->nExpr;

	/* For operators other than UNION ALL we have to make sure that
	 * the ORDER BY clause covers every term of the result set.  Add
	 * terms to the ORDER BY clause as necessary.
	 */
	if (op != TK_ALL) {
		for (i = 1; db->mallocFailed == 0 && i <= p->pEList->nExpr; i++) {
			struct ExprList_item *pItem;
			for (j = 0, pItem = pOrderBy->a; j < nOrderBy;
			     j++, pItem++) {
				assert(pItem->u.x.iOrderByCol > 0);
				if (pItem->u.x.iOrderByCol == i)
					break;
			}
			if (j == nOrderBy) {
				Expr *pNew = sqlite3Expr(db, TK_INTEGER, 0);
				if (pNew == 0)
					return SQLITE_NOMEM_BKPT;
				pNew->flags |= EP_IntValue;
				pNew->u.iValue = i;
				pOrderBy = sql_expr_list_append(pParse->db,
								pOrderBy, pNew);
				if (pOrderBy)
					pOrderBy->a[nOrderBy++].u.x.
					    iOrderByCol = (u16) i;
			}
		}
	}

	/* Compute the comparison permutation and key_def that is used with
	 * the permutation used to determine if the next
	 * row of results comes from selectA or selectB.  Also add explicit
	 * collations to the ORDER BY clause terms so that when the subqueries
	 * to the right and the left are evaluated, they use the correct
	 * collation.
	 */
	aPermute = sqlite3DbMallocRawNN(db, sizeof(int) * (nOrderBy + 1));
	if (aPermute) {
		struct ExprList_item *pItem;
		aPermute[0] = nOrderBy;
		for (i = 1, pItem = pOrderBy->a; i <= nOrderBy; i++, pItem++) {
			assert(pItem->u.x.iOrderByCol > 0);
			assert(pItem->u.x.iOrderByCol <= p->pEList->nExpr);
			aPermute[i] = pItem->u.x.iOrderByCol - 1;
		}
		def_merge = sql_multiselect_orderby_to_key_def(pParse, p, 1);
	} else {
		def_merge = NULL;
	}

	/* Reattach the ORDER BY clause to the query.
	 */
	p->pOrderBy = pOrderBy;
	pPrior->pOrderBy = sql_expr_list_dup(pParse->db, pOrderBy, 0);

	/* Allocate a range of temporary registers and the key_def needed
	 * for the logic that removes duplicate result rows when the
	 * operator is UNION, EXCEPT, or INTERSECT (but not UNION ALL).
	 */
	if (op == TK_ALL) {
		regPrev = 0;
	} else {
		int expr_count = p->pEList->nExpr;
		assert(nOrderBy >= expr_count || db->mallocFailed);
		regPrev = pParse->nMem + 1;
		pParse->nMem += expr_count + 1;
		sqlite3VdbeAddOp2(v, OP_Integer, 0, regPrev);
		def_dup = key_def_new(expr_count);
		if (def_dup != NULL) {
			for (int i = 0; i < expr_count; i++) {
				uint32_t id;
				struct coll *coll =
					multi_select_coll_seq(pParse, p, i,
							      &id);
				key_def_set_part(def_dup, i, i,
						 FIELD_TYPE_SCALAR,
						 ON_CONFLICT_ACTION_ABORT, coll,
						 id, SORT_ORDER_ASC);
			}
		} else {
			sqlite3OomFault(db);
		}
	}

	/* Separate the left and the right query from one another
	 */
	p->pPrior = 0;
	pPrior->pNext = 0;
	sqlite3ResolveOrderGroupBy(pParse, p, p->pOrderBy, "ORDER");
	if (pPrior->pPrior == 0) {
		sqlite3ResolveOrderGroupBy(pParse, pPrior, pPrior->pOrderBy,
					   "ORDER");
	}

	/* Compute the limit registers */
	computeLimitRegisters(pParse, p, labelEnd);
	if (p->iLimit && op == TK_ALL) {
		regLimitA = ++pParse->nMem;
		regLimitB = ++pParse->nMem;
		sqlite3VdbeAddOp2(v, OP_Copy,
				  p->iOffset ? p->iOffset + 1 : p->iLimit,
				  regLimitA);
		sqlite3VdbeAddOp2(v, OP_Copy, regLimitA, regLimitB);
	} else {
		regLimitA = regLimitB = 0;
	}
	sql_expr_delete(db, p->pLimit, false);
	p->pLimit = 0;
	sql_expr_delete(db, p->pOffset, false);
	p->pOffset = 0;

	regAddrA = ++pParse->nMem;
	regAddrB = ++pParse->nMem;
	regOutA = ++pParse->nMem;
	regOutB = ++pParse->nMem;
	sqlite3SelectDestInit(&destA, SRT_Coroutine, regAddrA);
	sqlite3SelectDestInit(&destB, SRT_Coroutine, regAddrB);

	/* Generate a coroutine to evaluate the SELECT statement to the
	 * left of the compound operator - the "A" select.
	 */
	addrSelectA = sqlite3VdbeCurrentAddr(v) + 1;
	addr1 =
	    sqlite3VdbeAddOp3(v, OP_InitCoroutine, regAddrA, 0, addrSelectA);
	VdbeComment((v, "left SELECT"));
	pPrior->iLimit = regLimitA;
	iSub1 = pParse->iNextSelectId;
	sqlite3Select(pParse, pPrior, &destA);
	sqlite3VdbeEndCoroutine(v, regAddrA);
	sqlite3VdbeJumpHere(v, addr1);

	/* Generate a coroutine to evaluate the SELECT statement on
	 * the right - the "B" select
	 */
	addrSelectB = sqlite3VdbeCurrentAddr(v) + 1;
	addr1 =
	    sqlite3VdbeAddOp3(v, OP_InitCoroutine, regAddrB, 0, addrSelectB);
	VdbeComment((v, "right SELECT"));
	savedLimit = p->iLimit;
	savedOffset = p->iOffset;
	p->iLimit = regLimitB;
	p->iOffset = 0;
	iSub2 = pParse->iNextSelectId;
	sqlite3Select(pParse, p, &destB);
	p->iLimit = savedLimit;
	p->iOffset = savedOffset;
	sqlite3VdbeEndCoroutine(v, regAddrB);

	/* Generate a subroutine that outputs the current row of the A
	 * select as the next output row of the compound select.
	 */
	VdbeNoopComment((v, "Output routine for A"));
	addrOutA = generateOutputSubroutine(pParse,
					    p, &destA, pDest, regOutA,
					    regPrev, def_dup, labelEnd);

	/* Generate a subroutine that outputs the current row of the B
	 * select as the next output row of the compound select.
	 */
	if (op == TK_ALL || op == TK_UNION) {
		VdbeNoopComment((v, "Output routine for B"));
		addrOutB = generateOutputSubroutine(pParse,
						    p, &destB, pDest, regOutB,
						    regPrev, def_dup, labelEnd);
	}

	key_def_delete(def_dup);

	/* Generate a subroutine to run when the results from select A
	 * are exhausted and only data in select B remains.
	 */
	if (op == TK_EXCEPT || op == TK_INTERSECT) {
		addrEofA_noB = addrEofA = labelEnd;
	} else {
		VdbeNoopComment((v, "eof-A subroutine"));
		addrEofA = sqlite3VdbeAddOp2(v, OP_Gosub, regOutB, addrOutB);
		addrEofA_noB =
		    sqlite3VdbeAddOp2(v, OP_Yield, regAddrB, labelEnd);
		VdbeCoverage(v);
		sqlite3VdbeGoto(v, addrEofA);
		p->nSelectRow =
		    sqlite3LogEstAdd(p->nSelectRow, pPrior->nSelectRow);
	}

	/* Generate a subroutine to run when the results from select B
	 * are exhausted and only data in select A remains.
	 */
	if (op == TK_INTERSECT) {
		addrEofB = addrEofA;
		if (p->nSelectRow > pPrior->nSelectRow)
			p->nSelectRow = pPrior->nSelectRow;
	} else {
		VdbeNoopComment((v, "eof-B subroutine"));
		addrEofB = sqlite3VdbeAddOp2(v, OP_Gosub, regOutA, addrOutA);
		sqlite3VdbeAddOp2(v, OP_Yield, regAddrA, labelEnd);
		VdbeCoverage(v);
		sqlite3VdbeGoto(v, addrEofB);
	}

	/* Generate code to handle the case of A<B
	 */
	VdbeNoopComment((v, "A-lt-B subroutine"));
	addrAltB = sqlite3VdbeAddOp2(v, OP_Gosub, regOutA, addrOutA);
	sqlite3VdbeAddOp2(v, OP_Yield, regAddrA, addrEofA);
	VdbeCoverage(v);
	sqlite3VdbeGoto(v, labelCmpr);

	/* Generate code to handle the case of A==B
	 */
	if (op == TK_ALL) {
		addrAeqB = addrAltB;
	} else if (op == TK_INTERSECT) {
		addrAeqB = addrAltB;
		addrAltB++;
	} else {
		VdbeNoopComment((v, "A-eq-B subroutine"));
		addrAeqB = sqlite3VdbeAddOp2(v, OP_Yield, regAddrA, addrEofA);
		VdbeCoverage(v);
		sqlite3VdbeGoto(v, labelCmpr);
	}

	/* Generate code to handle the case of A>B
	 */
	VdbeNoopComment((v, "A-gt-B subroutine"));
	addrAgtB = sqlite3VdbeCurrentAddr(v);
	if (op == TK_ALL || op == TK_UNION) {
		sqlite3VdbeAddOp2(v, OP_Gosub, regOutB, addrOutB);
	}
	sqlite3VdbeAddOp2(v, OP_Yield, regAddrB, addrEofB);
	VdbeCoverage(v);
	sqlite3VdbeGoto(v, labelCmpr);

	/* This code runs once to initialize everything.
	 */
	sqlite3VdbeJumpHere(v, addr1);
	sqlite3VdbeAddOp2(v, OP_Yield, regAddrA, addrEofA_noB);
	VdbeCoverage(v);
	sqlite3VdbeAddOp2(v, OP_Yield, regAddrB, addrEofB);
	VdbeCoverage(v);

	/* Implement the main merge loop
	 */
	sqlite3VdbeResolveLabel(v, labelCmpr);
	sqlite3VdbeAddOp4(v, OP_Permutation, 0, 0, 0, (char *)aPermute,
			  P4_INTARRAY);
	sqlite3VdbeAddOp4(v, OP_Compare, destA.iSdst, destB.iSdst, nOrderBy,
			  (char *)def_merge, P4_KEYDEF);
	sqlite3VdbeChangeP5(v, OPFLAG_PERMUTE);
	sqlite3VdbeAddOp3(v, OP_Jump, addrAltB, addrAeqB, addrAgtB);
	VdbeCoverage(v);

	/* Jump to the this point in order to terminate the query.
	 */
	sqlite3VdbeResolveLabel(v, labelEnd);

	/* Set the number of output columns
	 */
	if (pDest->eDest == SRT_Output) {
		Select *pFirst = pPrior;
		while (pFirst->pPrior)
			pFirst = pFirst->pPrior;
		generateColumnNames(pParse, pFirst->pSrc, pFirst->pEList);
	}

	/* Reassembly the compound query so that it will be freed correctly
	 * by the calling function
	 */
	if (p->pPrior) {
		sql_select_delete(db, p->pPrior);
	}
	p->pPrior = pPrior;
	pPrior->pNext = p;

  /*** TBD:  Insert subroutine calls to close cursors on incomplete
  *** subqueries ***
  */
	explainComposite(pParse, p->op, iSub1, iSub2, 0);
	return pParse->nErr != 0;
}
#endif

/* Forward Declarations */
static void substExprList(Parse *, ExprList *, int, ExprList *);
static void substSelect(Parse *, Select *, int, ExprList *, int);

/*
 * Scan through the expression pExpr.  Replace every reference to
 * a column in table number iTable with a copy of the iColumn-th
 * entry in pEList.
 *
 * This routine is part of the flattening procedure.  A subquery
 * whose result set is defined by pEList appears as entry in the
 * FROM clause of a SELECT such that the VDBE cursor assigned to that
 * FORM clause entry is iTable.  This routine make the necessary
 * changes to pExpr so that it refers directly to the source table
 * of the subquery rather the result set of the subquery.
 */
static Expr *
substExpr(Parse * pParse,	/* Report errors here */
	  Expr * pExpr,		/* Expr in which substitution occurs */
	  int iTable,		/* Table to be substituted */
	  ExprList * pEList)	/* Substitute expressions */
{
	sqlite3 *db = pParse->db;
	if (pExpr == 0)
		return 0;
	if (pExpr->op == TK_COLUMN && pExpr->iTable == iTable) {
		if (pExpr->iColumn < 0) {
			pExpr->op = TK_NULL;
		} else {
			Expr *pNew;
			Expr *pCopy = pEList->a[pExpr->iColumn].pExpr;
			assert(pEList != 0 && pExpr->iColumn < pEList->nExpr);
			assert(pExpr->pLeft == 0 && pExpr->pRight == 0);
			if (sqlite3ExprIsVector(pCopy)) {
				sqlite3VectorErrorMsg(pParse, pCopy);
			} else {
				pNew = sqlite3ExprDup(db, pCopy, 0);
				if (pNew && (pExpr->flags & EP_FromJoin)) {
					pNew->iRightJoinTable =
					    pExpr->iRightJoinTable;
					pNew->flags |= EP_FromJoin;
				}
				sql_expr_delete(db, pExpr, false);
				pExpr = pNew;
			}
		}
	} else {
		pExpr->pLeft = substExpr(pParse, pExpr->pLeft, iTable, pEList);
		pExpr->pRight =
		    substExpr(pParse, pExpr->pRight, iTable, pEList);
		if (ExprHasProperty(pExpr, EP_xIsSelect)) {
			substSelect(pParse, pExpr->x.pSelect, iTable, pEList,
				    1);
		} else {
			substExprList(pParse, pExpr->x.pList, iTable, pEList);
		}
	}
	return pExpr;
}

static void
substExprList(Parse * pParse,		/* Report errors here */
	      ExprList * pList,		/* List to scan and in which to make substitutes */
	      int iTable,		/* Table to be substituted */
	      ExprList * pEList)	/* Substitute values */
{
	int i;
	if (pList == 0)
		return;
	for (i = 0; i < pList->nExpr; i++) {
		pList->a[i].pExpr =
		    substExpr(pParse, pList->a[i].pExpr, iTable, pEList);
	}
}

static void
substSelect(Parse * pParse,	/* Report errors here */
	    Select * p,		/* SELECT statement in which to make substitutions */
	    int iTable,		/* Table to be replaced */
	    ExprList * pEList,	/* Substitute values */
	    int doPrior)	/* Do substitutes on p->pPrior too */
{
	SrcList *pSrc;
	struct SrcList_item *pItem;
	int i;
	if (!p)
		return;
	do {
		substExprList(pParse, p->pEList, iTable, pEList);
		substExprList(pParse, p->pGroupBy, iTable, pEList);
		substExprList(pParse, p->pOrderBy, iTable, pEList);
		p->pHaving = substExpr(pParse, p->pHaving, iTable, pEList);
		p->pWhere = substExpr(pParse, p->pWhere, iTable, pEList);
		pSrc = p->pSrc;
		assert(pSrc != 0);
		for (i = pSrc->nSrc, pItem = pSrc->a; i > 0; i--, pItem++) {
			substSelect(pParse, pItem->pSelect, iTable, pEList, 1);
			if (pItem->fg.isTabFunc) {
				substExprList(pParse, pItem->u1.pFuncArg,
					      iTable, pEList);
			}
		}
	} while (doPrior && (p = p->pPrior) != 0);
}

/*
 * This routine attempts to flatten subqueries as a performance optimization.
 * This routine returns 1 if it makes changes and 0 if no flattening occurs.
 *
 * To understand the concept of flattening, consider the following
 * query:
 *
 *     SELECT a FROM (SELECT x+y AS a FROM t1 WHERE z<100) WHERE a>5
 *
 * The default way of implementing this query is to execute the
 * subquery first and store the results in a temporary table, then
 * run the outer query on that temporary table.  This requires two
 * passes over the data.  Furthermore, because the temporary table
 * has no indices, the WHERE clause on the outer query cannot be
 * optimized.
 *
 * This routine attempts to rewrite queries such as the above into
 * a single flat select, like this:
 *
 *     SELECT x+y AS a FROM t1 WHERE z<100 AND a>5
 *
 * The code generated for this simplification gives the same result
 * but only has to scan the data once.  And because indices might
 * exist on the table t1, a complete scan of the data might be
 * avoided.
 *
 * Flattening is only attempted if all of the following are true:
 *
 *   (1)  The subquery and the outer query do not both use aggregates.
 *
 *   (2)  The subquery is not an aggregate or (2a) the outer query is not a join
 *        and (2b) the outer query does not use subqueries other than the one
 *        FROM-clause subquery that is a candidate for flattening.  (2b is
 *        due to ticket [2f7170d73bf9abf80] from 2015-02-09.)
 *
 *   (3)  The subquery is not the right operand of a left outer join
 *        (Originally ticket #306.  Strengthened by ticket #3300)
 *
 *   (4)  The subquery is not DISTINCT.
 *
 *  (**)  At one point restrictions (4) and (5) defined a subset of DISTINCT
 *        sub-queries that were excluded from this optimization. Restriction
 *        (4) has since been expanded to exclude all DISTINCT subqueries.
 *
 *   (6)  The subquery does not use aggregates or the outer query is not
 *        DISTINCT.
 *
 *   (7)  The subquery has a FROM clause.  TODO:  For subqueries without
 *        A FROM clause, consider adding a FROM close with the special
 *        table sqlite_once that consists of a single row containing a
 *        single NULL.
 *
 *   (8)  The subquery does not use LIMIT or the outer query is not a join.
 *
 *   (9)  The subquery does not use LIMIT or the outer query does not use
 *        aggregates.
 *
 *  (**)  Restriction (10) was removed from the code on 2005-02-05 but we
 *        accidently carried the comment forward until 2014-09-15.  Original
 *        text: "The subquery does not use aggregates or the outer query
 *        does not use LIMIT."
 *
 *  (11)  The subquery and the outer query do not both have ORDER BY clauses.
 *
 *  (**)  Not implemented.  Subsumed into restriction (3).  Was previously
 *        a separate restriction deriving from ticket #350.
 *
 *  (13)  The subquery and outer query do not both use LIMIT.
 *
 *  (14)  The subquery does not use OFFSET.
 *
 *  (15)  The outer query is not part of a compound select or the
 *        subquery does not have a LIMIT clause.
 *        (See ticket #2339 and ticket [02a8e81d44]).
 *
 *  (16)  The outer query is not an aggregate or the subquery does
 *        not contain ORDER BY.  (Ticket #2942)  This used to not matter
 *        until we introduced the group_concat() function.
 *
 *  (17)  The sub-query is not a compound select, or it is a UNION ALL
 *        compound clause made up entirely of non-aggregate queries, and
 *        the parent query:
 *
 *          * is not itself part of a compound select,
 *          * is not an aggregate or DISTINCT query, and
 *          * is not a join
 *
 *        The parent and sub-query may contain WHERE clauses. Subject to
 *        rules (11), (13) and (14), they may also contain ORDER BY,
 *        LIMIT and OFFSET clauses.  The subquery cannot use any compound
 *        operator other than UNION ALL because all the other compound
 *        operators have an implied DISTINCT which is disallowed by
 *        restriction (4).
 *
 *        Also, each component of the sub-query must return the same number
 *        of result columns. This is actually a requirement for any compound
 *        SELECT statement, but all the code here does is make sure that no
 *        such (illegal) sub-query is flattened. The caller will detect the
 *        syntax error and return a detailed message.
 *
 *  (18)  If the sub-query is a compound select, then all terms of the
 *        ORDER by clause of the parent must be simple references to
 *        columns of the sub-query.
 *
 *  (19)  The subquery does not use LIMIT or the outer query does not
 *        have a WHERE clause.
 *
 *  (20)  If the sub-query is a compound select, then it must not use
 *        an ORDER BY clause.  Ticket #3773.  We could relax this constraint
 *        somewhat by saying that the terms of the ORDER BY clause must
 *        appear as unmodified result columns in the outer query.  But we
 *        have other optimizations in mind to deal with that case.
 *
 *  (21)  The subquery does not use LIMIT or the outer query is not
 *        DISTINCT.  (See ticket [752e1646fc]).
 *
 *  (22)  The subquery is not a recursive CTE.
 *
 *  (23)  The parent is not a recursive CTE, or the sub-query is not a
 *        compound query. This restriction is because transforming the
 *        parent to a compound query confuses the code that handles
 *        recursive queries in multiSelect().
 *
 *  (24)  The subquery is not an aggregate that uses the built-in min() or
 *        or max() functions.  (Without this restriction, a query like:
 *        "SELECT x FROM (SELECT max(y), x FROM t1)" would not necessarily
 *        return the value X for which Y was maximal.)
 *
 *
 * In this routine, the "p" parameter is a pointer to the outer query.
 * The subquery is p->pSrc->a[iFrom].  isAgg is true if the outer query
 * uses aggregates and subqueryIsAgg is true if the subquery uses aggregates.
 *
 * If flattening is not attempted, this routine is a no-op and returns 0.
 * If flattening is attempted this routine returns 1.
 *
 * All of the expression analysis must occur on both the outer query and
 * the subquery before this routine runs.
 */
static int
flattenSubquery(Parse * pParse,		/* Parsing context */
		Select * p,		/* The parent or outer SELECT statement */
		int iFrom,		/* Index in p->pSrc->a[] of the inner subquery */
		int isAgg,		/* True if outer SELECT uses aggregate functions */
		int subqueryIsAgg)	/* True if the subquery uses aggregate functions */
{
	Select *pParent;	/* Current UNION ALL term of the other query */
	Select *pSub;		/* The inner query or "subquery" */
	Select *pSub1;		/* Pointer to the rightmost select in sub-query */
	SrcList *pSrc;		/* The FROM clause of the outer query */
	SrcList *pSubSrc;	/* The FROM clause of the subquery */
	ExprList *pList;	/* The result set of the outer query */
	int iParent;		/* VDBE cursor number of the pSub result set temp table */
	int i;			/* Loop counter */
	Expr *pWhere;		/* The WHERE clause */
	struct SrcList_item *pSubitem;	/* The subquery */
	sqlite3 *db = pParse->db;

	/* Check to see if flattening is permitted.  Return 0 if not.
	 */
	assert(p != 0);
	assert(p->pPrior == 0);	/* Unable to flatten compound queries */
	if (OptimizationDisabled(db, SQLITE_QueryFlattener))
		return 0;
	pSrc = p->pSrc;
	assert(pSrc && iFrom >= 0 && iFrom < pSrc->nSrc);
	pSubitem = &pSrc->a[iFrom];
	iParent = pSubitem->iCursor;
	pSub = pSubitem->pSelect;
	assert(pSub != 0);
	if (subqueryIsAgg) {
		if (isAgg)
			return 0;	/* Restriction (1)   */
		if (pSrc->nSrc > 1)
			return 0;	/* Restriction (2a)  */
		if ((p->pWhere && ExprHasProperty(p->pWhere, EP_Subquery))
		    || (sqlite3ExprListFlags(p->pEList) & EP_Subquery) != 0
		    || (sqlite3ExprListFlags(p->pOrderBy) & EP_Subquery) != 0) {
			return 0;	/* Restriction (2b)  */
		}
	}

	pSubSrc = pSub->pSrc;
	assert(pSubSrc);
	/* Prior to version 3.1.2, when LIMIT and OFFSET had to be simple constants,
	 * not arbitrary expressions, we allowed some combining of LIMIT and OFFSET
	 * because they could be computed at compile-time.  But when LIMIT and OFFSET
	 * became arbitrary expressions, we were forced to add restrictions (13)
	 * and (14).
	 */
	if (pSub->pLimit && p->pLimit)
		return 0;	/* Restriction (13) */
	if (pSub->pOffset)
		return 0;	/* Restriction (14) */
	if ((p->selFlags & SF_Compound) != 0 && pSub->pLimit) {
		return 0;	/* Restriction (15) */
	}
	if (pSubSrc->nSrc == 0)
		return 0;	/* Restriction (7)  */
	if (pSub->selFlags & SF_Distinct)
		return 0;	/* Restriction (5)  */
	if (pSub->pLimit && (pSrc->nSrc > 1 || isAgg)) {
		return 0;	/* Restrictions (8)(9) */
	}
	if ((p->selFlags & SF_Distinct) != 0 && subqueryIsAgg) {
		return 0;	/* Restriction (6)  */
	}
	if (p->pOrderBy && pSub->pOrderBy) {
		return 0;	/* Restriction (11) */
	}
	if (isAgg && pSub->pOrderBy)
		return 0;	/* Restriction (16) */
	if (pSub->pLimit && p->pWhere)
		return 0;	/* Restriction (19) */
	if (pSub->pLimit && (p->selFlags & SF_Distinct) != 0) {
		return 0;	/* Restriction (21) */
	}
	testcase(pSub->selFlags & SF_Recursive);
	testcase(pSub->selFlags & SF_MinMaxAgg);
	if (pSub->selFlags & (SF_Recursive | SF_MinMaxAgg)) {
		return 0;	/* Restrictions (22) and (24) */
	}
	if ((p->selFlags & SF_Recursive) && pSub->pPrior) {
		return 0;	/* Restriction (23) */
	}

	/* OBSOLETE COMMENT 1:
	 * Restriction 3:  If the subquery is a join, make sure the subquery is
	 * not used as the right operand of an outer join.  Examples of why this
	 * is not allowed:
	 *
	 *         t1 LEFT OUTER JOIN (t2 JOIN t3)
	 *
	 * If we flatten the above, we would get
	 *
	 *         (t1 LEFT OUTER JOIN t2) JOIN t3
	 *
	 * which is not at all the same thing.
	 *
	 * OBSOLETE COMMENT 2:
	 * Restriction 12:  If the subquery is the right operand of a left outer
	 * join, make sure the subquery has no WHERE clause.
	 * An examples of why this is not allowed:
	 *
	 *         t1 LEFT OUTER JOIN (SELECT * FROM t2 WHERE t2.x>0)
	 *
	 * If we flatten the above, we would get
	 *
	 *         (t1 LEFT OUTER JOIN t2) WHERE t2.x>0
	 *
	 * But the t2.x>0 test will always fail on a NULL row of t2, which
	 * effectively converts the OUTER JOIN into an INNER JOIN.
	 *
	 * THIS OVERRIDES OBSOLETE COMMENTS 1 AND 2 ABOVE:
	 * Ticket #3300 shows that flattening the right term of a LEFT JOIN
	 * is fraught with danger.  Best to avoid the whole thing.  If the
	 * subquery is the right term of a LEFT JOIN, then do not flatten.
	 */
	if ((pSubitem->fg.jointype & JT_OUTER) != 0) {
		return 0;
	}

	/* Restriction 17: If the sub-query is a compound SELECT, then it must
	 * use only the UNION ALL operator. And none of the simple select queries
	 * that make up the compound SELECT are allowed to be aggregate or distinct
	 * queries.
	 */
	if (pSub->pPrior) {
		if (isAgg || (p->selFlags & SF_Distinct) != 0
		    || pSrc->nSrc != 1) {
			return 0;
		}
		for (pSub1 = pSub; pSub1; pSub1 = pSub1->pPrior) {
			/* Restriction 20 */
			if (pSub1->pOrderBy != NULL)
				return 0;
			testcase((pSub1->
				  selFlags & (SF_Distinct | SF_Aggregate)) ==
				 SF_Distinct);
			testcase((pSub1->
				  selFlags & (SF_Distinct | SF_Aggregate)) ==
				 SF_Aggregate);
			assert(pSub->pSrc != 0);
			assert(pSub->pEList->nExpr == pSub1->pEList->nExpr);
			if ((pSub1->selFlags & (SF_Distinct | SF_Aggregate)) !=
			    0 || (pSub1->pPrior && pSub1->op != TK_ALL)
			    || pSub1->pSrc->nSrc < 1) {
				return 0;
			}
			testcase(pSub1->pSrc->nSrc > 1);
		}

		/* Restriction 18. */
		if (p->pOrderBy) {
			int ii;
			for (ii = 0; ii < p->pOrderBy->nExpr; ii++) {
				if (p->pOrderBy->a[ii].u.x.iOrderByCol == 0)
					return 0;
			}
		}
	}

	/***** If we reach this point, flattening is permitted. *****/
	SELECTTRACE(1, pParse, p, ("flatten %s.%p from term %d\n",
				   pSub->zSelName, pSub, iFrom));

	/* If the sub-query is a compound SELECT statement, then (by restrictions
	 * 17 and 18 above) it must be a UNION ALL and the parent query must
	 * be of the form:
	 *
	 *     SELECT <expr-list> FROM (<sub-query>) <where-clause>
	 *
	 * followed by any ORDER BY, LIMIT and/or OFFSET clauses. This block
	 * creates N-1 copies of the parent query without any ORDER BY, LIMIT or
	 * OFFSET clauses and joins them to the left-hand-side of the original
	 * using UNION ALL operators. In this case N is the number of simple
	 * select statements in the compound sub-query.
	 *
	 * Example:
	 *
	 *     SELECT a+1 FROM (
	 *        SELECT x FROM tab
	 *        UNION ALL
	 *        SELECT y FROM tab
	 *        UNION ALL
	 *        SELECT abs(z*2) FROM tab2
	 *     ) WHERE a!=5 ORDER BY 1
	 *
	 * Transformed into:
	 *
	 *     SELECT x+1 FROM tab WHERE x+1!=5
	 *     UNION ALL
	 *     SELECT y+1 FROM tab WHERE y+1!=5
	 *     UNION ALL
	 *     SELECT abs(z*2)+1 FROM tab2 WHERE abs(z*2)+1!=5
	 *     ORDER BY 1
	 *
	 * We call this the "compound-subquery flattening".
	 */
	for (pSub = pSub->pPrior; pSub; pSub = pSub->pPrior) {
		Select *pNew;
		ExprList *pOrderBy = p->pOrderBy;
		Expr *pLimit = p->pLimit;
		Expr *pOffset = p->pOffset;
		Select *pPrior = p->pPrior;
		p->pOrderBy = 0;
		p->pSrc = 0;
		p->pPrior = 0;
		p->pLimit = 0;
		p->pOffset = 0;
		pNew = sqlite3SelectDup(db, p, 0);
		sqlite3SelectSetName(pNew, pSub->zSelName);
		p->pOffset = pOffset;
		p->pLimit = pLimit;
		p->pOrderBy = pOrderBy;
		p->pSrc = pSrc;
		p->op = TK_ALL;
		if (pNew == 0) {
			p->pPrior = pPrior;
		} else {
			pNew->pPrior = pPrior;
			if (pPrior)
				pPrior->pNext = pNew;
			pNew->pNext = p;
			p->pPrior = pNew;
			SELECTTRACE(2, pParse, p,
				    ("compound-subquery flattener creates %s.%p as peer\n",
				     pNew->zSelName, pNew));
		}
		if (db->mallocFailed)
			return 1;
	}

	/* Begin flattening the iFrom-th entry of the FROM clause
	 * in the outer query.
	 */
	pSub = pSub1 = pSubitem->pSelect;

	/* Delete the transient table structure associated with the
	 * subquery
	 */
	sqlite3DbFree(db, pSubitem->zName);
	sqlite3DbFree(db, pSubitem->zAlias);
	pSubitem->zName = 0;
	pSubitem->zAlias = 0;
	pSubitem->pSelect = 0;

	/* Defer deleting the Table object associated with the
	 * subquery until code generation is
	 * complete, since there may still exist Expr.pTab entries that
	 * refer to the subquery even after flattening.  Ticket #3346.
	 *
	 * pSubitem->pTab is always non-NULL by test restrictions and tests above.
	 */
	if (ALWAYS(pSubitem->pTab != 0)) {
		Table *pTabToDel = pSubitem->pTab;
		if (pTabToDel->nTabRef == 1) {
			Parse *pToplevel = sqlite3ParseToplevel(pParse);
			pTabToDel->pNextZombie = pToplevel->pZombieTab;
			pToplevel->pZombieTab = pTabToDel;
		} else {
			pTabToDel->nTabRef--;
		}
		pSubitem->pTab = 0;
	}

	/* The following loop runs once for each term in a compound-subquery
	 * flattening (as described above).  If we are doing a different kind
	 * of flattening - a flattening other than a compound-subquery flattening -
	 * then this loop only runs once.
	 *
	 * This loop moves all of the FROM elements of the subquery into the
	 * the FROM clause of the outer query.  Before doing this, remember
	 * the cursor number for the original outer query FROM element in
	 * iParent.  The iParent cursor will never be used.  Subsequent code
	 * will scan expressions looking for iParent references and replace
	 * those references with expressions that resolve to the subquery FROM
	 * elements we are now copying in.
	 */
	for (pParent = p; pParent;
	     pParent = pParent->pPrior, pSub = pSub->pPrior) {
		int nSubSrc;
		u8 jointype = 0;
		pSubSrc = pSub->pSrc;	/* FROM clause of subquery */
		nSubSrc = pSubSrc->nSrc;	/* Number of terms in subquery FROM clause */
		pSrc = pParent->pSrc;	/* FROM clause of the outer query */

		if (pSrc) {
			assert(pParent == p);	/* First time through the loop */
			jointype = pSubitem->fg.jointype;
		} else {
			assert(pParent != p);	/* 2nd and subsequent times through the loop */
			pSrc = pParent->pSrc =
			    sqlite3SrcListAppend(db, 0, 0);
			if (pSrc == 0) {
				assert(db->mallocFailed);
				break;
			}
		}

		/* The subquery uses a single slot of the FROM clause of the outer
		 * query.  If the subquery has more than one element in its FROM clause,
		 * then expand the outer query to make space for it to hold all elements
		 * of the subquery.
		 *
		 * Example:
		 *
		 *    SELECT * FROM tabA, (SELECT * FROM sub1, sub2), tabB;
		 *
		 * The outer query has 3 slots in its FROM clause.  One slot of the
		 * outer query (the middle slot) is used by the subquery.  The next
		 * block of code will expand the outer query FROM clause to 4 slots.
		 * The middle slot is expanded to two slots in order to make space
		 * for the two elements in the FROM clause of the subquery.
		 */
		if (nSubSrc > 1) {
			pParent->pSrc = pSrc =
			    sqlite3SrcListEnlarge(db, pSrc, nSubSrc - 1,
						  iFrom + 1);
			if (db->mallocFailed) {
				break;
			}
		}

		/* Transfer the FROM clause terms from the subquery into the
		 * outer query.
		 */
		for (i = 0; i < nSubSrc; i++) {
			sqlite3IdListDelete(db, pSrc->a[i + iFrom].pUsing);
			assert(pSrc->a[i + iFrom].fg.isTabFunc == 0);
			pSrc->a[i + iFrom] = pSubSrc->a[i];
			memset(&pSubSrc->a[i], 0, sizeof(pSubSrc->a[i]));
		}
		pSrc->a[iFrom].fg.jointype = jointype;

		/* Now begin substituting subquery result set expressions for
		 * references to the iParent in the outer query.
		 *
		 * Example:
		 *
		 *   SELECT a+5, b*10 FROM (SELECT x*3 AS a, y+10 AS b FROM t1) WHERE a>b;
		 *   \                     \_____________ subquery __________/          /
		 *    \_____________________ outer query ______________________________/
		 *
		 * We look at every expression in the outer query and every place we see
		 * "a" we substitute "x*3" and every place we see "b" we substitute "y+10".
		 */
		pList = pParent->pEList;
		for (i = 0; i < pList->nExpr; i++) {
			if (pList->a[i].zName == 0) {
				char *zName =
				    sqlite3DbStrDup(db, pList->a[i].zSpan);
				sqlite3NormalizeName(zName);
				pList->a[i].zName = zName;
			}
		}
		if (pSub->pOrderBy) {
			/* At this point, any non-zero iOrderByCol values indicate that the
			 * ORDER BY column expression is identical to the iOrderByCol'th
			 * expression returned by SELECT statement pSub. Since these values
			 * do not necessarily correspond to columns in SELECT statement pParent,
			 * zero them before transfering the ORDER BY clause.
			 *
			 * Not doing this may cause an error if a subsequent call to this
			 * function attempts to flatten a compound sub-query into pParent
			 * (the only way this can happen is if the compound sub-query is
			 * currently part of pSub->pSrc). See ticket [d11a6e908f].
			 */
			ExprList *pOrderBy = pSub->pOrderBy;
			for (i = 0; i < pOrderBy->nExpr; i++) {
				pOrderBy->a[i].u.x.iOrderByCol = 0;
			}
			assert(pParent->pOrderBy == 0);
			assert(pSub->pPrior == 0);
			pParent->pOrderBy = pOrderBy;
			pSub->pOrderBy = 0;
		}
		pWhere = sqlite3ExprDup(db, pSub->pWhere, 0);
		if (subqueryIsAgg) {
			assert(pParent->pHaving == 0);
			pParent->pHaving = pParent->pWhere;
			pParent->pWhere = pWhere;
			pParent->pHaving = sqlite3ExprAnd(db,
							  sqlite3ExprDup(db,
									 pSub->pHaving,
									 0),
							  pParent->pHaving);
			assert(pParent->pGroupBy == 0);
			pParent->pGroupBy =
			    sql_expr_list_dup(db, pSub->pGroupBy, 0);
		} else {
			pParent->pWhere =
			    sqlite3ExprAnd(db, pWhere, pParent->pWhere);
		}
		substSelect(pParse, pParent, iParent, pSub->pEList, 0);

		/* The flattened query is distinct if either the inner or the
		 * outer query is distinct.
		 */
		pParent->selFlags |= pSub->selFlags & SF_Distinct;

		/*
		 * SELECT ... FROM (SELECT ... LIMIT a OFFSET b) LIMIT x OFFSET y;
		 *
		 * One is tempted to try to add a and b to combine the limits.  But this
		 * does not work if either limit is negative.
		 */
		if (pSub->pLimit) {
			pParent->pLimit = pSub->pLimit;
			pSub->pLimit = 0;
		}
	}

	/* Finially, delete what is left of the subquery and return
	 * success.
	 */
	sql_select_delete(db, pSub1);

#ifdef SELECTTRACE_ENABLED
	if (sqlite3SelectTrace & 0x100) {
		SELECTTRACE(0x100, pParse, p, ("After flattening:\n"));
		sqlite3TreeViewSelect(0, p, 0);
	}
#endif

	return 1;
}

/*
 * Make copies of relevant WHERE clause terms of the outer query into
 * the WHERE clause of subquery.  Example:
 *
 *    SELECT * FROM (SELECT a AS x, c-d AS y FROM t1) WHERE x=5 AND y=10;
 *
 * Transformed into:
 *
 *    SELECT * FROM (SELECT a AS x, c-d AS y FROM t1 WHERE a=5 AND c-d=10)
 *     WHERE x=5 AND y=10;
 *
 * The hope is that the terms added to the inner query will make it more
 * efficient.
 *
 * Do not attempt this optimization if:
 *
 *   (1) The inner query is an aggregate.  (In that case, we'd really want
 *       to copy the outer WHERE-clause terms onto the HAVING clause of the
 *       inner query.  But they probably won't help there so do not bother.)
 *
 *   (2) The inner query is the recursive part of a common table expression.
 *
 *   (3) The inner query has a LIMIT clause (since the changes to the WHERE
 *       close would change the meaning of the LIMIT).
 *
 *   (4) The inner query is the right operand of a LEFT JOIN.  (The caller
 *       enforces this restriction since this routine does not have enough
 *       information to know.)
 *
 *   (5) The WHERE clause expression originates in the ON or USING clause
 *       of a LEFT JOIN.
 *
 * Return 0 if no changes are made and non-zero if one or more WHERE clause
 * terms are duplicated into the subquery.
 */
static int
pushDownWhereTerms(Parse * pParse,	/* Parse context (for malloc() and error reporting) */
		   Select * pSubq,	/* The subquery whose WHERE clause is to be augmented */
		   Expr * pWhere,	/* The WHERE clause of the outer query */
		   int iCursor)		/* Cursor number of the subquery */
{
	Expr *pNew;
	int nChng = 0;
	Select *pX;		/* For looping over compound SELECTs in pSubq */
	if (pWhere == 0)
		return 0;
	for (pX = pSubq; pX; pX = pX->pPrior) {
		if ((pX->selFlags & (SF_Aggregate | SF_Recursive)) != 0) {
			testcase(pX->selFlags & SF_Aggregate);
			testcase(pX->selFlags & SF_Recursive);
			testcase(pX != pSubq);
			return 0;	/* restrictions (1) and (2) */
		}
	}
	if (pSubq->pLimit != 0) {
		return 0;	/* restriction (3) */
	}
	while (pWhere->op == TK_AND) {
		nChng +=
		    pushDownWhereTerms(pParse, pSubq, pWhere->pRight, iCursor);
		pWhere = pWhere->pLeft;
	}
	if (ExprHasProperty(pWhere, EP_FromJoin))
		return 0;	/* restriction 5 */
	if (sqlite3ExprIsTableConstant(pWhere, iCursor)) {
		nChng++;
		while (pSubq) {
			pNew = sqlite3ExprDup(pParse->db, pWhere, 0);
			pNew = substExpr(pParse, pNew, iCursor, pSubq->pEList);
			pSubq->pWhere =
			    sqlite3ExprAnd(pParse->db, pSubq->pWhere, pNew);
			pSubq = pSubq->pPrior;
		}
	}
	return nChng;
}

/*
 * Based on the contents of the AggInfo structure indicated by the first
 * argument, this function checks if the following are true:
 *
 *    * the query contains just a single aggregate function,
 *    * the aggregate function is either min() or max(), and
 *    * the argument to the aggregate function is a column value.
 *
 * If all of the above are true, then WHERE_ORDERBY_MIN or WHERE_ORDERBY_MAX
 * is returned as appropriate. Also, *ppMinMax is set to point to the
 * list of arguments passed to the aggregate before returning.
 *
 * Or, if the conditions above are not met, *ppMinMax is set to 0 and
 * WHERE_ORDERBY_NORMAL is returned.
 */
static u8
minMaxQuery(AggInfo * pAggInfo, ExprList ** ppMinMax)
{
	int eRet = WHERE_ORDERBY_NORMAL;	/* Return value */

	*ppMinMax = 0;
	if (pAggInfo->nFunc == 1) {
		Expr *pExpr = pAggInfo->aFunc[0].pExpr;	/* Aggregate function */
		ExprList *pEList = pExpr->x.pList;	/* Arguments to agg function */

		assert(pExpr->op == TK_AGG_FUNCTION);
		if (pEList && pEList->nExpr == 1
		    && pEList->a[0].pExpr->op == TK_AGG_COLUMN) {
			const char *zFunc = pExpr->u.zToken;
			if (sqlite3StrICmp(zFunc, "min") == 0) {
				eRet = WHERE_ORDERBY_MIN;
				*ppMinMax = pEList;
			} else if (sqlite3StrICmp(zFunc, "max") == 0) {
				eRet = WHERE_ORDERBY_MAX;
				*ppMinMax = pEList;
			}
		}
	}

	assert(*ppMinMax == 0 || (*ppMinMax)->nExpr == 1);
	return eRet;
}

/**
 * The second argument is the associated aggregate-info object.
 * This function tests if the SELECT is of the form:
 *
 *   SELECT count(*) FROM <tbl>
 *
 * where table is not a sub-select or view.
 *
 * @param select The select statement in form of aggregate query.
 * @param agg_info The associated aggregate-info object.
 * @retval Pointer to space representing the table,
 *         if the query matches this pattern. NULL otherwise.
 */
static struct space*
is_simple_count(struct Select *select, struct AggInfo *agg_info)
{
	assert(select->pGroupBy == NULL);
	if (select->pWhere != NULL || select->pEList->nExpr != 1 ||
	    select->pSrc->nSrc != 1 || select->pSrc->a[0].pSelect != NULL) {
		return NULL;
	}
	uint32_t space_id = select->pSrc->a[0].pTab->def->id;
	struct space *space = space_by_id(space_id);
	assert(space != NULL && !space->def->opts.is_view);
	struct Expr *expr = select->pEList->a[0].pExpr;
	assert(expr != NULL);
	if (expr->op != TK_AGG_FUNCTION)
		return NULL;
	if (NEVER(agg_info->nFunc == 0))
		return NULL;
	if ((agg_info->aFunc[0].pFunc->funcFlags & SQLITE_FUNC_COUNT) == 0)
		return NULL;
	if (expr->flags & EP_Distinct)
		return NULL;
	return space;
}

/*
 * If the source-list item passed as an argument was augmented with an
 * INDEXED BY clause, then try to locate the specified index. If there
 * was such a clause and the named index cannot be found, return
 * SQLITE_ERROR and leave an error in pParse. Otherwise, populate
 * pFrom->pIndex and return SQLITE_OK.
 */
int
sqlite3IndexedByLookup(Parse * pParse, struct SrcList_item *pFrom)
{
	if (pFrom->pTab && pFrom->fg.isIndexedBy) {
		Table *pTab = pFrom->pTab;
		char *zIndexedBy = pFrom->u1.zIndexedBy;
		Index *pIdx;
		for (pIdx = pTab->pIndex;
		     pIdx && strcmp(pIdx->def->name, zIndexedBy);
		     pIdx = pIdx->pNext) ;
		if (!pIdx) {
			sqlite3ErrorMsg(pParse, "no such index: %s", zIndexedBy,
					0);
			return SQLITE_ERROR;
		}
		pFrom->pIBIndex = pIdx;
	}
	return SQLITE_OK;
}

/*
 * Detect compound SELECT statements that use an ORDER BY clause with
 * an alternative collating sequence.
 *
 *    SELECT ... FROM t1 EXCEPT SELECT ... FROM t2 ORDER BY .. COLLATE ...
 *
 * These are rewritten as a subquery:
 *
 *    SELECT * FROM (SELECT ... FROM t1 EXCEPT SELECT ... FROM t2)
 *     ORDER BY ... COLLATE ...
 *
 * This transformation is necessary because the multiSelectOrderBy() routine
 * above that generates the code for a compound SELECT with an ORDER BY clause
 * uses a merge algorithm that requires the same collating sequence on the
 * result columns as on the ORDER BY clause.  See ticket
 * http://www.sqlite.org/src/info/6709574d2a
 *
 * This transformation is only needed for EXCEPT, INTERSECT, and UNION.
 * The UNION ALL operator works fine with multiSelectOrderBy() even when
 * there are COLLATE terms in the ORDER BY.
 */
static int
convertCompoundSelectToSubquery(Walker * pWalker, Select * p)
{
	int i;
	Select *pNew;
	Select *pX;
	sqlite3 *db;
	struct ExprList_item *a;
	SrcList *pNewSrc;
	Parse *pParse;
	Token dummy;

	if (p->pPrior == 0)
		return WRC_Continue;
	if (p->pOrderBy == 0)
		return WRC_Continue;
	for (pX = p; pX && (pX->op == TK_ALL || pX->op == TK_SELECT);
	     pX = pX->pPrior) {
	}
	if (pX == 0)
		return WRC_Continue;
	a = p->pOrderBy->a;
	for (i = p->pOrderBy->nExpr - 1; i >= 0; i--) {
		if (a[i].pExpr->flags & EP_Collate)
			break;
	}
	if (i < 0)
		return WRC_Continue;

	/* If we reach this point, that means the transformation is required. */

	pParse = pWalker->pParse;
	db = pParse->db;
	pNew = sqlite3DbMallocZero(db, sizeof(*pNew));
	if (pNew == 0)
		return WRC_Abort;
	memset(&dummy, 0, sizeof(dummy));
	pNewSrc =
	    sqlite3SrcListAppendFromTerm(pParse, 0, 0, &dummy, pNew, 0, 0);
	if (pNewSrc == 0)
		return WRC_Abort;
	*pNew = *p;
	p->pSrc = pNewSrc;
	p->pEList = sql_expr_list_append(pParse->db, NULL,
					 sqlite3Expr(db, TK_ASTERISK, 0));
	p->op = TK_SELECT;
	p->pWhere = 0;
	pNew->pGroupBy = 0;
	pNew->pHaving = 0;
	pNew->pOrderBy = 0;
	p->pPrior = 0;
	p->pNext = 0;
	p->pWith = 0;
	p->selFlags &= ~SF_Compound;
	assert((p->selFlags & SF_Converted) == 0);
	p->selFlags |= SF_Converted;
	assert(pNew->pPrior != 0);
	pNew->pPrior->pNext = pNew;
	pNew->pLimit = 0;
	pNew->pOffset = 0;
	return WRC_Continue;
}

/*
 * Check to see if the FROM clause term pFrom has table-valued function
 * arguments.  If it does, leave an error message in pParse and return
 * non-zero, since pFrom is not allowed to be a table-valued function.
 */
static int
cannotBeFunction(Parse * pParse, struct SrcList_item *pFrom)
{
	if (pFrom->fg.isTabFunc) {
		sqlite3ErrorMsg(pParse, "'%s' is not a function", pFrom->zName);
		return 1;
	}
	return 0;
}

#ifndef SQLITE_OMIT_CTE
/*
 * Argument pWith (which may be NULL) points to a linked list of nested
 * WITH contexts, from inner to outermost. If the table identified by
 * FROM clause element pItem is really a common-table-expression (CTE)
 * then return a pointer to the CTE definition for that table. Otherwise
 * return NULL.
 *
 * If a non-NULL value is returned, set *ppContext to point to the With
 * object that the returned CTE belongs to.
 */
static struct Cte *
searchWith(With * pWith,		/* Current innermost WITH clause */
	   struct SrcList_item *pItem,	/* FROM clause element to resolve */
	   With ** ppContext)		/* OUT: WITH clause return value belongs to */
{
	const char *zName;
	if ((zName = pItem->zName) != 0) {
		With *p;
		for (p = pWith; p; p = p->pOuter) {
			int i;
			for (i = 0; i < p->nCte; i++) {
				if (strcmp(zName, p->a[i].zName) == 0) {
					*ppContext = p;
					return &p->a[i];
				}
			}
		}
	}
	return 0;
}

/* The code generator maintains a stack of active WITH clauses
 * with the inner-most WITH clause being at the top of the stack.
 *
 * This routine pushes the WITH clause passed as the second argument
 * onto the top of the stack. If argument bFree is true, then this
 * WITH clause will never be popped from the stack. In this case it
 * should be freed along with the Parse object. In other cases, when
 * bFree==0, the With object will be freed along with the SELECT
 * statement with which it is associated.
 */
void
sqlite3WithPush(Parse * pParse, With * pWith, u8 bFree)
{
	assert(bFree == 0 || (pParse->pWith == 0 && pParse->pWithToFree == 0));
	if (pWith) {
		assert(pParse->pWith != pWith);
		pWith->pOuter = pParse->pWith;
		pParse->pWith = pWith;
		if (bFree)
			pParse->pWithToFree = pWith;
	}
}

/*
 * This function checks if argument pFrom refers to a CTE declared by
 * a WITH clause on the stack currently maintained by the parser. And,
 * if currently processing a CTE expression, if it is a recursive
 * reference to the current CTE.
 *
 * If pFrom falls into either of the two categories above, pFrom->pTab
 * and other fields are populated accordingly. The caller should check
 * (pFrom->pTab!=0) to determine whether or not a successful match
 * was found.
 *
 * Whether or not a match is found, SQLITE_OK is returned if no error
 * occurs. If an error does occur, an error message is stored in the
 * parser and some error code other than SQLITE_OK returned.
 */
static int
withExpand(Walker * pWalker, struct SrcList_item *pFrom)
{
	Parse *pParse = pWalker->pParse;
	sqlite3 *db = pParse->db;
	struct Cte *pCte;	/* Matched CTE (or NULL if no match) */
	With *pWith;		/* WITH clause that pCte belongs to */

	assert(pFrom->pTab == 0);

	pCte = searchWith(pParse->pWith, pFrom, &pWith);
	if (pCte) {
		Table *pTab;
		ExprList *pEList;
		Select *pSel;
		Select *pLeft;	/* Left-most SELECT statement */
		int bMayRecursive;	/* True if compound joined by UNION [ALL] */
		With *pSavedWith;	/* Initial value of pParse->pWith */

		/* If pCte->zCteErr is non-NULL at this point, then this is an illegal
		 * recursive reference to CTE pCte. Leave an error in pParse and return
		 * early. If pCte->zCteErr is NULL, then this is not a recursive reference.
		 * In this case, proceed.
		 */
		if (pCte->zCteErr) {
			sqlite3ErrorMsg(pParse, pCte->zCteErr, pCte->zName);
			return SQLITE_ERROR;
		}
		if (cannotBeFunction(pParse, pFrom))
			return SQLITE_ERROR;

		assert(pFrom->pTab == 0);
		pFrom->pTab = pTab =
			sql_ephemeral_table_new(pParse, pCte->zName);
		if (pTab == NULL)
			return WRC_Abort;
		pTab->nTabRef = 1;
		pTab->tuple_log_count = DEFAULT_TUPLE_LOG_COUNT;
		assert(sqlite3LogEst(DEFAULT_TUPLE_COUNT) ==
		       DEFAULT_TUPLE_LOG_COUNT);
		pFrom->pSelect = sqlite3SelectDup(db, pCte->pSelect, 0);
		if (db->mallocFailed)
			return SQLITE_NOMEM_BKPT;
		assert(pFrom->pSelect);

		/* Check if this is a recursive CTE. */
		pSel = pFrom->pSelect;
		bMayRecursive = (pSel->op == TK_ALL || pSel->op == TK_UNION);
		if (bMayRecursive) {
			int i;
			SrcList *pSrc = pFrom->pSelect->pSrc;
			for (i = 0; i < pSrc->nSrc; i++) {
				struct SrcList_item *pItem = &pSrc->a[i];
				if (pItem->zName != 0
				    && 0 == sqlite3StrICmp(pItem->zName,
							   pCte->zName)
				    ) {
					pItem->pTab = pTab;
					pItem->fg.isRecursive = 1;
					pTab->nTabRef++;
					pSel->selFlags |= SF_Recursive;
				}
			}
		}

		/* Only one recursive reference is permitted. */
		if (pTab->nTabRef > 2) {
			sqlite3ErrorMsg(pParse,
					"multiple references to recursive table: %s",
					pCte->zName);
			return SQLITE_ERROR;
		}
		assert(pTab->nTabRef == 1
		       || ((pSel->selFlags & SF_Recursive)
			   && pTab->nTabRef == 2));

		pCte->zCteErr = "circular reference: %s";
		pSavedWith = pParse->pWith;
		pParse->pWith = pWith;
		sqlite3WalkSelect(pWalker, bMayRecursive ? pSel->pPrior : pSel);
		pParse->pWith = pWith;

		for (pLeft = pSel; pLeft->pPrior; pLeft = pLeft->pPrior) ;
		pEList = pLeft->pEList;
		if (pCte->pCols) {
			if (pEList && pEList->nExpr != pCte->pCols->nExpr) {
				sqlite3ErrorMsg(pParse,
						"table %s has %d values for %d columns",
						pCte->zName, pEList->nExpr,
						pCte->pCols->nExpr);
				pParse->pWith = pSavedWith;
				return SQLITE_ERROR;
			}
			pEList = pCte->pCols;
		}

		sqlite3ColumnsFromExprList(pParse, pEList, pTab);

		if (bMayRecursive) {
			if (pSel->selFlags & SF_Recursive) {
				pCte->zCteErr =
				    "multiple recursive references: %s";
			} else {
				pCte->zCteErr =
				    "recursive reference in a subquery: %s";
			}
			sqlite3WalkSelect(pWalker, pSel);
		}
		pCte->zCteErr = 0;
		pParse->pWith = pSavedWith;
	}

	return SQLITE_OK;
}
#endif

#ifndef SQLITE_OMIT_CTE
/*
 * If the SELECT passed as the second argument has an associated WITH
 * clause, pop it from the stack stored as part of the Parse object.
 *
 * This function is used as the xSelectCallback2() callback by
 * sqlite3SelectExpand() when walking a SELECT tree to resolve table
 * names and other FROM clause elements.
 */
static void
selectPopWith(Walker * pWalker, Select * p)
{
	Parse *pParse = pWalker->pParse;
	With *pWith = findRightmost(p)->pWith;
	if (pWith != 0) {
		assert(pParse->pWith == pWith);
		pParse->pWith = pWith->pOuter;
	}
}
#else
#define selectPopWith 0
#endif

/*
 * This routine is a Walker callback for "expanding" a SELECT statement.
 * "Expanding" means to do the following:
 *
 *    (1)  Make sure VDBE cursor numbers have been assigned to every
 *         element of the FROM clause.
 *
 *    (2)  Fill in the pTabList->a[].pTab fields in the SrcList that
 *         defines FROM clause.  When views appear in the FROM clause,
 *         fill pTabList->a[].pSelect with a copy of the SELECT statement
 *         that implements the view.  A copy is made of the view's SELECT
 *         statement so that we can freely modify or delete that statement
 *         without worrying about messing up the persistent representation
 *         of the view.
 *
 *    (3)  Add terms to the WHERE clause to accommodate the NATURAL keyword
 *         on joins and the ON and USING clause of joins.
 *
 *    (4)  Scan the list of columns in the result set (pEList) looking
 *         for instances of the "*" operator or the TABLE.* operator.
 *         If found, expand each "*" to be every column in every table
 *         and TABLE.* to be every column in TABLE.
 *
 */
static int
selectExpander(Walker * pWalker, Select * p)
{
	Parse *pParse = pWalker->pParse;
	int i, j, k;
	SrcList *pTabList;
	ExprList *pEList;
	struct SrcList_item *pFrom;
	sqlite3 *db = pParse->db;
	Expr *pE, *pRight, *pExpr;
	u16 selFlags = p->selFlags;
	struct session *user_session = current_session();

	p->selFlags |= SF_Expanded;
	if (db->mallocFailed) {
		return WRC_Abort;
	}
	if (NEVER(p->pSrc == 0) || (selFlags & SF_Expanded) != 0) {
		return WRC_Prune;
	}
	pTabList = p->pSrc;
	pEList = p->pEList;
	if (pWalker->xSelectCallback2 == selectPopWith) {
		sqlite3WithPush(pParse, findRightmost(p)->pWith, 0);
	}

	/* Make sure cursor numbers have been assigned to all entries in
	 * the FROM clause of the SELECT statement.
	 */
	sqlite3SrcListAssignCursors(pParse, pTabList);

	/* Look up every table named in the FROM clause of the select.  If
	 * an entry of the FROM clause is a subquery instead of a table or view,
	 * then create a transient table structure to describe the subquery.
	 */
	for (i = 0, pFrom = pTabList->a; i < pTabList->nSrc; i++, pFrom++) {
		Table *pTab;
		assert(pFrom->fg.isRecursive == 0 || pFrom->pTab != 0);
		if (pFrom->fg.isRecursive)
			continue;
		assert(pFrom->pTab == 0);
#ifndef SQLITE_OMIT_CTE
		if (withExpand(pWalker, pFrom))
			return WRC_Abort;
		if (pFrom->pTab) {
		} else
#endif
		if (pFrom->zName == 0) {
			Select *pSel = pFrom->pSelect;
			/* A sub-query in the FROM clause of a SELECT */
			assert(pSel != 0);
			assert(pFrom->pTab == 0);
			if (sqlite3WalkSelect(pWalker, pSel))
				return WRC_Abort;
			/*
			 * Will be overwritten with pointer as
			 * unique identifier.
			 */
			const char *name = "sqlite_sq_DEADBEAFDEADBEAF";
			pFrom->pTab = pTab =
				sql_ephemeral_table_new(pParse, name);
			if (pTab == NULL)
				return WRC_Abort;
			/*
			 * Rewrite old name with correct pointer.
			 */
			name = tt_sprintf("sqlite_sq_%llX", (void *)pTab);
			sprintf(pTab->def->name, "%s", name);
			pTab->nTabRef = 1;
			while (pSel->pPrior) {
				pSel = pSel->pPrior;
			}
			sqlite3ColumnsFromExprList(pParse, pSel->pEList, pTab);
			if (sql_table_def_rebuild(db, pTab) != 0)
				return WRC_Abort;
			pTab->tuple_log_count = DEFAULT_TUPLE_LOG_COUNT;
			assert(sqlite3LogEst(DEFAULT_TUPLE_COUNT) ==
			       DEFAULT_TUPLE_LOG_COUNT);
		} else {
			/*
			 * An ordinary table or view name in the
			 * FROM clause.
			 */
			assert(pFrom->pTab == NULL);
			const char *t_name = pFrom->zName;
			pFrom->pTab = pTab =
			    sqlite3LocateTable(pParse, LOCATE_NOERR, t_name);
			if (pTab == NULL) {
				int space_id =
					box_space_id_by_name(t_name,
							     strlen(t_name));
				struct space *space = space_by_id(space_id);
				if (space == NULL) {
					sqlite3ErrorMsg(pParse,
							"no such table: %s",
							t_name);
					return WRC_Abort;
				}
				if (space->def->field_count <= 0) {
					sqlite3ErrorMsg(pParse, "no format for"\
							" space: %s", t_name);
					return WRC_Abort;
				}
				struct Table *tab =
					sqlite3DbMallocZero(db, sizeof(*tab));
				if (tab == NULL)
					return WRC_Abort;
				tab->nTabRef = 1;
				tab->def = space_def_dup(space->def);
				pFrom->pTab = pTab = tab;
			} else {
				if (pTab->nTabRef >= 0xffff) {
					sqlite3ErrorMsg(pParse, "too many "\
							"references to "\
							"\"%s\": max 65535",
							t_name);
					pFrom->pTab = NULL;
					return WRC_Abort;
				}
				pTab->nTabRef++;
			}
			if (cannotBeFunction(pParse, pFrom))
				return WRC_Abort;
			if (pTab->def->opts.is_view) {
				struct Select *select =
					sql_view_compile(db,
							 pTab->def->opts.sql);
				if (select == NULL)
					return WRC_Abort;
				sqlite3SrcListAssignCursors(pParse,
							    select->pSrc);
				assert(pFrom->pSelect == 0);
				pFrom->pSelect = select;
				sqlite3SelectSetName(pFrom->pSelect,
						     pTab->def->name);
				int columns = pTab->def->field_count;
				pTab->def->field_count = -1;
				sqlite3WalkSelect(pWalker, pFrom->pSelect);
				pTab->def->field_count = columns;
			}
		}
		/* Locate the index named by the INDEXED BY clause, if any. */
		if (sqlite3IndexedByLookup(pParse, pFrom)) {
			return WRC_Abort;
		}
	}

	/* Process NATURAL keywords, and ON and USING clauses of joins.
	 */
	if (db->mallocFailed || sqliteProcessJoin(pParse, p)) {
		return WRC_Abort;
	}

	/* For every "*" that occurs in the column list, insert the names of
	 * all columns in all tables.  And for every TABLE.* insert the names
	 * of all columns in TABLE.  The parser inserted a special expression
	 * with the TK_ASTERISK operator for each "*" that it found in the column
	 * list.  The following code just has to locate the TK_ASTERISK
	 * expressions and expand each one to the list of all columns in
	 * all tables.
	 *
	 * The first loop just checks to see if there are any "*" operators
	 * that need expanding.
	 */
	for (k = 0; k < pEList->nExpr; k++) {
		pE = pEList->a[k].pExpr;
		if (pE->op == TK_ASTERISK)
			break;
		assert(pE->op != TK_DOT || pE->pRight != 0);
		assert(pE->op != TK_DOT
		       || (pE->pLeft != 0 && pE->pLeft->op == TK_ID));
		if (pE->op == TK_DOT && pE->pRight->op == TK_ASTERISK)
			break;
	}
	if (k < pEList->nExpr) {
		/*
		 * If we get here it means the result set contains one or more "*"
		 * operators that need to be expanded.  Loop through each expression
		 * in the result set and expand them one by one.
		 */
		struct ExprList_item *a = pEList->a;
		ExprList *pNew = 0;
		uint32_t flags = user_session->sql_flags;
		int longNames = (flags & SQLITE_FullColNames) != 0
		    && (flags & SQLITE_ShortColNames) == 0;

		for (k = 0; k < pEList->nExpr; k++) {
			pE = a[k].pExpr;
			pRight = pE->pRight;
			assert(pE->op != TK_DOT || pRight != 0);
			if (pE->op != TK_ASTERISK
			    && (pE->op != TK_DOT || pRight->op != TK_ASTERISK)
			    ) {
				/* This particular expression does not need to be expanded.
				 */
				pNew = sql_expr_list_append(pParse->db, pNew,
							    a[k].pExpr);
				if (pNew != NULL) {
					pNew->a[pNew->nExpr - 1].zName =
					    a[k].zName;
					pNew->a[pNew->nExpr - 1].zSpan =
					    a[k].zSpan;
					a[k].zName = 0;
					a[k].zSpan = 0;
				}
				a[k].pExpr = 0;
			} else {
				/* This expression is a "*" or a "TABLE.*" and needs to be
				 * expanded.
				 */
				int tableSeen = 0;	/* Set to 1 when TABLE matches */
				char *zTName = 0;	/* text of name of TABLE */
				if (pE->op == TK_DOT) {
					assert(pE->pLeft != 0);
					assert(!ExprHasProperty
					       (pE->pLeft, EP_IntValue));
					zTName = pE->pLeft->u.zToken;
				}
				for (i = 0, pFrom = pTabList->a;
				     i < pTabList->nSrc; i++, pFrom++) {
					Table *pTab = pFrom->pTab;
					Select *pSub = pFrom->pSelect;
					char *zTabName = pFrom->zAlias;
					if (zTabName == 0) {
						zTabName = pTab->def->name;
					}
					if (db->mallocFailed)
						break;
					if (pSub == 0
					    || (pSub->
						selFlags & SF_NestedFrom) ==
					    0) {
						pSub = 0;
						if (zTName
						    && strcmp(zTName,
								      zTabName)
						    != 0) {
							continue;
						}
					}
					for (j = 0; j < (int)pTab->def->field_count; j++) {
						char *zName = pTab->def->fields[j].name;
						char *zColname;	/* The computed column name */
						char *zToFree;	/* Malloced string that needs to be freed */
						Token sColname;	/* Computed column name as a token */

						assert(zName);
						if (zTName && pSub
						    && sqlite3MatchSpanName(pSub->pEList->a[j].zSpan,
									    0,
									    zTName) == 0) {
							continue;
						}
						tableSeen = 1;

						if (i > 0 && zTName == 0) {
							if ((pFrom->fg.jointype & JT_NATURAL) != 0
							    && tableAndColumnIndex(pTabList, i, zName, 0, 0)) {
								/* In a NATURAL join, omit the join columns from the
								 * table to the right of the join
								 */
								continue;
							}
							if (sqlite3IdListIndex(pFrom->pUsing, zName) >= 0) {
								/* In a join with a USING clause, omit columns in the
								 * using clause from the table on the right.
								 */
								continue;
							}
						}
						pRight =
						    sqlite3Expr(db, TK_ID,
								zName);
						zColname = zName;
						zToFree = 0;
						if (longNames
						    || pTabList->nSrc > 1) {
							Expr *pLeft;
							pLeft =
							    sqlite3Expr(db,
									TK_ID,
									zTabName);
							pExpr =
							    sqlite3PExpr(pParse,
									 TK_DOT,
									 pLeft,
									 pRight);
							if (longNames) {
								zColname =
								    sqlite3MPrintf
								    (db,
								     "%s.%s",
								     zTabName,
								     zName);
								zToFree =
								    zColname;
							}
						} else {
							pExpr = pRight;
						}
						pNew = sql_expr_list_append(
							pParse->db, pNew, pExpr);
						sqlite3TokenInit(&sColname, zColname);
						sqlite3ExprListSetName(pParse,
								       pNew,
								       &sColname,
								       0);
						if (pNew != NULL
						    && (p->
							selFlags &
							SF_NestedFrom) != 0) {
							struct ExprList_item *pX
							    =
							    &pNew->a[pNew->
								     nExpr - 1];
							if (pSub) {
								pX->zSpan = sqlite3DbStrDup(db,
											    pSub->pEList->a[j].zSpan);
								testcase(pX->zSpan == 0);
							} else {
								pX->zSpan = sqlite3MPrintf(db,
											   "%s.%s",
											   zTabName,
											   zColname);
								testcase(pX->zSpan == 0);
							}
							pX->bSpanIsTab = 1;
						}
						sqlite3DbFree(db, zToFree);
					}
				}
				if (!tableSeen) {
					if (zTName) {
						sqlite3ErrorMsg(pParse,
								"no such table: %s",
								zTName);
					} else {
						sqlite3ErrorMsg(pParse,
								"no tables specified");
					}
				}
			}
		}
		sql_expr_list_delete(db, pEList);
		p->pEList = pNew;
	}
#if SQLITE_MAX_COLUMN
	if (p->pEList && p->pEList->nExpr > db->aLimit[SQLITE_LIMIT_COLUMN]) {
		sqlite3ErrorMsg(pParse, "too many columns in result set");
		return WRC_Abort;
	}
#endif
	return WRC_Continue;
}

/*
 * No-op routine for the parse-tree walker.
 *
 * When this routine is the Walker.xExprCallback then expression trees
 * are walked without any actions being taken at each node.  Presumably,
 * when this routine is used for Walker.xExprCallback then
 * Walker.xSelectCallback is set to do something useful for every
 * subquery in the parser tree.
 */
int
sqlite3ExprWalkNoop(Walker * NotUsed, Expr * NotUsed2)
{
	UNUSED_PARAMETER2(NotUsed, NotUsed2);
	return WRC_Continue;
}

/*
 * This routine "expands" a SELECT statement and all of its subqueries.
 * For additional information on what it means to "expand" a SELECT
 * statement, see the comment on the selectExpand worker callback above.
 *
 * Expanding a SELECT statement is the first step in processing a
 * SELECT statement.  The SELECT statement must be expanded before
 * name resolution is performed.
 *
 * If anything goes wrong, an error message is written into pParse.
 * The calling function can detect the problem by looking at pParse->nErr
 * and/or pParse->db->mallocFailed.
 */
static void
sqlite3SelectExpand(Parse * pParse, Select * pSelect)
{
	Walker w;
	memset(&w, 0, sizeof(w));
	w.xExprCallback = sqlite3ExprWalkNoop;
	w.pParse = pParse;
	if (pParse->hasCompound) {
		w.xSelectCallback = convertCompoundSelectToSubquery;
		sqlite3WalkSelect(&w, pSelect);
	}
	w.xSelectCallback = selectExpander;
	if ((pSelect->selFlags & SF_MultiValue) == 0) {
		w.xSelectCallback2 = selectPopWith;
	}
	sqlite3WalkSelect(&w, pSelect);
}

/*
 * This is a Walker.xSelectCallback callback for the sqlite3SelectTypeInfo()
 * interface.
 *
 * For each FROM-clause subquery, add Column.zType and Column.zColl
 * information to the Table structure that represents the result set
 * of that subquery.
 *
 * The Table structure that represents the result set was constructed
 * by selectExpander() but the type and collation information was omitted
 * at that point because identifiers had not yet been resolved.  This
 * routine is called after identifier resolution.
 */
static void
selectAddSubqueryTypeInfo(Walker * pWalker, Select * p)
{
	Parse *pParse;
	int i;
	SrcList *pTabList;
	struct SrcList_item *pFrom;

	assert(p->selFlags & SF_Resolved);
	assert((p->selFlags & SF_HasTypeInfo) == 0);
	p->selFlags |= SF_HasTypeInfo;
	pParse = pWalker->pParse;
	pTabList = p->pSrc;
	for (i = 0, pFrom = pTabList->a; i < pTabList->nSrc; i++, pFrom++) {
		Table *pTab = pFrom->pTab;
		assert(pTab != 0);
		if (pTab->def->id == 0) {
			/* A sub-query in the FROM clause of a SELECT */
			Select *pSel = pFrom->pSelect;
			if (pSel) {
				while (pSel->pPrior)
					pSel = pSel->pPrior;
				sqlite3SelectAddColumnTypeAndCollation(pParse,
								       pTab,
								       pSel);
			}
		}
	}
}

/*
 * This routine adds datatype and collating sequence information to
 * the Table structures of all FROM-clause subqueries in a
 * SELECT statement.
 *
 * Use this routine after name resolution.
 */
static void
sqlite3SelectAddTypeInfo(Parse * pParse, Select * pSelect)
{
	Walker w;
	memset(&w, 0, sizeof(w));
	w.xSelectCallback2 = selectAddSubqueryTypeInfo;
	w.xExprCallback = sqlite3ExprWalkNoop;
	w.pParse = pParse;
	sqlite3WalkSelect(&w, pSelect);
}

/*
 * This routine sets up a SELECT statement for processing.  The
 * following is accomplished:
 *
 *     *  VDBE Cursor numbers are assigned to all FROM-clause terms.
 *     *  Ephemeral Table objects are created for all FROM-clause subqueries.
 *     *  ON and USING clauses are shifted into WHERE statements
 *     *  Wildcards "*" and "TABLE.*" in result sets are expanded.
 *     *  Identifiers in expression are matched to tables.
 *
 * This routine acts recursively on all subqueries within the SELECT.
 */
void
sqlite3SelectPrep(Parse * pParse,	/* The parser context */
		  Select * p,	/* The SELECT statement being coded. */
		  NameContext * pOuterNC	/* Name context for container */
    )
{
	sqlite3 *db;
	if (NEVER(p == 0))
		return;
	db = pParse->db;
	if (db->mallocFailed)
		return;
	if (p->selFlags & SF_HasTypeInfo)
		return;
	sqlite3SelectExpand(pParse, p);
	if (pParse->nErr || db->mallocFailed)
		return;
	sqlite3ResolveSelectNames(pParse, p, pOuterNC);
	if (pParse->nErr || db->mallocFailed)
		return;
	sqlite3SelectAddTypeInfo(pParse, p);
}

/*
 * Reset the aggregate accumulator.
 *
 * The aggregate accumulator is a set of memory cells that hold
 * intermediate results while calculating an aggregate.  This
 * routine generates code that stores NULLs in all of those memory
 * cells.
 */
static void
resetAccumulator(Parse * pParse, AggInfo * pAggInfo)
{
	Vdbe *v = pParse->pVdbe;
	int i;
	struct AggInfo_func *pFunc;
	int nReg = pAggInfo->nFunc + pAggInfo->nColumn;
	if (nReg == 0)
		return;
#ifdef SQLITE_DEBUG
	/* Verify that all AggInfo registers are within the range specified by
	 * AggInfo.mnReg..AggInfo.mxReg
	 */
	assert(nReg == pAggInfo->mxReg - pAggInfo->mnReg + 1);
	for (i = 0; i < pAggInfo->nColumn; i++) {
		assert(pAggInfo->aCol[i].iMem >= pAggInfo->mnReg
		       && pAggInfo->aCol[i].iMem <= pAggInfo->mxReg);
	}
	for (i = 0; i < pAggInfo->nFunc; i++) {
		assert(pAggInfo->aFunc[i].iMem >= pAggInfo->mnReg
		       && pAggInfo->aFunc[i].iMem <= pAggInfo->mxReg);
	}
#endif
	sqlite3VdbeAddOp3(v, OP_Null, 0, pAggInfo->mnReg, pAggInfo->mxReg);
	for (pFunc = pAggInfo->aFunc, i = 0; i < pAggInfo->nFunc; i++, pFunc++) {
		if (pFunc->iDistinct >= 0) {
			Expr *pE = pFunc->pExpr;
			assert(!ExprHasProperty(pE, EP_xIsSelect));
			if (pE->x.pList == 0 || pE->x.pList->nExpr != 1) {
				sqlite3ErrorMsg(pParse,
						"DISTINCT aggregates must have exactly one "
						"argument");
				pFunc->iDistinct = -1;
			} else {
				struct key_def *def =
					sql_expr_list_to_key_def(pParse,
								 pE->x.pList,
								 0);
				sqlite3VdbeAddOp4(v, OP_OpenTEphemeral,
						  pFunc->iDistinct, 1, 0,
						  (char *)def, P4_KEYDEF);
			}
		}
	}
}

/*
 * Invoke the OP_AggFinalize opcode for every aggregate function
 * in the AggInfo structure.
 */
static void
finalizeAggFunctions(Parse * pParse, AggInfo * pAggInfo)
{
	Vdbe *v = pParse->pVdbe;
	int i;
	struct AggInfo_func *pF;
	for (i = 0, pF = pAggInfo->aFunc; i < pAggInfo->nFunc; i++, pF++) {
		ExprList *pList = pF->pExpr->x.pList;
		assert(!ExprHasProperty(pF->pExpr, EP_xIsSelect));
		sqlite3VdbeAddOp2(v, OP_AggFinal, pF->iMem,
				  pList ? pList->nExpr : 0);
		sqlite3VdbeAppendP4(v, pF->pFunc, P4_FUNCDEF);
	}
}

/*
 * Update the accumulator memory cells for an aggregate based on
 * the current cursor position.
 */
static void
updateAccumulator(Parse * pParse, AggInfo * pAggInfo)
{
	Vdbe *v = pParse->pVdbe;
	int i;
	int regHit = 0;
	int addrHitTest = 0;
	struct AggInfo_func *pF;
	struct AggInfo_col *pC;

	pAggInfo->directMode = 1;
	for (i = 0, pF = pAggInfo->aFunc; i < pAggInfo->nFunc; i++, pF++) {
		int nArg;
		int addrNext = 0;
		int regAgg;
		ExprList *pList = pF->pExpr->x.pList;
		assert(!ExprHasProperty(pF->pExpr, EP_xIsSelect));
		if (pList) {
			nArg = pList->nExpr;
			regAgg = sqlite3GetTempRange(pParse, nArg);
			sqlite3ExprCodeExprList(pParse, pList, regAgg, 0,
						SQLITE_ECEL_DUP);
		} else {
			nArg = 0;
			regAgg = 0;
		}
		if (pF->iDistinct >= 0) {
			addrNext = sqlite3VdbeMakeLabel(v);
			testcase(nArg == 0);	/* Error condition */
			testcase(nArg > 1);	/* Also an error */
			codeDistinct(pParse, pF->iDistinct, addrNext, 1,
				     regAgg);
		}
		if (pF->pFunc->funcFlags & SQLITE_FUNC_NEEDCOLL) {
			struct coll *coll = NULL;
			struct ExprList_item *pItem;
			int j;
			assert(pList != 0);	/* pList!=0 if pF->pFunc has NEEDCOLL */
			bool is_found = false;
			uint32_t id;
			for (j = 0, pItem = pList->a; !is_found && j < nArg;
			     j++, pItem++) {
				coll = sql_expr_coll(pParse, pItem->pExpr,
						     &is_found, &id);
			}
			if (regHit == 0 && pAggInfo->nAccumulator)
				regHit = ++pParse->nMem;
			sqlite3VdbeAddOp4(v, OP_CollSeq, regHit, 0, 0,
					  (char *)coll, P4_COLLSEQ);
		}
		sqlite3VdbeAddOp3(v, OP_AggStep0, 0, regAgg, pF->iMem);
		sqlite3VdbeAppendP4(v, pF->pFunc, P4_FUNCDEF);
		sqlite3VdbeChangeP5(v, (u8) nArg);
		sqlite3ExprCacheAffinityChange(pParse, regAgg, nArg);
		sqlite3ReleaseTempRange(pParse, regAgg, nArg);
		if (addrNext) {
			sqlite3VdbeResolveLabel(v, addrNext);
			sqlite3ExprCacheClear(pParse);
		}
	}

	/* Before populating the accumulator registers, clear the column cache.
	 * Otherwise, if any of the required column values are already present
	 * in registers, sqlite3ExprCode() may use OP_SCopy to copy the value
	 * to pC->iMem. But by the time the value is used, the original register
	 * may have been used, invalidating the underlying buffer holding the
	 * text or blob value. See ticket [883034dcb5].
	 *
	 * Another solution would be to change the OP_SCopy used to copy cached
	 * values to an OP_Copy.
	 */
	if (regHit) {
		addrHitTest = sqlite3VdbeAddOp1(v, OP_If, regHit);
		VdbeCoverage(v);
	}
	sqlite3ExprCacheClear(pParse);
	for (i = 0, pC = pAggInfo->aCol; i < pAggInfo->nAccumulator; i++, pC++) {
		sqlite3ExprCode(pParse, pC->pExpr, pC->iMem);
	}
	pAggInfo->directMode = 0;
	sqlite3ExprCacheClear(pParse);
	if (addrHitTest) {
		sqlite3VdbeJumpHere(v, addrHitTest);
	}
}

/**
 * Add a single OP_Explain instruction to the VDBE to explain
 * a simple count(*) query ("SELECT count(*) FROM <tab>").
 * For memtx engine count is a simple operation,
 * which takes O(1) complexity.
 *
 * @param parse_context Current parsing context.
 * @param table_name Name of table being queried.
 */
static void
explain_simple_count(struct Parse *parse_context, const char *table_name)
{
	if (parse_context->explain == 2) {
		char *zEqp = sqlite3MPrintf(parse_context->db, "B+tree count %s",
					    table_name);
		sqlite3VdbeAddOp4(parse_context->pVdbe, OP_Explain,
				  parse_context->iSelectId, 0, 0, zEqp,
				  P4_DYNAMIC);
	}
}

/**
 * Generate VDBE code that HALT program when subselect returned
 * more than one row (determined as LIMIT 1 overflow).
 * @param parser Current parsing context.
 * @param limit_reg LIMIT register.
 * @param end_mark mark to jump if select returned distinct one
 *                 row as expected.
 */
static void
vdbe_code_raise_on_multiple_rows(struct Parse *parser, int limit_reg, int end_mark)
{
	assert(limit_reg != 0);
	struct Vdbe *v = sqlite3GetVdbe(parser);
	assert(v != NULL);

	int r1 = sqlite3GetTempReg(parser);
	sqlite3VdbeAddOp2(v, OP_Integer, 0, r1);
	sqlite3VdbeAddOp3(v, OP_Ne, r1, end_mark, limit_reg);
	const char *error =
		"SQL error: Expression subquery returned more than 1 row";
	sqlite3VdbeAddOp4(v, OP_Halt, SQL_TARANTOOL_ERROR, 0, 0, error,
			  P4_STATIC);
	sqlite3VdbeChangeP5(v, ER_SQL_EXECUTE);
	sqlite3ReleaseTempReg(parser, r1);
}

/*
 * Generate code for the SELECT statement given in the p argument.
 *
 * The results are returned according to the SelectDest structure.
 * See comments in sqliteInt.h for further information.
 *
 * This routine returns the number of errors.  If any errors are
 * encountered, then an appropriate error message is left in
 * pParse->zErrMsg.
 *
 * This routine does NOT free the Select structure passed in.  The
 * calling function needs to do that.
 */
int
sqlite3Select(Parse * pParse,		/* The parser context */
	      Select * p,		/* The SELECT statement being coded. */
	      SelectDest * pDest)	/* What to do with the query results */
{
	int i, j;		/* Loop counters */
	WhereInfo *pWInfo;	/* Return from sqlite3WhereBegin() */
	Vdbe *v;		/* The virtual machine under construction */
	int isAgg;		/* True for select lists like "count(*)" */
	ExprList *pEList = 0;	/* List of columns to extract. */
	SrcList *pTabList;	/* List of tables to select from */
	Expr *pWhere;		/* The WHERE clause.  May be NULL */
	ExprList *pGroupBy;	/* The GROUP BY clause.  May be NULL */
	Expr *pHaving;		/* The HAVING clause.  May be NULL */
	int rc = 1;		/* Value to return from this function */
	DistinctCtx sDistinct;	/* Info on how to code the DISTINCT keyword */
	SortCtx sSort;		/* Info on how to code the ORDER BY clause */
	AggInfo sAggInfo;	/* Information used by aggregate queries */
	int iEnd;		/* Address of the end of the query */
	sqlite3 *db;		/* The database connection */
	int iRestoreSelectId = pParse->iSelectId;
	pParse->iSelectId = pParse->iNextSelectId++;

	db = pParse->db;
	if (p == 0 || db->mallocFailed || pParse->nErr) {
		return 1;
	}
	memset(&sAggInfo, 0, sizeof(sAggInfo));
#ifdef SELECTTRACE_ENABLED
	pParse->nSelectIndent++;
	SELECTTRACE(1, pParse, p, ("begin processing:\n"));
	if (sqlite3SelectTrace & 0x100) {
		sqlite3TreeViewSelect(0, p, 0);
	}
#endif

	assert(p->pOrderBy == 0 || pDest->eDest != SRT_DistFifo);
	assert(p->pOrderBy == 0 || pDest->eDest != SRT_Fifo);
	assert(p->pOrderBy == 0 || pDest->eDest != SRT_DistQueue);
	assert(p->pOrderBy == 0 || pDest->eDest != SRT_Queue);
	if (IgnorableOrderby(pDest)) {
		assert(pDest->eDest == SRT_Exists || pDest->eDest == SRT_Union
		       || pDest->eDest == SRT_Except
		       || pDest->eDest == SRT_Discard
		       || pDest->eDest == SRT_Queue
		       || pDest->eDest == SRT_DistFifo
		       || pDest->eDest == SRT_DistQueue
		       || pDest->eDest == SRT_Fifo);
		/* If ORDER BY makes no difference in the output then neither does
		 * DISTINCT so it can be removed too.
		 */
		sql_expr_list_delete(db, p->pOrderBy);
		p->pOrderBy = 0;
		p->selFlags &= ~SF_Distinct;
	}
	sqlite3SelectPrep(pParse, p, 0);
	memset(&sSort, 0, sizeof(sSort));
	sSort.pOrderBy = p->pOrderBy;
	pTabList = p->pSrc;
	if (pParse->nErr || db->mallocFailed) {
		goto select_end;
	}
	assert(p->pEList != 0);
	isAgg = (p->selFlags & SF_Aggregate) != 0;
#ifdef SELECTTRACE_ENABLED
	if (sqlite3SelectTrace & 0x100) {
		SELECTTRACE(0x100, pParse, p, ("after name resolution:\n"));
		sqlite3TreeViewSelect(0, p, 0);
	}
#endif

	/* Try to flatten subqueries in the FROM clause up into the main query
	 */
	for (i = 0; !p->pPrior && i < pTabList->nSrc; i++) {
		struct SrcList_item *pItem = &pTabList->a[i];
		Select *pSub = pItem->pSelect;
		int isAggSub;
		Table *pTab = pItem->pTab;
		if (pSub == 0)
			continue;

		/* Catch mismatch in the declared columns of a view and the number of
		 * columns in the SELECT on the RHS
		 */
		if ((int)pTab->def->field_count != pSub->pEList->nExpr) {
			sqlite3ErrorMsg(pParse,
					"expected %d columns for '%s' but got %d",
					pTab->def->field_count, pTab->def->name,
					pSub->pEList->nExpr);
			goto select_end;
		}

		isAggSub = (pSub->selFlags & SF_Aggregate) != 0;
		if (flattenSubquery(pParse, p, i, isAgg, isAggSub)) {
			/* This subquery can be absorbed into its parent. */
			if (isAggSub) {
				isAgg = 1;
				p->selFlags |= SF_Aggregate;
			}
			i = -1;
		}
		pTabList = p->pSrc;
		if (db->mallocFailed)
			goto select_end;
		if (!IgnorableOrderby(pDest)) {
			sSort.pOrderBy = p->pOrderBy;
		}
	}

	/* Get a pointer the VDBE under construction, allocating a new VDBE if one
	 * does not already exist
	 */
	v = sqlite3GetVdbe(pParse);
	if (v == 0)
		goto select_end;

#ifndef SQLITE_OMIT_COMPOUND_SELECT
	/* Handle compound SELECT statements using the separate multiSelect()
	 * procedure.
	 */
	if (p->pPrior) {
		rc = multiSelect(pParse, p, pDest);
		pParse->iSelectId = iRestoreSelectId;

		int end = sqlite3VdbeMakeLabel(v);
		if ((p->selFlags & SF_SingleRow) != 0 && p->iLimit != 0) {
			vdbe_code_raise_on_multiple_rows(pParse, p->iLimit,
							 end);
		}
		sqlite3VdbeResolveLabel(v, end);

#ifdef SELECTTRACE_ENABLED
		SELECTTRACE(1, pParse, p, ("end compound-select processing\n"));
		pParse->nSelectIndent--;
#endif
		return rc;
	}
#endif

	/* Generate code for all sub-queries in the FROM clause
	 */
	for (i = 0; i < pTabList->nSrc; i++) {
		struct SrcList_item *pItem = &pTabList->a[i];
		SelectDest dest;
		Select *pSub = pItem->pSelect;
		if (pSub == 0)
			continue;

		/* Sometimes the code for a subquery will be generated more than
		 * once, if the subquery is part of the WHERE clause in a LEFT JOIN,
		 * for example.  In that case, do not regenerate the code to manifest
		 * a view or the co-routine to implement a view.  The first instance
		 * is sufficient, though the subroutine to manifest the view does need
		 * to be invoked again.
		 */
		if (pItem->addrFillSub) {
			if (pItem->fg.viaCoroutine == 0) {
				sqlite3VdbeAddOp2(v, OP_Gosub, pItem->regReturn,
						  pItem->addrFillSub);
			}
			continue;
		}

		/* Increment Parse.nHeight by the height of the largest expression
		 * tree referred to by this, the parent select. The child select
		 * may contain expression trees of at most
		 * (SQLITE_MAX_EXPR_DEPTH-Parse.nHeight) height. This is a bit
		 * more conservative than necessary, but much easier than enforcing
		 * an exact limit.
		 */
		pParse->nHeight += sqlite3SelectExprHeight(p);

		/* Make copies of constant WHERE-clause terms in the outer query down
		 * inside the subquery.  This can help the subquery to run more efficiently.
		 */
		if ((pItem->fg.jointype & JT_OUTER) == 0
		    && pushDownWhereTerms(pParse, pSub, p->pWhere,
					  pItem->iCursor)
		    ) {
#ifdef SELECTTRACE_ENABLED
			if (sqlite3SelectTrace & 0x100) {
				SELECTTRACE(0x100, pParse, p,
					    ("After WHERE-clause push-down:\n"));
				sqlite3TreeViewSelect(0, p, 0);
			}
#endif
		}

		/* Generate code to implement the subquery
		 *
		 * The subquery is implemented as a co-routine if all of these are true:
		 *   (1)  The subquery is guaranteed to be the outer loop (so that it
		 *        does not need to be computed more than once)
		 *   (2)  The ALL keyword after SELECT is omitted.  (Applications are
		 *        allowed to say "SELECT ALL" instead of just "SELECT" to disable
		 *        the use of co-routines.)
		 *   (3)  Co-routines are not disabled using sqlite3_test_control()
		 *        with SQLITE_TESTCTRL_OPTIMIZATIONS.
		 *
		 * TODO: Are there other reasons beside (1) to use a co-routine
		 * implementation?
		 */
		if (i == 0 && (pTabList->nSrc == 1 || (pTabList->a[1].fg.jointype & (JT_LEFT | JT_CROSS)) != 0)	/* (1) */
		    &&(p->selFlags & SF_All) == 0	/* (2) */
		    && OptimizationEnabled(db, SQLITE_SubqCoroutine)	/* (3) */
		    ) {
			/* Implement a co-routine that will return a single row of the result
			 * set on each invocation.
			 */
			int addrTop = sqlite3VdbeCurrentAddr(v) + 1;
			pItem->regReturn = ++pParse->nMem;
			sqlite3VdbeAddOp3(v, OP_InitCoroutine, pItem->regReturn,
					  0, addrTop);
			VdbeComment((v, "%s", pItem->pTab->def->name));
			pItem->addrFillSub = addrTop;
			sqlite3SelectDestInit(&dest, SRT_Coroutine,
					      pItem->regReturn);
			pItem->iSelectId = pParse->iNextSelectId;
			sqlite3Select(pParse, pSub, &dest);
			pItem->pTab->tuple_log_count = pSub->nSelectRow;
			pItem->fg.viaCoroutine = 1;
			pItem->regResult = dest.iSdst;
			sqlite3VdbeEndCoroutine(v, pItem->regReturn);
			sqlite3VdbeJumpHere(v, addrTop - 1);
			sqlite3ClearTempRegCache(pParse);
		} else {
			/* Generate a subroutine that will fill an ephemeral table with
			 * the content of this subquery.  pItem->addrFillSub will point
			 * to the address of the generated subroutine.  pItem->regReturn
			 * is a register allocated to hold the subroutine return address
			 */
			int topAddr;
			int onceAddr = 0;
			int retAddr;
			assert(pItem->addrFillSub == 0);
			pItem->regReturn = ++pParse->nMem;
			topAddr =
			    sqlite3VdbeAddOp2(v, OP_Integer, 0,
					      pItem->regReturn);
			pItem->addrFillSub = topAddr + 1;
			if (pItem->fg.isCorrelated == 0) {
				/* If the subquery is not correlated and if we are not inside of
				 * a trigger, then we only need to compute the value of the subquery
				 * once.
				 */
				onceAddr = sqlite3VdbeAddOp0(v, OP_Once);
				VdbeCoverage(v);
				VdbeComment((v, "materialize \"%s\"",
					     pItem->pTab->def->name));
			} else {
				VdbeNoopComment((v, "materialize \"%s\"",
						 pItem->pTab->def->name));
			}
			sqlite3SelectDestInit(&dest, SRT_EphemTab,
					      pItem->iCursor);
			pItem->iSelectId = pParse->iNextSelectId;
			sqlite3Select(pParse, pSub, &dest);
			pItem->pTab->tuple_log_count = pSub->nSelectRow;
			if (onceAddr)
				sqlite3VdbeJumpHere(v, onceAddr);
			retAddr =
			    sqlite3VdbeAddOp1(v, OP_Return, pItem->regReturn);
			VdbeComment((v, "end %s", pItem->pTab->def->name));
			sqlite3VdbeChangeP1(v, topAddr, retAddr);
			sqlite3ClearTempRegCache(pParse);
		}
		if (db->mallocFailed)
			goto select_end;
		pParse->nHeight -= sqlite3SelectExprHeight(p);
	}

	/* Various elements of the SELECT copied into local variables for
	 * convenience
	 */
	pEList = p->pEList;
	pWhere = p->pWhere;
	pGroupBy = p->pGroupBy;
	pHaving = p->pHaving;
	sDistinct.isTnct = (p->selFlags & SF_Distinct) != 0;

#ifdef SELECTTRACE_ENABLED
	if (sqlite3SelectTrace & 0x400) {
		SELECTTRACE(0x400, pParse, p,
			    ("After all FROM-clause analysis:\n"));
		sqlite3TreeViewSelect(0, p, 0);
	}
#endif

	/* If the query is DISTINCT with an ORDER BY but is not an aggregate, and
	 * if the select-list is the same as the ORDER BY list, then this query
	 * can be rewritten as a GROUP BY. In other words, this:
	 *
	 *     SELECT DISTINCT xyz FROM ... ORDER BY xyz
	 *
	 * is transformed to:
	 *
	 *     SELECT xyz FROM ... GROUP BY xyz ORDER BY xyz
	 *
	 * The second form is preferred as a single index (or temp-table) may be
	 * used for both the ORDER BY and DISTINCT processing. As originally
	 * written the query must use a temp-table for at least one of the ORDER
	 * BY and DISTINCT, and an index or separate temp-table for the other.
	 */
	if ((p->selFlags & (SF_Distinct | SF_Aggregate)) == SF_Distinct
	    && sqlite3ExprListCompare(sSort.pOrderBy, pEList, -1) == 0) {
		p->selFlags &= ~SF_Distinct;
		pGroupBy = p->pGroupBy = sql_expr_list_dup(db, pEList, 0);
		/* Notice that even thought SF_Distinct has been cleared from p->selFlags,
		 * the sDistinct.isTnct is still set.  Hence, isTnct represents the
		 * original setting of the SF_Distinct flag, not the current setting
		 */
		assert(sDistinct.isTnct);

#ifdef SELECTTRACE_ENABLED
		if (sqlite3SelectTrace & 0x400) {
			SELECTTRACE(0x400, pParse, p,
				    ("Transform DISTINCT into GROUP BY:\n"));
			sqlite3TreeViewSelect(0, p, 0);
		}
#endif
	}

	/* If there is an ORDER BY clause, then create an ephemeral index to
	 * do the sorting.  But this sorting ephemeral index might end up
	 * being unused if the data can be extracted in pre-sorted order.
	 * If that is the case, then the OP_OpenEphemeral instruction will be
	 * changed to an OP_Noop once we figure out that the sorting index is
	 * not needed.  The sSort.addrSortIndex variable is used to facilitate
	 * that change.
	 */
	if (sSort.pOrderBy) {
		struct key_def *def =
			sql_expr_list_to_key_def(pParse, sSort.pOrderBy, 0);
		sSort.iECursor = pParse->nTab++;
		/* Number of columns in transient table equals to number of columns in
		 * SELECT statement plus number of columns in ORDER BY statement
		 * and plus one column for ID.
		 */
		int nCols = pEList->nExpr + sSort.pOrderBy->nExpr + 1;
		if (def->parts[0].sort_order == SORT_ORDER_DESC) {
			sSort.sortFlags |= SORTFLAG_DESC;
		}
		sSort.addrSortIndex =
		    sqlite3VdbeAddOp4(v, OP_OpenTEphemeral,
				      sSort.iECursor,
				      nCols,
				      0, (char *)def, P4_KEYDEF);
		VdbeComment((v, "Sort table"));
	} else {
		sSort.addrSortIndex = -1;
	}

	/* If the output is destined for a temporary table, open that table.
	 */
	if (pDest->eDest == SRT_EphemTab) {
		sqlite3VdbeAddOp2(v, OP_OpenTEphemeral, pDest->iSDParm,
				  pEList->nExpr + 1);

		VdbeComment((v, "Output table"));
	}

	/* Set the limiter.
	 */
	iEnd = sqlite3VdbeMakeLabel(v);
	if ((p->selFlags & SF_FixedLimit) == 0) {
		p->nSelectRow = 320;	/* 4 billion rows */
	}
	computeLimitRegisters(pParse, p, iEnd);
	if (p->iLimit == 0 && sSort.addrSortIndex >= 0) {
		sqlite3VdbeChangeOpcode(v, sSort.addrSortIndex, OP_SorterOpen);
		sSort.sortFlags |= SORTFLAG_UseSorter;
	}

	/* Open an ephemeral index to use for the distinct set.
	 */
	if (p->selFlags & SF_Distinct) {
		sDistinct.tabTnct = pParse->nTab++;
		struct key_def *def = sql_expr_list_to_key_def(pParse, p->pEList, 0);
		sDistinct.addrTnct = sqlite3VdbeAddOp4(v, OP_OpenTEphemeral,
						       sDistinct.tabTnct,
						       def->part_count,
						       0, (char *)def,
						       P4_KEYDEF);
		VdbeComment((v, "Distinct table"));
		sDistinct.eTnctType = WHERE_DISTINCT_UNORDERED;
	} else {
		sDistinct.eTnctType = WHERE_DISTINCT_NOOP;
	}

	if (!isAgg && pGroupBy == 0) {
		/* No aggregate functions and no GROUP BY clause */
		u16 wctrlFlags = (sDistinct.isTnct ? WHERE_WANT_DISTINCT : 0);
		assert(WHERE_USE_LIMIT == SF_FixedLimit);
		wctrlFlags |= p->selFlags & SF_FixedLimit;

		/* Begin the database scan. */
		pWInfo =
		    sqlite3WhereBegin(pParse, pTabList, pWhere, sSort.pOrderBy,
				      p->pEList, wctrlFlags, p->nSelectRow);
		if (pWInfo == 0)
			goto select_end;
		if (sqlite3WhereOutputRowCount(pWInfo) < p->nSelectRow) {
			p->nSelectRow = sqlite3WhereOutputRowCount(pWInfo);
		}
		if (sDistinct.isTnct && sqlite3WhereIsDistinct(pWInfo)) {
			sDistinct.eTnctType = sqlite3WhereIsDistinct(pWInfo);
		}
		if (sSort.pOrderBy) {
			sSort.nOBSat = sqlite3WhereIsOrdered(pWInfo);
			sSort.bOrderedInnerLoop =
			    sqlite3WhereOrderedInnerLoop(pWInfo);
			if (sSort.nOBSat == sSort.pOrderBy->nExpr) {
				sSort.pOrderBy = 0;
			}
		}

		/* If sorting index that was created by a prior OP_OpenEphemeral
		 * instruction ended up not being needed, then change the OP_OpenEphemeral
		 * into an OP_Noop.
		 */
		if (sSort.addrSortIndex >= 0 && sSort.pOrderBy == 0) {
			sqlite3VdbeChangeToNoop(v, sSort.addrSortIndex);
		}

		/* Use the standard inner loop. */
		selectInnerLoop(pParse, p, pEList, -1, &sSort, &sDistinct,
				pDest, sqlite3WhereContinueLabel(pWInfo),
				sqlite3WhereBreakLabel(pWInfo));

		/* End the database scan loop.
		 */
		sqlite3WhereEnd(pWInfo);
	} else {
		/* This case when there exist aggregate functions or a GROUP BY clause
		 * or both
		 */
		NameContext sNC;	/* Name context for processing aggregate information */
		int iAMem;	/* First Mem address for storing current GROUP BY */
		int iBMem;	/* First Mem address for previous GROUP BY */
		int iUseFlag;	/* Mem address holding flag indicating that at least
				 * one row of the input to the aggregator has been
				 * processed
				 */
		int iAbortFlag;	/* Mem address which causes query abort if positive */
		int groupBySort;	/* Rows come from source in GROUP BY order */
		int addrEnd;	/* End of processing for this SELECT */
		int sortPTab = 0;	/* Pseudotable used to decode sorting results */
		int sortOut = 0;	/* Output register from the sorter */
		int orderByGrp = 0;	/* True if the GROUP BY and ORDER BY are the same */

		/* Remove any and all aliases between the result set and the
		 * GROUP BY clause.
		 */
		if (pGroupBy) {
			int k;	/* Loop counter */
			struct ExprList_item *pItem;	/* For looping over expression in a list */

			for (k = p->pEList->nExpr, pItem = p->pEList->a; k > 0;
			     k--, pItem++) {
				pItem->u.x.iAlias = 0;
			}
			for (k = pGroupBy->nExpr, pItem = pGroupBy->a; k > 0;
			     k--, pItem++) {
				pItem->u.x.iAlias = 0;
			}
			assert(66 == sqlite3LogEst(100));
			if (p->nSelectRow > 66)
				p->nSelectRow = 66;
		} else {
			assert(0 == sqlite3LogEst(1));
			p->nSelectRow = 0;
		}

		/* If there is both a GROUP BY and an ORDER BY clause and they are
		 * identical, then it may be possible to disable the ORDER BY clause
		 * on the grounds that the GROUP BY will cause elements to come out
		 * in the correct order. It also may not - the GROUP BY might use a
		 * database index that causes rows to be grouped together as required
		 * but not actually sorted. Either way, record the fact that the
		 * ORDER BY and GROUP BY clauses are the same by setting the orderByGrp
		 * variable.
		 */
		if (sqlite3ExprListCompare(pGroupBy, sSort.pOrderBy, -1) == 0) {
			orderByGrp = 1;
		}

		/* Create a label to jump to when we want to abort the query */
		addrEnd = sqlite3VdbeMakeLabel(v);

		/* Convert TK_COLUMN nodes into TK_AGG_COLUMN and make entries in
		 * sAggInfo for all TK_AGG_FUNCTION nodes in expressions of the
		 * SELECT statement.
		 */
		memset(&sNC, 0, sizeof(sNC));
		sNC.pParse = pParse;
		sNC.pSrcList = pTabList;
		sNC.pAggInfo = &sAggInfo;
		sAggInfo.mnReg = pParse->nMem + 1;
		sAggInfo.nSortingColumn = pGroupBy ? pGroupBy->nExpr : 0;
		sAggInfo.pGroupBy = pGroupBy;
		sqlite3ExprAnalyzeAggList(&sNC, pEList);
		sqlite3ExprAnalyzeAggList(&sNC, sSort.pOrderBy);
		if (pHaving) {
			sqlite3ExprAnalyzeAggregates(&sNC, pHaving);
		}
		sAggInfo.nAccumulator = sAggInfo.nColumn;
		for (i = 0; i < sAggInfo.nFunc; i++) {
			assert(!ExprHasProperty
			       (sAggInfo.aFunc[i].pExpr, EP_xIsSelect));
			sNC.ncFlags |= NC_InAggFunc;
			sqlite3ExprAnalyzeAggList(&sNC,
						  sAggInfo.aFunc[i].pExpr->x.
						  pList);
			sNC.ncFlags &= ~NC_InAggFunc;
		}
		sAggInfo.mxReg = pParse->nMem;
		if (db->mallocFailed)
			goto select_end;

		/* Processing for aggregates with GROUP BY is very different and
		 * much more complex than aggregates without a GROUP BY.
		 */
		if (pGroupBy) {
			int addr1;	/* A-vs-B comparision jump */
			int addrOutputRow;	/* Start of subroutine that outputs a result row */
			int regOutputRow;	/* Return address register for output subroutine */
			int addrSetAbort;	/* Set the abort flag and return */
			int addrTopOfLoop;	/* Top of the input loop */
			int addrSortingIdx;	/* The OP_OpenEphemeral for the sorting index */
			int addrReset;	/* Subroutine for resetting the accumulator */
			int regReset;	/* Return address register for reset subroutine */

			/* If there is a GROUP BY clause we might need a sorting index to
			 * implement it.  Allocate that sorting index now.  If it turns out
			 * that we do not need it after all, the OP_SorterOpen instruction
			 * will be converted into a Noop.
			 */
			sAggInfo.sortingIdx = pParse->nTab++;
			struct key_def *def =
				sql_expr_list_to_key_def(pParse, pGroupBy, 0);
			addrSortingIdx =
			    sqlite3VdbeAddOp4(v, OP_SorterOpen,
					      sAggInfo.sortingIdx,
					      sAggInfo.nSortingColumn, 0,
					      (char *)def, P4_KEYDEF);

			/* Initialize memory locations used by GROUP BY aggregate processing
			 */
			iUseFlag = ++pParse->nMem;
			iAbortFlag = ++pParse->nMem;
			regOutputRow = ++pParse->nMem;
			addrOutputRow = sqlite3VdbeMakeLabel(v);
			regReset = ++pParse->nMem;
			addrReset = sqlite3VdbeMakeLabel(v);
			iAMem = pParse->nMem + 1;
			pParse->nMem += pGroupBy->nExpr;
			iBMem = pParse->nMem + 1;
			pParse->nMem += pGroupBy->nExpr;
			sqlite3VdbeAddOp2(v, OP_Integer, 0, iAbortFlag);
			VdbeComment((v, "clear abort flag"));
			sqlite3VdbeAddOp2(v, OP_Integer, 0, iUseFlag);
			VdbeComment((v, "indicate accumulator empty"));
			sqlite3VdbeAddOp3(v, OP_Null, 0, iAMem,
					  iAMem + pGroupBy->nExpr - 1);

			/* Begin a loop that will extract all source rows in GROUP BY order.
			 * This might involve two separate loops with an OP_Sort in between, or
			 * it might be a single loop that uses an index to extract information
			 * in the right order to begin with.
			 */
			sqlite3VdbeAddOp2(v, OP_Gosub, regReset, addrReset);
			pWInfo =
			    sqlite3WhereBegin(pParse, pTabList, pWhere,
					      pGroupBy, 0,
					      WHERE_GROUPBY | (orderByGrp ?
							       WHERE_SORTBYGROUP
							       : 0), 0);
			if (pWInfo == 0)
				goto select_end;
			if (sqlite3WhereIsOrdered(pWInfo) == pGroupBy->nExpr) {
				/* The optimizer is able to deliver rows in group by order so
				 * we do not have to sort.  The OP_OpenEphemeral table will be
				 * cancelled later because we still need to use the key_def
				 */
				groupBySort = 0;
			} else {
				/* Rows are coming out in undetermined order.  We have to push
				 * each row into a sorting index, terminate the first loop,
				 * then loop over the sorting index in order to get the output
				 * in sorted order
				 */
				int regBase;
				int regRecord;
				int nCol;
				int nGroupBy;

				explainTempTable(pParse,
						 (sDistinct.isTnct
						  && (p->
						      selFlags & SF_Distinct) ==
						  0) ? "DISTINCT" : "GROUP BY");

				groupBySort = 1;
				nGroupBy = pGroupBy->nExpr;
				nCol = nGroupBy;
				j = nGroupBy;
				for (i = 0; i < sAggInfo.nColumn; i++) {
					if (sAggInfo.aCol[i].iSorterColumn >= j) {
						nCol++;
						j++;
					}
				}
				regBase = sqlite3GetTempRange(pParse, nCol);
				sqlite3ExprCacheClear(pParse);
				sqlite3ExprCodeExprList(pParse, pGroupBy,
							regBase, 0, 0);
				j = nGroupBy;
				for (i = 0; i < sAggInfo.nColumn; i++) {
					struct AggInfo_col *pCol =
					    &sAggInfo.aCol[i];
					if (pCol->iSorterColumn >= j) {
						int r1 = j + regBase;
						sqlite3ExprCodeGetColumnToReg
						    (pParse, pCol->space_def,
						     pCol->iColumn,
						     pCol->iTable, r1);
						j++;
					}
				}
				regRecord = sqlite3GetTempReg(pParse);
				sqlite3VdbeAddOp3(v, OP_MakeRecord, regBase,
						  nCol, regRecord);
				sqlite3VdbeAddOp2(v, OP_SorterInsert,
						  sAggInfo.sortingIdx,
						  regRecord);
				sqlite3ReleaseTempReg(pParse, regRecord);
				sqlite3ReleaseTempRange(pParse, regBase, nCol);
				sqlite3WhereEnd(pWInfo);
				sAggInfo.sortingIdxPTab = sortPTab =
				    pParse->nTab++;
				sortOut = sqlite3GetTempReg(pParse);
				sqlite3VdbeAddOp3(v, OP_OpenPseudo, sortPTab,
						  sortOut, nCol);
				sqlite3VdbeAddOp2(v, OP_SorterSort,
						  sAggInfo.sortingIdx, addrEnd);
				VdbeComment((v, "GROUP BY sort"));
				VdbeCoverage(v);
				sAggInfo.useSortingIdx = 1;
				sqlite3ExprCacheClear(pParse);

			}

			/* If the index or temporary table used by the GROUP BY sort
			 * will naturally deliver rows in the order required by the ORDER BY
			 * clause, cancel the ephemeral table open coded earlier.
			 *
			 * This is an optimization - the correct answer should result regardless.
			 * Use the SQLITE_GroupByOrder flag with SQLITE_TESTCTRL_OPTIMIZER to
			 * disable this optimization for testing purposes.
			 */
			if (orderByGrp
			    && OptimizationEnabled(db, SQLITE_GroupByOrder)
			    && (groupBySort || sqlite3WhereIsSorted(pWInfo))
			    ) {
				sSort.pOrderBy = 0;
				sqlite3VdbeChangeToNoop(v, sSort.addrSortIndex);
			}

			/* Evaluate the current GROUP BY terms and store in b0, b1, b2...
			 * (b0 is memory location iBMem+0, b1 is iBMem+1, and so forth)
			 * Then compare the current GROUP BY terms against the GROUP BY terms
			 * from the previous row currently stored in a0, a1, a2...
			 */
			addrTopOfLoop = sqlite3VdbeCurrentAddr(v);
			sqlite3ExprCacheClear(pParse);
			if (groupBySort) {
				sqlite3VdbeAddOp3(v, OP_SorterData,
						  sAggInfo.sortingIdx, sortOut,
						  sortPTab);
			}
			for (j = 0; j < pGroupBy->nExpr; j++) {
				if (groupBySort) {
					sqlite3VdbeAddOp3(v, OP_Column,
							  sortPTab, j,
							  iBMem + j);
				} else {
					sAggInfo.directMode = 1;
					sqlite3ExprCode(pParse,
							pGroupBy->a[j].pExpr,
							iBMem + j);
				}
			}
			struct key_def *dup_def = key_def_dup(def);
			if (dup_def == NULL) {
				sqlite3OomFault(db);
				goto select_end;
			}
			sqlite3VdbeAddOp4(v, OP_Compare, iAMem, iBMem,
					  pGroupBy->nExpr, (char*)dup_def,
					  P4_KEYDEF);
			addr1 = sqlite3VdbeCurrentAddr(v);
			sqlite3VdbeAddOp3(v, OP_Jump, addr1 + 1, 0, addr1 + 1);
			VdbeCoverage(v);

			/* Generate code that runs whenever the GROUP BY changes.
			 * Changes in the GROUP BY are detected by the previous code
			 * block.  If there were no changes, this block is skipped.
			 *
			 * This code copies current group by terms in b0,b1,b2,...
			 * over to a0,a1,a2.  It then calls the output subroutine
			 * and resets the aggregate accumulator registers in preparation
			 * for the next GROUP BY batch.
			 */
			sqlite3ExprCodeMove(pParse, iBMem, iAMem,
					    pGroupBy->nExpr);
			sqlite3VdbeAddOp2(v, OP_Gosub, regOutputRow,
					  addrOutputRow);
			VdbeComment((v, "output one row"));
			sqlite3VdbeAddOp2(v, OP_IfPos, iAbortFlag, addrEnd);
			VdbeCoverage(v);
			VdbeComment((v, "check abort flag"));
			sqlite3VdbeAddOp2(v, OP_Gosub, regReset, addrReset);
			VdbeComment((v, "reset accumulator"));

			/* Update the aggregate accumulators based on the content of
			 * the current row
			 */
			sqlite3VdbeJumpHere(v, addr1);
			updateAccumulator(pParse, &sAggInfo);
			sqlite3VdbeAddOp2(v, OP_Integer, 1, iUseFlag);
			VdbeComment((v, "indicate data in accumulator"));

			/* End of the loop
			 */
			if (groupBySort) {
				sqlite3VdbeAddOp2(v, OP_SorterNext,
						  sAggInfo.sortingIdx,
						  addrTopOfLoop);
				VdbeCoverage(v);
			} else {
				sqlite3WhereEnd(pWInfo);
				sqlite3VdbeChangeToNoop(v, addrSortingIdx);
			}

			/* Output the final row of result
			 */
			sqlite3VdbeAddOp2(v, OP_Gosub, regOutputRow,
					  addrOutputRow);
			VdbeComment((v, "output final row"));

			/* Jump over the subroutines
			 */
			sqlite3VdbeGoto(v, addrEnd);

			/* Generate a subroutine that outputs a single row of the result
			 * set.  This subroutine first looks at the iUseFlag.  If iUseFlag
			 * is less than or equal to zero, the subroutine is a no-op.  If
			 * the processing calls for the query to abort, this subroutine
			 * increments the iAbortFlag memory location before returning in
			 * order to signal the caller to abort.
			 */
			addrSetAbort = sqlite3VdbeCurrentAddr(v);
			sqlite3VdbeAddOp2(v, OP_Integer, 1, iAbortFlag);
			VdbeComment((v, "set abort flag"));
			sqlite3VdbeAddOp1(v, OP_Return, regOutputRow);
			sqlite3VdbeResolveLabel(v, addrOutputRow);
			addrOutputRow = sqlite3VdbeCurrentAddr(v);
			sqlite3VdbeAddOp2(v, OP_IfPos, iUseFlag,
					  addrOutputRow + 2);
			VdbeCoverage(v);
			VdbeComment((v,
				     "Groupby result generator entry point"));
			sqlite3VdbeAddOp1(v, OP_Return, regOutputRow);
			finalizeAggFunctions(pParse, &sAggInfo);
			sqlite3ExprIfFalse(pParse, pHaving, addrOutputRow + 1,
					   SQLITE_JUMPIFNULL);
			selectInnerLoop(pParse, p, p->pEList, -1, &sSort,
					&sDistinct, pDest, addrOutputRow + 1,
					addrSetAbort);
			sqlite3VdbeAddOp1(v, OP_Return, regOutputRow);
			VdbeComment((v, "end groupby result generator"));

			/* Generate a subroutine that will reset the group-by accumulator
			 */
			sqlite3VdbeResolveLabel(v, addrReset);
			resetAccumulator(pParse, &sAggInfo);
			sqlite3VdbeAddOp1(v, OP_Return, regReset);

		} /* endif pGroupBy.  Begin aggregate queries without GROUP BY: */
		else {
			struct space *space = is_simple_count(p, &sAggInfo);
			if (space != NULL) {
				/*
				 * If is_simple_count() returns a pointer to
				 * space, then the SQL statement is of the form:
				 *
				 *   SELECT count(*) FROM <tbl>
				 *
				 * This statement is so common that it is
				 * optimized specially. The OP_Count instruction
				 * is executed on the primary key index,
				 * since there is no difference which index
				 * to choose.
				 */
				const int cursor = pParse->nTab++;
				/*
				 * Open the cursor, execute the OP_Count,
				 * close the cursor.
				 */
				vdbe_emit_open_cursor(pParse, cursor, 0, space);
				sqlite3VdbeAddOp2(v, OP_Count, cursor,
						  sAggInfo.aFunc[0].iMem);
				sqlite3VdbeAddOp1(v, OP_Close, cursor);
				explain_simple_count(pParse, space->def->name);
			} else
			{
				/* Check if the query is of one of the following forms:
				 *
				 *   SELECT min(x) FROM ...
				 *   SELECT max(x) FROM ...
				 *
				 * If it is, then ask the code in where.c to attempt to sort results
				 * as if there was an "ORDER ON x" or "ORDER ON x DESC" clause.
				 * If where.c is able to produce results sorted in this order, then
				 * add vdbe code to break out of the processing loop after the
				 * first iteration (since the first iteration of the loop is
				 * guaranteed to operate on the row with the minimum or maximum
				 * value of x, the only row required).
				 *
				 * A special flag must be passed to sqlite3WhereBegin() to slightly
				 * modify behavior as follows:
				 *
				 *   + If the query is a "SELECT min(x)", then the loop coded by
				 *     where.c should not iterate over any values with a NULL value
				 *     for x.
				 *
				 *   + The optimizer code in where.c (the thing that decides which
				 *     index or indices to use) should place a different priority on
				 *     satisfying the 'ORDER BY' clause than it does in other cases.
				 *     Refer to code and comments in where.c for details.
				 */
				ExprList *pMinMax = 0;
				u8 flag = WHERE_ORDERBY_NORMAL;
				ExprList *pDel = 0;

				assert(p->pGroupBy == 0);
				assert(flag == 0);
				if (p->pHaving == 0) {
					flag = minMaxQuery(&sAggInfo, &pMinMax);
				}
				assert(flag == 0
				       || (pMinMax != 0
					   && pMinMax->nExpr == 1));

				if (flag) {
					pMinMax =
					    sql_expr_list_dup(db, pMinMax, 0);
					pDel = pMinMax;
					assert(db->mallocFailed
					       || pMinMax != 0);
					if (!db->mallocFailed) {
						pMinMax->a[0].sort_order =
						    flag !=
						    WHERE_ORDERBY_MIN ? 1 : 0;
						pMinMax->a[0].pExpr->op =
						    TK_COLUMN;
					}
				}

				/* This case runs if the aggregate has no GROUP BY clause.  The
				 * processing is much simpler since there is only a single row
				 * of output.
				 */
				resetAccumulator(pParse, &sAggInfo);
				pWInfo =
				    sqlite3WhereBegin(pParse, pTabList, pWhere,
						      pMinMax, 0, flag, 0);
				if (pWInfo == 0) {
					sql_expr_list_delete(db, pDel);
					goto select_end;
				}
				updateAccumulator(pParse, &sAggInfo);
				assert(pMinMax == 0 || pMinMax->nExpr == 1);
				if (sqlite3WhereIsOrdered(pWInfo) > 0) {
					sqlite3VdbeGoto(v,
							sqlite3WhereBreakLabel
							(pWInfo));
					VdbeComment((v, "%s() by index",
						     (flag ==
						      WHERE_ORDERBY_MIN ? "min"
						      : "max")));
				}
				sqlite3WhereEnd(pWInfo);
				finalizeAggFunctions(pParse, &sAggInfo);
				sql_expr_list_delete(db, pDel);
			}

			sSort.pOrderBy = 0;
			sqlite3ExprIfFalse(pParse, pHaving, addrEnd,
					   SQLITE_JUMPIFNULL);
			selectInnerLoop(pParse, p, p->pEList, -1, 0, 0, pDest,
					addrEnd, addrEnd);
		}
		sqlite3VdbeResolveLabel(v, addrEnd);

	}			/* endif aggregate query */

	if (sDistinct.eTnctType == WHERE_DISTINCT_UNORDERED) {
		explainTempTable(pParse, "DISTINCT");
	}

	/* If there is an ORDER BY clause, then we need to sort the results
	 * and send them to the callback one by one.
	 */
	if (sSort.pOrderBy) {
		explainTempTable(pParse,
				 sSort.nOBSat >
				 0 ? "RIGHT PART OF ORDER BY" : "ORDER BY");
		generateSortTail(pParse, p, &sSort, pEList->nExpr, pDest);
	}

	/* Generate code that prevent returning multiple rows. */
	if ((p->selFlags & SF_SingleRow) != 0 && p->iLimit != 0)
		vdbe_code_raise_on_multiple_rows(pParse, p->iLimit, iEnd);
	/* Jump here to skip this query. */
	sqlite3VdbeResolveLabel(v, iEnd);

	/* The SELECT has been coded. If there is an error in the Parse structure,
	 * set the return code to 1. Otherwise 0.
	 */
	rc = (pParse->nErr > 0);

	/* Control jumps to here if an error is encountered above, or upon
	 * successful coding of the SELECT.
	 */
 select_end:
	pParse->iSelectId = iRestoreSelectId;

	/* Identify column names if results of the SELECT are to be output.
	 */
	if (rc == SQLITE_OK && pDest->eDest == SRT_Output) {
		generateColumnNames(pParse, pTabList, pEList);
	}

	sqlite3DbFree(db, sAggInfo.aCol);
	sqlite3DbFree(db, sAggInfo.aFunc);
#ifdef SELECTTRACE_ENABLED
	SELECTTRACE(1, pParse, p, ("end processing\n"));
	pParse->nSelectIndent--;
#endif
	return rc;
}

void
sql_expr_extract_select(struct Parse *parser, struct Select *select)
{
	struct ExprList *expr_list = select->pEList;
	assert(expr_list->nExpr == 1);
	parser->parsed_ast_type = AST_TYPE_EXPR;
	parser->parsed_ast.expr = sqlite3ExprDup(parser->db,
						 expr_list->a->pExpr,
						 EXPRDUP_REDUCE);
}