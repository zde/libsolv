/*
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * solverdebug.h
 *
 */

#ifndef SATSOLVER_SOLVERDEBUG_H
#define SATSOLVER_SOLVERDEBUG_H

#include "pooltypes.h"
#include "pool.h"
#include "solver.h"

extern Id *solver_create_decisions_obsoletesmap(Solver *solv);
extern void solver_printruleelement(Solver *solv, int type, Rule *r, Id v);
extern void solver_printrule(Solver *solv, int type, Rule *r);
extern void solver_printruleclass(Solver *solv, int type, Rule *r);
extern void solver_printproblem(Solver *solv, Id v);
extern void solver_printwatches(Solver *solv, int type);
extern void solver_printdecisions(Solver *solv);
extern void solver_printprobleminfo(Solver *solv, Queue *job, Id problem);
extern void solver_printsolutions(Solver *solv, Queue *job);

#endif /* SATSOLVER_SOLVERDEBUG_H */
